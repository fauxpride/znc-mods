"""Live-ZNC test harness for delayedperform.

Derived from the highlightctx and ignore_drop harnesses and kept as a separate
copy so that each module's tests are self-contained. Compared with the
ignore_drop copy it adds server-side nick ownership (433/437), ISON, MONITOR,
PART/KICK echo, connection drops, and a timestamped log of what ZNC sends.

Components:
  FakeIRCd  - single-connection IRC server for one ZNC network. Registers
              ZNC (refusing nicks that other users hold), echoes JOIN and
              PART, answers ISON and, depending on `monitor`, MONITOR. Lets a
              test hand nicks to and take them from simulated other users,
              send channel traffic from a chosen nick!ident@host, kick ZNC,
              and drop the connection so ZNC reconnects. Every line ZNC sends
              is recorded with the time it arrived.
  Znc       - writes a znc.conf with one user and any number of networks,
              each with its own FakeIRCd; places LoadModule lines at global,
              user or network level; seeds NV registry files; installs module
              .so files by atomic rename; runs `znc --foreground`; supports
              graceful stop, SIGKILL and restart against the same datadir.
  Client    - raw IRC client logged in to one network. Every request is
              followed by a PING to ZNC, whose PONG marks the point where all
              output caused by the request has been received. Output that a
              timer produces later is collected with wait_for() or drain().
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
    for b in v.encode("utf-8", "surrogateescape"):
        c = chr(b)
        if c.isascii() and c.isalnum():
            out.append(c)
        else:
            out.append("%%%02x;" % b)
    return "".join(out)


def nv_decode(v):
    return re.sub(r"%([0-9a-fA-F]{1,2});", lambda m: chr(int(m.group(1), 16)), v)


def rfc_lower(s):
    return s.lower().replace("[", "{").replace("]", "}").replace("\\", "|").replace("~", "^")


class FakeIRCd:
    """monitor: None  - MONITOR is not advertised and is answered with 421.
                "ok"  - MONITOR=100 is advertised; + replies 730/731, and
                        monitored nicks get 730/731 as they come and go.
                "full" - advertised, but every + is answered with 734.
                "silent" - advertised, but MONITOR commands get no reply.
    welcome_nick: if set, 001 assigns this nick instead of the one ZNC chose.
    """

    def __init__(self, port, nick=MYNICK, names=("alice", "bob"), monitor=None, isupport=(),
                 welcome_nick=None):
        self.port = port
        self.nick = nick
        self.names = list(names)
        self.monitor = monitor
        self.isupport = list(isupport)
        self.welcome_nick = welcome_nick
        self.online = set()       # folded nicks held by other (simulated) users
        self.refuse = {}          # folded nick -> "433" or "437": refuse NICK to it even if free
        self.monitored = set()    # folded nicks ZNC has MONITOR + 'd
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
        self.received = []        # every line ZNC sent us
        self.rx = []              # (monotonic time, line) for the same lines
        self.welcomes = []        # monotonic time of each 001 we sent
        self._reg = {"nick": None, "user": False, "done": False}
        self.accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.accept_thread.start()

    # ---------------------------------------------------------------- plumbing

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
                self.monitored = set()
                self._reg = {"nick": None, "user": False, "done": False}
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
                try:
                    self._handle(ln.decode("utf-8", "replace").rstrip("\r"))
                except (OSError, RuntimeError):
                    pass
        with self.lock:
            if self.conn is c:
                self.conn = None
        self.connected.clear()

    def me(self):
        return "%s!tim@me.test" % self.nick

    def _taken(self, nick):
        return rfc_lower(nick) in self.online

    def _welcome(self):
        if self.welcome_nick:
            self.nick = self.welcome_nick
        n = self.nick
        tokens = ["CHANTYPES=#", "PREFIX=(ov)@+", "CASEMAPPING=rfc1459"] + self.isupport
        if self.monitor in ("ok", "full", "silent"):
            tokens.append("MONITOR=100")
        # Stamped before 001 goes out: ZNC can answer 001 before we finish
        # sending the rest of the burst.
        self.welcomes.append(time.monotonic())
        self.raw(":irc.test 001 %s :Welcome" % n)
        self.raw(":irc.test 002 %s :Host" % n)
        self.raw(":irc.test 003 %s :Created" % n)
        self.raw(":irc.test 004 %s irc.test fake-1 io ntk" % n)
        self.raw(":irc.test 005 %s %s :are supported" % (n, " ".join(tokens)))
        self.raw(":irc.test 422 %s :No MOTD" % n)

    def _handle(self, ln):
        with self.lock:
            self.received.append(ln)
            self.rx.append((time.monotonic(), ln))
        parts = ln.split(" ")
        cmd = parts[0].upper()
        reg = self._reg
        if not reg["done"] and cmd not in ("CAP", "NICK", "USER", "PASS", "PING", "PONG", "QUIT"):
            # As a real server does for anything sent before registration.
            self.raw(":irc.test 451 * :You have not registered")
            return
        if cmd == "CAP":
            sub = parts[1].upper() if len(parts) > 1 else ""
            if sub == "LS":
                self.raw(":irc.test CAP * LS :server-time")
            elif sub == "REQ":
                caps = ln.split(":", 1)[1] if ":" in ln else parts[2]
                self.raw(":irc.test CAP * ACK :" + caps)
        elif cmd == "NICK" and len(parts) > 1:
            want = parts[1].lstrip(":")
            if not reg["done"]:
                if self._taken(want):
                    self.raw(":irc.test 433 * %s :Nickname is already in use" % want)
                    return
                self.nick = want
                reg["nick"] = want
                if reg["user"]:
                    reg["done"] = True
                    self._welcome()
                return
            why = self.refuse.get(rfc_lower(want))
            if why == "437":
                self.raw(":irc.test 437 %s %s :Nick/channel is temporarily unavailable" % (self.nick, want))
            elif why == "433":
                self.raw(":irc.test 433 %s %s :Nickname is already in use" % (self.nick, want))
            elif self._taken(want) and rfc_lower(want) != rfc_lower(self.nick):
                self.raw(":irc.test 433 %s %s :Nickname is already in use" % (self.nick, want))
            else:
                old = self.me()
                self.nick = want
                self.raw(":%s NICK :%s" % (old, want))
        elif cmd == "USER":
            reg["user"] = True
            if reg["nick"] and not reg["done"]:
                reg["done"] = True
                self._welcome()
        elif cmd == "PING":
            self.raw(":irc.test PONG irc.test :" + ln.split(" ", 1)[1].lstrip(":"))
        elif cmd == "PONG":
            tok = ln.rsplit(":", 1)[-1] if ":" in ln else parts[-1]
            with self.lock:
                self.pongs.add(tok)
        elif cmd == "JOIN" and len(parts) > 1:
            if parts[1] == "0":
                for ch in sorted(self.joined):
                    self.raw(":%s PART %s :Left all channels" % (self.me(), ch))
                with self.lock:
                    self.joined = set()
                return
            for ch in parts[1].split(","):
                ch = ch.strip()
                if not ch:
                    continue
                self.raw(":%s JOIN %s" % (self.me(), ch))
                self.raw(":irc.test 353 %s = %s :%s %s" % (self.nick, ch, self.nick, " ".join(self.names)))
                self.raw(":irc.test 366 %s %s :End of NAMES" % (self.nick, ch))
                with self.lock:
                    self.joined.add(ch.lower())
        elif cmd == "PART" and len(parts) > 1:
            reason = ln.split(" :", 1)[1] if " :" in ln else ""
            for ch in parts[1].split(","):
                if ch.lower() in self.joined:
                    self.raw(":%s PART %s :%s" % (self.me(), ch, reason))
                    with self.lock:
                        self.joined.discard(ch.lower())
        elif cmd == "ISON":
            asked = " ".join(parts[1:]).lstrip(":").split()
            present = [n for n in asked if self._taken(n) or rfc_lower(n) == rfc_lower(self.nick)]
            self.raw(":irc.test 303 %s :%s" % (self.nick, " ".join(present)))
        elif cmd == "MONITOR":
            self._monitor(parts)

    def _monitor(self, parts):
        if self.monitor is None:
            self.raw(":irc.test 421 %s MONITOR :Unknown command" % self.nick)
            return
        if self.monitor == "silent":
            return
        sub = parts[1] if len(parts) > 1 else ""
        targets = [t for t in (parts[2].lstrip(":").split(",") if len(parts) > 2 else []) if t]
        if sub == "+":
            if self.monitor == "full":
                self.raw(":irc.test 734 %s 100 %s :Monitor list is full" % (self.nick, ",".join(targets)))
                return
            # ZNC's own nick is online too, as on a real server.
            live = lambda t: self._taken(t) or rfc_lower(t) == rfc_lower(self.nick)
            on = [t for t in targets if live(t)]
            off = [t for t in targets if not live(t)]
            for t in targets:
                self.monitored.add(rfc_lower(t))
            if on:
                self.raw(":irc.test 730 %s :%s" % (self.nick, ",".join("%s!u@other.host" % t for t in on)))
            if off:
                self.raw(":irc.test 731 %s :%s" % (self.nick, ",".join(off)))
        elif sub == "-":
            for t in targets:
                self.monitored.discard(rfc_lower(t))

    # ---------------------------------------------------- simulated other users

    def take(self, nick):
        """Another user takes `nick`. Monitoring clients are told (730)."""
        self.online.add(rfc_lower(nick))
        if self.conn is not None and self.monitor == "ok" and rfc_lower(nick) in self.monitored:
            self.raw(":irc.test 730 %s :%s!u@other.host" % (self.nick, nick))

    def release(self, nick, visible=None, newnick=None):
        """The user holding `nick` leaves it. visible: None (ZNC shares no
        channel with them), "quit" or "nick" (ZNC sees the QUIT or NICK).
        Monitoring clients are told (731)."""
        self.online.discard(rfc_lower(nick))
        if self.conn is None:
            return
        src = "%s!u@other.host" % nick
        if visible == "quit":
            self.raw(":%s QUIT :Quit: bye" % src)
        elif visible == "nick":
            newnick = newnick or nick + "_away"
            self.online.add(rfc_lower(newnick))
            self.raw(":%s NICK :%s" % (src, newnick))
        if self.monitor == "ok" and rfc_lower(nick) in self.monitored:
            self.raw(":irc.test 731 %s :%s" % (self.nick, nick))

    def note(self, line):
        """Send ZNC an arbitrary line (a raw event or numeric)."""
        self.raw(line)

    def kick(self, chan, by="op", reason="out"):
        self.raw(":%s!%s@%s.host KICK %s %s :%s" % (by, by, by, chan, self.nick, reason))
        with self.lock:
            self.joined.discard(chan.lower())

    def force_part(self, chan, reason="forced"):
        """The server parts ZNC from `chan` on its own (e.g. SAPART)."""
        self.raw(":%s PART %s :%s" % (self.me(), chan, reason))
        with self.lock:
            self.joined.discard(chan.lower())

    def drop(self):
        """Close ZNC's connection; ZNC reconnects on its own."""
        with self.lock:
            c = self.conn
        if c is not None:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.close()
        # Wait for this socket to be gone. Not for `connected` to clear: ZNC
        # reconnects within milliseconds and sets it again.
        end = time.time() + 5
        while c is not None and self.conn is c and time.time() < end:
            time.sleep(0.01)

    # ------------------------------------------------------------- waiting

    def wait_ready(self, chans, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if self.conn is not None and self._reg.get("done") and \
                        all(c.lower() in self.joined for c in chans):
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

    def mark(self):
        """Index into rx; pass to since= to look only at later lines."""
        with self.lock:
            return len(self.rx)

    def lines_since(self, mark, pred=None):
        with self.lock:
            got = list(self.rx[mark:])
        return [(t, l) for t, l in got if pred is None or pred(l)]

    def wait_rx(self, pred, timeout, since=0):
        """First (time, line) at index >= since matching pred, or None."""
        end = time.time() + timeout
        while True:
            with self.lock:
                got = list(self.rx[since:])
            for t, l in got:
                if pred(l):
                    return t, l
            if time.time() >= end:
                return None
            time.sleep(0.02)

    def say(self, target, who, text, kind="T", ts=None):
        """Send a message from `who` to `target`. Kinds: T = PRIVMSG,
        N = NOTICE, A = CTCP ACTION."""
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
    def __init__(self, name, chans=("#a",), load_lines=(), chan_settings=None, settings=None, **ircd_kw):
        """chan_settings: {"#chan": {"Buffer": "450", ...}} written inside
        that channel's <Chan> block. settings: extra "Key = Value" lines for
        the <Network> block. Flood protection is off unless settings say
        otherwise: with it, ZNC queues lines once registration has used the
        burst, and timing assertions would measure the throttle."""
        self.name = name
        self.settings = {"FloodRate": "-1"}
        self.settings.update(settings or {})
        self.chans = list(chans)
        self.load_lines = list(load_lines)
        self.chan_settings = dict(chan_settings or {})
        self.port = free_port()
        self.ircd = FakeIRCd(self.port, **ircd_kw)


class Znc:
    """One ZNC instance with user `tim` and the given networks.

    user_load_lines   - LoadModule lines placed in the <User> section.
    user_settings     - extra "Key = Value" lines for the <User> section.
    global_settings   - extra "Key = Value" lines at the top of znc.conf.
    registries        - {"user" or network name: {"<module>": {key: value}}},
                        written to the matching moddata/<module>/.registry
                        before ZNC starts.
    global_load_lines - LoadModule lines at the top level of znc.conf.
    """

    def __init__(self, workdir, znc_bin, modules, networks, user_load_lines=(), user_settings=None,
                 registries=None, extra_env=None, global_load_lines=(), global_settings=None):
        self.workdir = workdir
        self.znc_bin = znc_bin
        self.networks = list(networks)
        self.user_load_lines = list(user_load_lines)
        self.global_load_lines = list(global_load_lines)
        self.user_settings = dict(user_settings or {})
        self.global_settings = dict(global_settings or {})
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
        if self.proc is not None:
            subprocess.run(["chown", "-R", "znctest:znctest", os.path.join(self.workdir, "modules")], check=True)

    def write_config(self):
        net_blocks = ""
        for n in self.networks:
            mods = "".join("            LoadModule = %s\n" % l for l in n.load_lines)
            chans = ""
            for c in n.chans:
                extra = "".join("                %s = %s\n" % kv for kv in n.chan_settings.get(c, {}).items())
                chans += "            <Chan %s>\n%s            </Chan>\n" % (c, extra)
            nset = "".join("        %s = %s\n" % kv for kv in n.settings.items())
            net_blocks += "    <Network %s>\n        Server = 127.0.0.1 %d\n%s%s%s    </Network>\n" % (n.name, n.port, nset, mods, chans)
        umods = "".join("    LoadModule = %s\n" % l for l in self.user_load_lines)
        uset = "".join("    %s = %s\n" % kv for kv in self.user_settings.items())
        gmods = "".join("LoadModule = %s\n" % l for l in self.global_load_lines)
        gset = "".join("%s = %s\n" % kv for kv in self.global_settings.items())
        # ConnectDelay and ServerThrottle default to 5 s and 30 s; with them,
        # a second network on 127.0.0.1, or a reconnect, would not happen
        # within the wait.
        cfg = """Version = 1.10.3
ConnectDelay = 0
ServerThrottle = 0
{gset}{gmods}<Listener l>
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
""".format(lp=self.listen_port, uset=uset, umods=umods, nets=net_blocks, gmods=gmods, gset=gset)
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
        if self.proc is not None:
            subprocess.run(["chown", "-R", "znctest:znctest", os.path.join(self.workdir, "users")], check=True)

    def registry(self, where, mod):
        """The module's NV store as ZNC last wrote it, or None if absent."""
        p = self.registry_path(where, mod)
        if not os.path.exists(p):
            return None
        out = {}
        with open(p, errors="surrogateescape") as f:
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
        p = os.path.join(self.workdir, "znc.out")
        if not os.path.exists(p):
            return ""
        with open(p, errors="replace") as f:
            return f.read()

    def client(self, net=None, nick="tim"):
        return Client(self.listen_port, self.net(net).name, nick=nick)


# A line ZNC delivers to the client: optional tags, :prefix, command, params.
LINE = re.compile(r"^(?:@(?P<tags>\S+) )?:(?P<src>\S+) (?P<cmd>\S+) (?P<target>\S+)(?: :?(?P<text>.*))?$")


class Client:
    _seq = 0

    def __init__(self, port, network, nick="tim"):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.buf = b""
        self.lines = []
        self.mark = 0
        self.send("PASS tim/%s:pass" % network)
        self.send("NICK %s" % nick)
        self.send("USER tim 0 * :tim")
        self._wait(lambda l: " 001 " in l)
        # Everything ZNC sends while attaching (JOINs, buffer playback) ends
        # before it processes our next command, so a ping here marks the end
        # of the attach sequence.
        self.attach_lines = self.sync(keep_prior=True)

    def send(self, line):
        self.sock.sendall((line + "\r\n").encode("utf-8", "surrogateescape"))

    def send_many(self, lines):
        """Several lines in one write, as a client script would."""
        self.sock.sendall("".join(l + "\r\n" for l in lines).encode("utf-8", "surrogateescape"))

    def _readline(self, deadline=None):
        while b"\n" not in self.buf:
            if deadline is not None:
                left = deadline - time.time()
                if left <= 0:
                    raise socket.timeout()
                self.sock.settimeout(left)
            try:
                d = self.sock.recv(65536)
            finally:
                self.sock.settimeout(15)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        ln, self.buf = self.buf.split(b"\n", 1)
        # Only the line terminator is stripped: a CR inside a line survives,
        # so a test can see one.
        ln = ln.decode("utf-8", "replace")
        if ln.endswith("\r"):
            ln = ln[:-1]
        if ln.startswith("PING "):
            self.send("PONG " + ln[5:])
        self.lines.append(ln)
        return ln

    def _wait(self, pred):
        while True:
            ln = self._readline()
            if pred(ln):
                return ln

    def wait_for(self, pred, timeout):
        """Read until a line matches pred; returns it, or None on timeout.
        Lines read meanwhile stay in self.lines."""
        end = time.time() + timeout
        while True:
            try:
                ln = self._readline(deadline=end)
            except socket.timeout:
                return None
            if pred(ln):
                return ln

    def drain(self, seconds):
        """All lines that arrive in the next `seconds`."""
        start = len(self.lines)
        end = time.time() + seconds
        while True:
            try:
                self._readline(deadline=end)
            except socket.timeout:
                break
        return self.lines[start:]

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
        """Send PRIVMSG to a module (e.g. *status) and return the text of
        each reply line it sent back."""
        self.sync()
        self.send("PRIVMSG %s :%s" % (target, text))
        got = self.sync()
        return modmsgs(got, target)

    def quit(self, wait=0.6):
        try:
            self.send("QUIT :bye")
            time.sleep(0.1)
            self.sock.close()
        except OSError:
            pass
        time.sleep(wait)  # let ZNC process the detach before more traffic arrives


def modmsgs(lines, target):
    """Text of each PRIVMSG/NOTICE from `target` (e.g. *status) in lines."""
    out = []
    for l in lines:
        m = LINE.match(l)
        if m and m.group("src").split("!")[0] == target and m.group("cmd") in ("PRIVMSG", "NOTICE"):
            out.append(m.group("text") or "")
    return out


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
