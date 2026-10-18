"""Live-ZNC test harness for ignore_drop.

Derived from the highlightctx harness (modules/highlightctx/tests/hx.py) and
kept as a separate copy so that each module's tests are self-contained.

Components:
  FakeIRCd  - single-connection IRC server for one ZNC network. Lets a test
              choose the sender's full nick!ident@host for every line, and
              send channel or private PRIVMSG, NOTICE, ACTION and CTCP.
  Znc       - writes a znc.conf with one user and any number of networks,
              each with its own FakeIRCd; places LoadModule lines at user or
              network level; seeds NV registry files; installs module .so
              files by atomic rename; runs `znc --foreground`; supports
              graceful stop, SIGKILL and restart against the same datadir.
  Client    - raw IRC client logged in to one network. Every request is
              followed by a PING to ZNC, whose PONG marks the point where all
              output caused by the request has been received.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time

BASE_TS = 1757577600  # 2025-09-11T08:00:00Z, arbitrary fixed epoch
MYNICK = "tim"


def iso(ts):
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(ts))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def nv_encode(v):
    """ZNC's MCString::Encode: every byte that is not alphanumeric becomes %xx;."""
    out = []
    for b in v.encode("utf-8"):
        c = chr(b)
        if c.isascii() and c.isalnum():
            out.append(c)
        else:
            out.append("%%%02x;" % b)
    return "".join(out)


def nv_decode(v):
    return re.sub(r"%([0-9a-fA-F]{1,2});", lambda m: chr(int(m.group(1), 16)), v)


