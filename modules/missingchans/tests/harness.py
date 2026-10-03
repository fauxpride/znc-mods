"""Isolated, real-ZNC integration harness. Python standard library only.

All listeners bind to loopback. Each test creates a unique temporary data
folder, synthetic credentials, and simulated IRC networks. No production
configuration, network, channel, or service is contacted.
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time


def wait_for(predicate, timeout=10, description="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for " + description)


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.lines = []
        self.lock = threading.RLock()
        self.write_lock = threading.Lock()
        self.error = None
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def send(self, line):
        with self.write_lock:
            self.sock.sendall((line + "\r\n").encode())

    def read(self):
        buf = b""
        try:
            while True:
                data = self.sock.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").rstrip("\r")
                    with self.lock:
                        self.lines.append((time.monotonic(), line))
                    self.handle(line)
        except OSError:
            pass
        except Exception as exc:
            self.error = repr(exc)

    def handle(self, line):
        pass

    def texts(self):
        with self.lock:
            return [line for _, line in self.lines]

    def count(self, fragment):
        return sum(fragment in line for line in self.texts())

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.thread.join(timeout=2)


class IRCd(Peer):
    def __init__(self, listener, name, denied=()):
        self.listener = listener
        self.port = listener.getsockname()[1]
        self.name = name
        self.denied = set(denied)
        self.joined = set()
        self.omitted = set()
        self.whois_override = None
        self.hold_whois = False
        self.pending_whois = []
        self.whois_error = False
        self.whois_end_on_error = True
        self.on_line = None
        self.nick = "tester"
        self.ready = threading.Event()
        self.accept_thread = threading.Thread(target=self.accept, daemon=True)
        self.accept_thread.start()

    def accept(self):
        try:
            sock, _ = self.listener.accept()
            Peer.__init__(self, sock)
            self.ready.set()
        except OSError:
            pass

    def numeric(self, code, text):
        self.send(f":{self.name}.test {code} {self.nick} {text}")

    def handle(self, line):
        if self.on_line:
            self.on_line(line)
        parts = line.split()
        if not parts:
            return
        cmd = parts[0].upper()
        if cmd == "CAP" and parts[1] == "LS":
            self.send(f":{self.name}.test CAP * LS :")
        elif cmd == "NICK":
            old = self.nick
            self.nick = parts[1]
            if self.joined:
                self.send(f":{old}!u@host.test NICK :{self.nick}")
        elif cmd == "USER":
            for code, text in [("001", ":Welcome"), ("004", f"{self.name}.test fake io nt"),
                               ("005", "CHANTYPES=#&+! PREFIX=(Yqaohv)!~&@%+ CASEMAPPING=rfc1459 :supported"),
                               ("422", ":No MOTD")]:
                self.numeric(code, text)
        elif cmd == "PING":
            self.send(f":{self.name}.test PONG :{parts[-1].lstrip(':')}")
        elif cmd == "JOIN":
            for chan in parts[1].split(','):
                if chan in self.denied:
                    self.numeric("473", f"{chan} :Invite only")
                elif chan not in self.joined:
                    self.join(chan)
                # IRCds normally silently ignore a duplicate JOIN.
        elif cmd == "PART":
            self.part(parts[1])
        elif cmd == "MODE" and len(parts) == 2 and parts[1].startswith('#'):
            self.numeric("324", f"{parts[1]} +nt")
        elif cmd == "WHOIS":
            targets = parts[-1].lstrip(':').split(',')
            for target in targets:
                if self.hold_whois:
                    self.pending_whois.append(target)
                else:
                    self.reply_whois(target)

    def join_count(self, chan):
        return sum(line.startswith('JOIN ') and chan in line.split()[1].split(',')
                   for line in self.texts())

    def join(self, chan):
        self.joined.add(chan)
        self.send(f":{self.nick}!u@host.test JOIN :{chan}")
        self.numeric("353", f"= {chan} :{self.nick} other")
        self.numeric("366", f"{chan} :End of NAMES")

    def part(self, chan):
        self.joined.discard(chan)
        self.send(f":{self.nick}!u@host.test PART {chan} :Leaving")

    def kick(self, chan, nick=None):
        target = nick or self.nick
        if target == self.nick:
            self.joined.discard(chan)
        self.send(f":operator!u@host.test KICK {chan} {target} :Test")

    def reply_whois(self, target):
        if self.whois_error or target != self.nick:
            self.numeric("401", f"{target} :No such nick")
            if not self.whois_end_on_error:
                return
        else:
            self.numeric("311", f"{target} u host.test * :Test user")
            chans = self.whois_override
            if chans is None:
                chans = sorted(self.joined - self.omitted)
            for chunk in [chans[i:i+3] for i in range(0, len(chans), 3)]:
                self.numeric("319", f"{target} :{' '.join(chunk)}")
        self.numeric("318", f"{target} :End of WHOIS")

    def release_whois(self):
        pending, self.pending_whois = self.pending_whois, []
        self.hold_whois = False
        for target in pending:
            self.reply_whois(target)

    def sync(self):
        token = f"server-sync-{time.monotonic_ns()}"
        self.send("PING :" + token)
        wait_for(lambda: self.count(token), description="server barrier")

    def close(self):
        self.listener.close()
        if self.ready.is_set():
            super().close()
        self.accept_thread.join(timeout=2)


class Client(Peer):
    def __init__(self, port, network):
        super().__init__(socket.create_connection(("127.0.0.1", port), timeout=10))
        self.sock.settimeout(None)
        self.send(f"PASS tester/{network}:test-password")
        self.send("NICK tester")
        self.send("USER tester 0 * :Test")
        wait_for(lambda: self.count(" 001 "), description="client login")
        self.sync()

    def handle(self, line):
        if line.startswith("PING "):
            self.send("PONG " + line.split(' ', 1)[1])

    def sync(self):
        token = f"client-sync-{time.monotonic_ns()}"
        self.send("PING :" + token)
        wait_for(lambda: self.count(token), description="client barrier")

    def command(self, command, module="missingchans"):
        start = len(self.texts())
        self.send(f"PRIVMSG *{module} :{command}")
        self.sync()
        return '\n'.join(self.texts()[start:])


def listener(port=0):
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    sock.listen(4)
    return sock


class ZNC:
    def __init__(self, module, name, channels=("#gate",), denied=(), network_perform=True,
                 user_perform=False, extra_modules=(), chan_options=None, first_modules=()):
        root = os.environ.get("MC_TEST_ROOT")
        if root:
            Path(root).mkdir(parents=True, exist_ok=True)
        self.path = Path(tempfile.mkdtemp(prefix=name+'-', dir=root))
        self.clients = []
        self.proc = None
        self.servers = {n: IRCd(listener(), n, denied if n == 'a' else ()) for n in ('a','b')}
        for sub in ('configs', 'modules'):
            (self.path/sub).mkdir()
        shutil.copyfile(module, self.path/'modules/missingchans.so')
        probe = listener()
        self.port = probe.getsockname()[1]
        probe.close()
        config = f'''Version = 1.10.3
ConnectDelay = 1
ServerThrottle = 0
<Listener test>
 Host = 127.0.0.1
 Port = {self.port}
 IPv4 = true
 IPv6 = false
 SSL = false
</Listener>
<User tester>
 Admin = true
 Nick = tester
 Ident = tester
 RealName = Test
 MaxNetworks = 5
 JoinTries = 1
 <Pass password>
  Method = plain
  Hash = test-password
 </Pass>
'''
        if user_perform:
            config += " LoadModule = perform\n"
        for n, server in self.servers.items():
            config += f" <Network {n}>\n  Server = 127.0.0.1 {server.port}\n  FloodBurst = 1000\n  FloodRate = 0.1\n"
            # Load order matters for which shared object other modules bind to.
            for mod in first_modules:
                config += f"  LoadModule = {mod}\n"
            config += "  LoadModule = missingchans\n"
            if network_perform and n == 'a':
                config += "  LoadModule = perform\n"
            for mod in extra_modules:
                config += f"  LoadModule = {mod}\n"
            for chan in (channels if n == 'a' else ('#observer',)):
                options = (chan_options or {}).get(chan, '')
                config += f"  <Chan {chan}>\n{options}  </Chan>\n"
            config += " </Network>\n"
        config += "</User>\n"
        (self.path/'configs/znc.conf').write_text(config)

    def __enter__(self):
        print('  logs: ' + str(self.path), flush=True)
        self.log = (self.path/'znc.log').open('w')
        binary = os.environ.get('ZNC_BIN', 'znc')
        args = [binary, '--foreground', '--debug', '--datadir', str(self.path)]
        identity = {}
        if os.geteuid() == 0:
            # Drop privileges for the child only; never modify system accounts.
            for path in [self.path, *self.path.rglob('*')]:
                os.chown(path, 65534, 65534)
            identity = dict(user=65534, group=65534, extra_groups=[])

        env = dict(os.environ)
        if env.get('ZNC_PRELOAD'):
            env['LD_PRELOAD'] = env['ZNC_PRELOAD']
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT, env=env, **identity)
        try:
            for s in self.servers.values():
                wait_for(s.ready.is_set, description="ZNC IRC connection")
                wait_for(lambda: s.count("USER "), description="IRC registration")
                s.sync()
            self.a = self.client('a')
            self.b = self.client('b')
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def restart(self):
        """Restart the test process against the same datadir and installed .so."""
        for client in self.clients:
            client.close()
        self.clients = []
        self.proc.terminate()
        self.proc.wait(timeout=5)
        self.log.close()
        (self.path/'znc.log').rename(self.path/'znc-before-restart.log')
        old_servers = self.servers
        self.servers = {}
        for name, server in old_servers.items():
            server.close()
            (self.path/f'irc-{name}-before-restart.log').write_text('\n'.join(server.texts()))
            self.servers[name] = IRCd(listener(server.port), name, server.denied)
        self.__enter__()

    def install(self, module):
        """Replace the module file the way the README instructs: copy to a
        sibling, then rename over the installed file (new inode)."""
        target = self.path/'modules/missingchans.so'
        staging = self.path/'modules/missingchans.so.new'
        shutil.copyfile(module, staging)
        if os.geteuid() == 0:
            os.chown(staging, 65534, 65534)
        os.replace(staging, target)

    def mapped_builds(self):
        """Distinct missingchans files currently mapped into the ZNC process,
        as (inode, path) pairs. Private loads appear under their unlinked
        temporary names."""
        found = set()
        with open(f'/proc/{self.proc.pid}/maps') as maps:
            for line in maps:
                fields = line.split(None, 5)
                if len(fields) == 6 and 'missingchans' in fields[5]:
                    found.add((fields[4], fields[5].strip().replace(' (deleted)', '')))
        return found

    def leftover_links(self):
        return sorted(p.name for p in (self.path/'modules').iterdir() if p.name.startswith('.missingchans-load-'))

    def client(self, network):
        client = Client(self.port, network)
        self.clients.append(client)
        return client

    def setup(self, **settings):
        defaults = dict(joinmissing='on', retryperform='on', retries=3, retrystep=1)
        defaults.update(settings)
        for key, value in defaults.items():
            out = self.a.command(f"SET {key} {value}")
            assert "OK." in out, out

    def run(self):
        self.a.command('RUN')
        wait_for(lambda: self.servers['a'].count('WHOIS '), description='verification request')

    def __exit__(self, exc_type, exc, tb):
        for client in self.clients:
            client.close()
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.log.close()
        errors = []
        for name, server in self.servers.items():
            if server.ready.is_set():
                (self.path/f'irc-{name}.log').write_text('\n'.join(server.texts())+'\n')
                if server.error:
                    errors.append(server.error)
            server.close()
        for i, client in enumerate(self.clients):
            (self.path/f'client-{i}.log').write_text('\n'.join(client.texts())+'\n')
            if client.error:
                errors.append(client.error)
        if os.geteuid() == 0:
            for path in [self.path, *self.path.rglob('*')]:
                os.chown(path, 0, 0)
        log = (self.path/'znc.log').read_text(errors='replace')
        if any(marker in log for marker in ('ERROR: AddressSanitizer', 'runtime error:', 'LeakSanitizer', 'AddressSanitizer:DEADLYSIGNAL')):
            errors.append('sanitizer diagnostic')
        if self.proc and self.proc.returncode not in (0, -15):
            errors.append(f'ZNC exited {self.proc.returncode}')
        if errors and exc_type is None:
            raise AssertionError(f'{errors}; logs: {self.path}')
