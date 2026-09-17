"""Live-ZNC test harness for highlightctx.

Components:
  FakeIRCd  - single-connection IRC server that ZNC's network connects to.
              Advertises server-time so injected lines carry deterministic
              @time tags (the module stores Message.GetTime()).
  Znc       - writes a znc.conf into a scratch datadir, installs module .so
              files, runs `znc --foreground`, supports graceful stop, SIGKILL,
              and restart against the same datadir.
  Client    - raw IRC client that logs in to ZNC (optionally with server-time),
              captures the *highlightctx replay emitted on attach, and can run
              module commands.
  parse_replay - splits replay output into structured events.
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


class FakeIRCd:
    def __init__(self, port):
        self.port = port
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.srv.settimeout(0.1)
        self.conn = None
        self.buf = b""
        self.lock = threading.Lock()
        self.pongs = set()
        self.joined = set()
        self.registered = threading.Event()
        self.capreq = []
        self.alive = True
        self.connected = threading.Event()
        self.ts = BASE_TS
        self.accepts = 0
        self.evlog = []
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
            if not self.alive or c.getsockname()[1] != self.port:
                c.close()
                continue
            self.evlog.append((time.time(), "accept", c.getpeername()))
            with self.lock:
                self.accepts += 1
                self.conn = c
                self.buf = b""
                self.joined = set()
                self.registered.clear()
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
        self.evlog.append((time.time(), "eof", id(c), self.conn is c))
        with self.lock:
            if self.conn is c:
                self.conn = None
        self.connected.clear()

    def _handle(self, ln):
        parts = ln.split(" ")
        cmd = parts[0].upper()
        if cmd == "CAP":
            sub = parts[1].upper() if len(parts) > 1 else ""
            if sub == "LS":
                self.raw(":irc.test CAP * LS :server-time")
            elif sub == "REQ":
                caps = ln.split(":", 1)[1] if ":" in ln else parts[2]
                self.capreq.append(caps)
                self.raw(":irc.test CAP * ACK :" + caps)
        elif cmd == "NICK":
            pass
        elif cmd == "USER":
            self.raw(":irc.test 001 %s :Welcome" % MYNICK)
            self.raw(":irc.test 002 %s :Host" % MYNICK)
            self.raw(":irc.test 003 %s :Created" % MYNICK)
            self.raw(":irc.test 004 %s irc.test fake-1 io ntk" % MYNICK)
            self.raw(":irc.test 005 %s CHANTYPES=# PREFIX=(ov)@+ CASEMAPPING=rfc1459 :are supported" % MYNICK)
            self.raw(":irc.test 422 %s :No MOTD" % MYNICK)
            self.registered.set()
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
                self.raw(":%s!tim@me.test JOIN %s" % (MYNICK, ch))
                self.raw(":irc.test 353 %s = %s :%s alice bob" % (MYNICK, ch, MYNICK))
                self.raw(":irc.test 366 %s %s :End of NAMES" % (MYNICK, ch))
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
            print("EVLOG", self.evlog, "now", time.time())
            raise RuntimeError("ZNC did not connect/join in time: joined=%s conn=%r connected=%s accepts=%d" % (self.joined, self.conn, self.connected.is_set(), self.accepts))
        self.sync()

    def sync(self, timeout=10):
        tok = "sync%d" % int(time.time() * 1e6)
        self.raw("PING :" + tok)
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if tok in self.pongs:
                    return
            time.sleep(0.01)
        raise RuntimeError("sync timeout")

    def say(self, chan, nick, text, kind="T", ts=None):
        if ts is None:
            self.ts += 1
            ts = self.ts
        pfx = "@time=%s :%s!%s@%s.host" % (iso(ts), nick, nick.lower(), nick.lower())
        if kind == "T":
            self.raw("%s PRIVMSG %s :%s" % (pfx, chan, text))
        elif kind == "N":
            self.raw("%s NOTICE %s :%s" % (pfx, chan, text))
        elif kind == "A":
            self.raw("%s PRIVMSG %s :\x01ACTION %s\x01" % (pfx, chan, text))
        return ts

    def close(self):
        self.alive = False
        # Stop the accept thread BEFORE releasing the fd, so a stale thread can
        # never call accept() on a recycled descriptor owned by a newer server.
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


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Znc:
    def __init__(self, workdir, znc_bin, modules, load_lines, chans=("#a", "#b"), extra_env=None):
        self.workdir = workdir
        self.znc_bin = znc_bin
        self.modules = modules          # dict name -> .so path
        self.load_lines = load_lines    # list of "name args"
        self.chans = list(chans)
        self.extra_env = extra_env or {}
        self.proc = None
        self.listen_port = free_port()
        self.irc_port = free_port()
        self.ircd = FakeIRCd(self.irc_port)
        shutil.rmtree(workdir, ignore_errors=True)
        os.makedirs(os.path.join(workdir, "configs"))
        os.makedirs(os.path.join(workdir, "modules"))
        self.install_modules(modules)
        self.write_config()

    def install_modules(self, modules):
        self.modules = modules
        for name, path in modules.items():
            dst = os.path.join(self.workdir, "modules", name + ".so")
            tmp = dst + ".tmp"
            shutil.copy(path, tmp)
            os.rename(tmp, dst)  # atomic replace, never overwrite a mapped .so in place

    def write_config(self):
        chan_blocks = "".join("            <Chan %s>\n            </Chan>\n" % c for c in self.chans)
        mods = "".join("            LoadModule = %s\n" % l for l in self.load_lines)
        cfg = """Version = 1.9.0