class FakeIRCd:
    def __init__(self, port, nick=MYNICK, names=("alice", "bob")):
        self.port = port
        self.nick = nick
        self.names = list(names)
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.srv.settimeout(0.1)
        self.conn = None
        self.lock = threading.Lock()
        self.pongs = set()
        self.joined = set()
        self.alive = True
        self.connected = threading.Event()
        self.ts = BASE_TS
        self.accepts = 0
        self.received = []          # every line ZNC sent us
        self.accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.accept_thread.start()

    def _accept_loop(self):
        while self.alive:
            try:
                c, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            c.settimeout(None)
            if not self.alive:
                c.close()
                continue
            with self.lock:
                self.accepts += 1
                self.conn = c
                self.joined = set()
            self.connected.set()
            threading.Thread(target=self._reader, args=(c,), daemon=True).start()

    def raw(self, line):
        with self.lock:
            c = self.conn
        if c is None:
            raise RuntimeError("no ZNC connection")
        c.sendall((line + "\r\n").encode("utf-8", "surrogateescape"))

    def _reader(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(65536)
            except OSError:
                break
            if not d:
                break
            buf += d
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                self._handle(ln.decode("utf-8", "replace").rstrip("\r"))
        with self.lock:
            if self.conn is c:
                self.conn = None
        self.connected.clear()

    def _handle(self, ln):
        self.received.append(ln)
        parts = ln.split(" ")
        cmd = parts[0].upper()
        if cmd == "CAP":
            sub = parts[1].upper() if len(parts) > 1 else ""
            if sub == "LS":
                self.raw(":irc.test CAP * LS :server-time")
            elif sub == "REQ":
                caps = ln.split(":", 1)[1] if ":" in ln else parts[2]
                self.raw(":irc.test CAP * ACK :" + caps)
        elif cmd == "USER":
            self.raw(":irc.test 001 %s :Welcome" % self.nick)
            self.raw(":irc.test 002 %s :Host" % self.nick)
            self.raw(":irc.test 003 %s :Created" % self.nick)
            self.raw(":irc.test 004 %s irc.test fake-1 io ntk" % self.nick)
            self.raw(":irc.test 005 %s CHANTYPES=# PREFIX=(ov)@+ CASEMAPPING=rfc1459 :are supported" % self.nick)
            self.raw(":irc.test 422 %s :No MOTD" % self.nick)
        elif cmd == "PING":
            self.raw(":irc.test PONG irc.test :" + ln.split(" ", 1)[1].lstrip(":"))
        elif cmd == "PONG":
            tok = ln.rsplit(":", 1)[-1] if ":" in ln else parts[-1]
            with self.lock:
                self.pongs.add(tok)
        elif cmd == "JOIN":
            for ch in parts[1].split(","):
                ch = ch.strip()
                if not ch:
                    continue
                self.raw(":%s!tim@me.test JOIN %s" % (self.nick, ch))
                self.raw(":irc.test 353 %s = %s :%s %s" % (self.nick, ch, self.nick, " ".join(self.names)))
                self.raw(":irc.test 366 %s %s :End of NAMES" % (self.nick, ch))
                with self.lock:
                    self.joined.add(ch.lower())

    def wait_ready(self, chans, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if self.conn is not None and all(c.lower() in self.joined for c in chans):
                    break
            time.sleep(0.05)
        else:
            raise RuntimeError("ZNC did not connect/join in time: joined=%s accepts=%d" % (self.joined, self.accepts))
        self.sync()

    def sync(self, timeout=10):
        """Return once ZNC has processed everything sent before this call."""
        tok = "sync%d" % int(time.time() * 1e6)
        self.raw("PING :" + tok)
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if tok in self.pongs:
                    return
            time.sleep(0.01)
        raise RuntimeError("sync timeout")

    def say(self, target, who, text, kind="T", ts=None):
        """Send a message from `who` to `target` (a channel, or our nick for a
        private message). `who` is a nick, or a full nick!ident@host. Kinds:
        T = PRIVMSG, N = NOTICE, A = CTCP ACTION, C = other CTCP request
        (text is the CTCP command, e.g. "VERSION")."""
        if ts is None:
            self.ts += 1
            ts = self.ts
        if "!" not in who:
            who = "%s!%s@%s.host" % (who, who.lower(), who.lower())
        pfx = "@time=%s :%s" % (iso(ts), who)
        if kind == "T":
            self.raw("%s PRIVMSG %s :%s" % (pfx, target, text))
        elif kind == "N":
            self.raw("%s NOTICE %s :%s" % (pfx, target, text))
        elif kind == "A":
            self.raw("%s PRIVMSG %s :\x01ACTION %s\x01" % (pfx, target, text))
        elif kind == "C":
            self.raw("%s PRIVMSG %s :\x01%s\x01" % (pfx, target, text))
        else:
            raise ValueError(kind)
        return ts

    def close(self):
        self.alive = False
        self.accept_thread.join(timeout=5)
        try:
            self.srv.close()
        except OSError:
            pass
        with self.lock:
            if self.conn:
                try:
                    self.conn.close()
                except OSError:
                    pass


class Network:
    def __init__(self, name, chans=("#a",), load_lines=(), names=("alice", "bob")):
        self.name = name
        self.chans = list(chans)
        self.load_lines = list(load_lines)
        self.port = free_port()
        self.ircd = FakeIRCd(self.port, names=names)


class Znc:
    """One ZNC instance with user `tim` and the given networks.

    user_load_lines - LoadModule lines placed in the <User> section.
    user_settings   - extra "Key = Value" lines for the <User> section.
    registries      - {"user" or network name: {"<module>": {key: value}}},
                      written to the matching moddata/<module>/.registry
                      before ZNC starts.
    global_load_lines - LoadModule lines at the top level of znc.conf.
    """

    def __init__(self, workdir, znc_bin, modules, networks, user_load_lines=(), user_settings=None,
                 registries=None, extra_env=None, global_load_lines=()):
        self.workdir = workdir
        self.znc_bin = znc_bin
        self.networks = list(networks)
        self.user_load_lines = list(user_load_lines)
        self.global_load_lines = list(global_load_lines)
        self.user_settings = dict(user_settings or {})
        self.extra_env = dict(extra_env or {})
        self.proc = None
        self.log = None
        self.listen_port = free_port()
        shutil.rmtree(workdir, ignore_errors=True)
        os.makedirs(os.path.join(workdir, "configs"))
        os.makedirs(os.path.join(workdir, "modules"))
        self.install_modules(modules)
        self.write_config()
        for where, mods in (registries or {}).items():
            for mod, kv in mods.items():
                self.write_registry(where, mod, kv)

    def net(self, name=None):
        if name is None:
            return self.networks[0]
        return next(n for n in self.networks if n.name == name)

    def install_modules(self, modules):
        for name, path in modules.items():
            dst = os.path.join(self.workdir, "modules", name + ".so")
            tmp = dst + ".tmp"
            shutil.copy(path, tmp)
            os.rename(tmp, dst)  # atomic replace, never overwrite a mapped .so in place

    def write_config(self):
        net_blocks = ""
        for n in self.networks:
            mods = "".join("            LoadModule = %s\n" % l for l in n.load_lines)
            chans = "".join("            <Chan %s>\n            </Chan>\n" % c for c in n.chans)
            net_blocks += "    <Network %s>\n        Server = 127.0.0.1 %d\n%s%s    </Network>\n" % (n.name, n.port, mods, chans)
        umods = "".join("    LoadModule = %s\n" % l for l in self.user_load_lines)
        uset = "".join("    %s = %s\n" % kv for kv in self.user_settings.items())
        gmods = "".join("LoadModule = %s\n" % l for l in self.global_load_lines)
        # ConnectDelay and ServerThrottle default to 5 s and 30 s; with them,
        # a second network on 127.0.0.1 would not connect within the wait.
        cfg = """Version = 1.10.3
ConnectDelay = 0
ServerThrottle = 0
{gmods}<Listener l>
    Port = {lp}
    IPv4 = true
    IPv6 = false
    SSL = false
</Listener>
<User tim>
    <Pass password>
        Method = plain
        Hash = pass
    </Pass>
    Admin = true
    Nick = tim
    AltNick = tim_
    Ident = tim
    RealName = tim
    MaxNetworks = 10
{uset}{umods}{nets}</User>
""".format(lp=self.listen_port, uset=uset, umods=umods, nets=net_blocks, gmods=gmods)
        with open(self.config_path(), "w") as f:
            f.write(cfg)

    def config_path(self):
        return os.path.join(self.workdir, "configs", "znc.conf")

    def config_text(self):
        with open(self.config_path()) as f:
            return f.read()

    def registry_path(self, where, mod):
        if where == "user":
            base = os.path.join(self.workdir, "users", "tim")
        else:
            base = os.path.join(self.workdir, "users", "tim", "networks", where)
        return os.path.join(base, "moddata", mod, ".registry")

    def write_registry(self, where, mod, kv):
        p = self.registry_path(where, mod)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            for k, v in kv.items():
                f.write("%s %s\n" % (nv_encode(k), nv_encode(v)))

    def registry(self, where, mod="ignore_drop"):
        """The module's NV store as ZNC last wrote it, or None if absent."""
        p = self.registry_path(where, mod)
        if not os.path.exists(p):
            return None
        out = {}
        with open(p) as f:
            for ln in f:
                ln = ln.rstrip("\n")
                if not ln:
                    continue
                k, _, v = ln.partition(" ")
                out[nv_decode(k)] = nv_decode(v)
        return out

    def start(self, expect_ready=True, timeout=20):
        subprocess.run(["chown", "-R", "znctest:znctest", self.workdir], check=True)
        env = dict(os.environ)
        env["HOME"] = "/home/znctest"
        env.update(self.extra_env)
        self.log = open(os.path.join(self.workdir, "znc.out"), "a")
        self.proc = subprocess.Popen(
            ["setpriv", "--reuid=znctest", "--regid=znctest", "--init-groups",
             self.znc_bin, "--foreground", "--debug", "--datadir", self.workdir],
            stdout=self.log, stderr=subprocess.STDOUT, env=env)
        if not expect_ready:
            return
        for n in self.networks:
            n.ircd.wait_ready(n.chans, timeout=timeout)

    def wait_exit(self, timeout=15):
        """For a start expected to fail: the exit code, or None if still running."""
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self, kill=False):
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None
        self.log.close()
        for n in self.networks:
            end = time.time() + 5
            while n.ircd.connected.is_set() and time.time() < end:
                time.sleep(0.02)

    def close(self):
        try:
            self.stop()
        finally:
            for n in self.networks:
                n.ircd.close()

    def output(self):
        with open(os.path.join(self.workdir, "znc.out"), errors="replace") as f:
            return f.read()

    def client(self, net=None):
        return Client(self.listen_port, self.net(net).name)


# A line ZNC delivers to the client: optional tags, :prefix, command, params.
LINE = re.compile(r"^(?:@(?P<tags>\S+) )?:(?P<src>\S+) (?P<cmd>\S+) (?P<target>\S+)(?: :?(?P<text>.*))?$")


class Client:
    _seq = 0

    def __init__(self, port, network):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""
        self.lines = []
        self.mark = 0
        self.send("PASS tim/%s:pass" % network)
        self.send("NICK tim")
        self.send("USER tim 0 * :tim")
        self._wait(lambda l: " 001 " in l)
        # Everything ZNC sends while attaching (JOINs, buffer playback) ends
        # before it processes our next command, so a ping here marks the end
        # of the attach sequence.
        self.attach_lines = self.sync(keep_prior=True)

    def send(self, line):
        self.sock.sendall((line + "\r\n").encode("utf-8", "surrogateescape"))

    def _readline(self):
        while b"\n" not in self.buf:
            d = self.sock.recv(65536)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        ln, self.buf = self.buf.split(b"\n", 1)
        ln = ln.decode("utf-8", "replace").rstrip("\r")
        if ln.startswith("PING "):
            self.send("PONG " + ln[5:])
        self.lines.append(ln)
        return ln

    def _wait(self, pred):
        while True:
            ln = self._readline()
            if pred(ln):
                return ln

    def sync(self, keep_prior=False):
        """Return the lines received since the previous sync (or since login
        with keep_prior), once ZNC has answered a PING sent now."""
        Client._seq += 1
        tok = "hx%d" % Client._seq
        start = 0 if keep_prior else self.mark
        self.send("PING :" + tok)
        self._wait(lambda l: " PONG " in l and l.endswith(tok))
        out = self.lines[start:-1]
        self.mark = len(self.lines)
        return out

    def cmd(self, target, text):
        """Send PRIVMSG to a module (e.g. *ignore_drop, *status) and return the
        text of each reply line it sent back."""
        self.sync()
        self.send("PRIVMSG %s :%s" % (target, text))
        got = self.sync()
        out = []
        for l in got:
            m = LINE.match(l)
            if m and m.group("src").split("!")[0] == target and m.group("cmd") in ("PRIVMSG", "NOTICE"):
                out.append(m.group("text") or "")
        return out

    def quit(self, wait=0.6):
        try:
            self.send("QUIT :bye")
            time.sleep(0.1)
            self.sock.close()
        except OSError:
            pass
        time.sleep(wait)  # let ZNC process the detach before more traffic arrives


def deliveries(lines):
    """Messages from IRC users among `lines`, as (src_nick, cmd, target, text).
    Module and *status output is left out."""
    out = []
    for l in lines:
        m = LINE.match(l)
        if not m or m.group("cmd") not in ("PRIVMSG", "NOTICE"):
            continue
        nick = m.group("src").split("!")[0]
        if nick.startswith("*") or "." in nick:
            continue
        out.append((nick, m.group("cmd"), m.group("target"), m.group("text") or ""))
    return out


def tokens(lines):
    """The marker word (first token matching [A-Z][0-9]+) of each delivered
    message, in order. Scenario traffic carries markers like M1, A2."""
    out = []
    for _, _, _, text in deliveries(lines):
        m = re.search(r"\b([A-Z]{1,3}\d+)\b", text)
        if m:
            out.append(m.group(1))
    return out