<Listener l>
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
    MaxNetworks = 5
    <Network test>
        Server = 127.0.0.1 {ip}
{mods}{chans}    </Network>
</User>
""".format(lp=self.listen_port, ip=self.irc_port, mods=mods, chans=chan_blocks)
        with open(os.path.join(self.workdir, "configs", "znc.conf"), "w") as f:
            f.write(cfg)

    def start(self):
        subprocess.run(["chown", "-R", "znctest:znctest", self.workdir], check=True)
        env = dict(os.environ)
        env["HOME"] = "/home/znctest"
        env.update(self.extra_env)
        self.log = open(os.path.join(self.workdir, "znc.out"), "a")
        self.proc = subprocess.Popen(
            ["setpriv", "--reuid=znctest", "--regid=znctest", "--init-groups",
             self.znc_bin, "--foreground", "--debug", "--datadir", self.workdir],
            stdout=self.log, stderr=subprocess.STDOUT, env=env)
        self.ircd.wait_ready(self.chans)

    def stop(self, kill=False):
        if not self.proc:
            return
        self.ircd.evlog.append((time.time(), "stop", kill))
        if kill:
            self.proc.send_signal(signal.SIGKILL)
        else:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc = None
        self.log.close()
        # wait for the fake server to notice the drop
        end = time.time() + 5
        while self.ircd.connected.is_set() and time.time() < end:
            time.sleep(0.02)

    def close(self):
        try:
            self.stop()
        finally:
            self.ircd.close()

    def journal_path(self):
        return os.path.join(self.workdir, "users", "tim", "networks", "test", "moddata", "highlightctx", "highlightctx.journal")

    def journal_lines(self):
        try:
            with open(self.journal_path()) as f:
                return [l.rstrip("\n") for l in f if l.strip()]
        except FileNotFoundError:
            return []

    def output(self):
        with open(os.path.join(self.workdir, "znc.out"), errors="replace") as f:
            return f.read()

    def client(self, server_time=True):
        return Client(self.listen_port, server_time)


class Client:
    def __init__(self, port, server_time=True):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""
        self.lines = []
        self.server_time = server_time
        if server_time:
            self.send("CAP LS 302")
        self.send("PASS tim/test:pass")
        self.send("NICK tim")
        self.send("USER tim 0 * :tim")
        if server_time:
            self._wait(lambda l: " CAP " in l and " LS " in l)
            self.send("CAP REQ :server-time")
            self._wait(lambda l: " CAP " in l and (" ACK " in l or " NAK " in l))
            self.send("CAP END")
        self._wait(lambda l: " 001 " in l)
        # Replay happens in OnClientAttached during login. A command sent now is
        # processed afterwards, so its reply marks the end of the replay.
        self.attach_lines = self.command("Version", until="Version: highlightctx", keep_prior=True)

    def send(self, line):
        self.sock.sendall((line + "\r\n").encode())

    def _readline(self):
        while b"\n" not in self.buf:
            d = self.sock.recv(65536)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        ln, self.buf = self.buf.split(b"\n", 1)
        ln = ln.decode("utf-8", "replace").rstrip("\r")
        if ln.startswith("PING") or " PING " in ln.split(" :")[0]:
            self.send("PONG :" + ln.rsplit(":", 1)[-1])
        self.lines.append(ln)
        return ln

    def _wait(self, pred):
        while True:
            ln = self._readline()
            if pred(ln):
                return ln

    def command(self, cmd, until=None, keep_prior=False, settle=0.3):
        """Send a *highlightctx command. Returns module output bodies.

        Without `until`, a Version command is sent afterwards as a sentinel."""
        start = 0 if keep_prior else len(self.lines)
        self.send("PRIVMSG *highlightctx :" + cmd)
        if until is None and cmd.strip().lower() == "version":
            until = "Version: highlightctx"
        if until is None:
            self.send("PRIVMSG *highlightctx :Version")
            until = "Version: highlightctx"
        self._wait(lambda l: "*highlightctx!" in l and until in l)
        return [module_body(l) for l in self.lines[start:] if "*highlightctx!" in l and " PRIVMSG " in l]

    def status_cmd(self, target, cmd):
        start = len(self.lines)
        self.send("PRIVMSG %s :%s" % (target, cmd))
        self.send("PRIVMSG *highlightctx :Version")
        self._wait(lambda l: "*highlightctx!" in l and "Version: highlightctx" in l)
        return [module_body(l) for l in self.lines[start:] if " PRIVMSG " in l]

    def replay_raw(self):
        return [l for l in self.attach_lines]

    def quit(self, wait=0.6):
        try:
            self.send("QUIT :bye")
            time.sleep(0.1)
            self.sock.close()
        except OSError:
            pass
        time.sleep(wait)  # let ZNC process the detach before more traffic arrives


def module_body(raw):
    """Return (tags_time, body) body text of a PRIVMSG from *highlightctx."""
    tags = ""
    rest = raw
    if rest.startswith("@"):
        tags, rest = rest.split(" ", 1)
    body = rest.split(" :", 1)[1] if " :" in rest else ""
    t = None
    m = re.search(r"(?:^@|;)time=([^; ]+)", tags)
    if m:
        t = m.group(1)
    return (t, body)


HDR = re.compile(r"^\[(?P<chan>[^\]]+)\] highlight event #(?P<id>\d+) \((?P<state>complete|partial), before=(?P<before>\d+), after=(?P<after>\d+)/(?P<target>\d+)(?:, triggers=(?P<triggers>\d+))?(?P<capped>, capped)?\)$")
INLINE_TS = re.compile(r"^\[\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.000Z\] ")


def parse_replay(bodies):
    """bodies: list of (time, body) from module output. Returns list of events."""
    events = []
    cur = None
    for t, body in bodies:
        b = body
        inline = None
        if INLINE_TS.match(b):
            inline = b[1:25]
            b = b[27:]
        m = HDR.match(b)
        if m:
            cur = dict(chan=m.group("chan"), id=int(m.group("id")), state=m.group("state"),
                       before=int(m.group("before")), after=int(m.group("after")),
                       target=int(m.group("target")),
                       triggers=int(m.group("triggers")) if m.group("triggers") else None,
                       capped=bool(m.group("capped")),
                       lines=[], raw_header=b)
            events.append(cur)
            continue
        if b.startswith("-----"):
            continue
        if cur is None:
            continue
        pfx = "[%s] " % cur["chan"]
        if not b.startswith(pfx):
            continue
        rest = b[len(pfx):]
        marked = rest.startswith(">>> ")
        if marked:
            rest = rest[4:]
        cur["lines"].append(dict(marked=marked, text=rest, time=t or inline))
    return events


def events_of(client):
    return parse_replay(client.attach_lines)
