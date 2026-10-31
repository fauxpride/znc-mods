"""keepnick_instant test suite: runs a real ZNC against fake IRC servers.

The fake server knows which nicks other (simulated) users hold, answers ISON
and, depending on the scenario, MONITOR, and refuses NICK attempts with 433
or 437 on request. Assertions are made on what ZNC sends to the server (with
arrival times relative to registration), on what the client receives, and on
the module's NV store. The suite runs in real time; see ../TESTING.md for the
timing tolerances.
"""
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hx import Network, Znc, modmsgs  # noqa: E402

MOD = "keepnick_instant"
ZNC_BIN = os.environ.get("ZNC_BIN", "/usr/bin/znc")
NEW = os.environ.get("NEW", "")        # keepnick_instant.so under test
PROBE = os.environ.get("PROBE", "")    # NEW rebuilt with a different version string
if not NEW:
    raise SystemExit("set NEW (the .so under test); see TESTING.md")
EXTRA_ENV = {}
if os.environ.get("ZNC_PRELOAD"):
    EXTRA_ENV["LD_PRELOAD"] = os.environ["ZNC_PRELOAD"]
    EXTRA_ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "")
    EXTRA_ENV["UBSAN_OPTIONS"] = os.environ.get("UBSAN_OPTIONS", "")
ROOT = os.environ.get("HX_ROOT", "/tmp/keepnick_instant-tests")
ONLY = set(a for a in sys.argv[1:] if not a.startswith("-"))

TOL = 0.6     # seconds of slack on every timing assertion
# Fast defaults for most scenarios, seeded into the NV store before start.
FAST = {"StartDelaySec": "2", "IntervalSec": "5", "IdleGapSec": "0", "MinGapSec": "0"}

results = []
open_zncs = []


def check(scn, name, ok, detail=""):
    results.append((scn, name, bool(ok), detail))
    if not ok:
        print("    FAIL %s :: %s :: %s" % (scn, name, detail))


def mk(name, so=NEW, seed=None, monitor=None, held=("tim",), load=True, networks=None, start=True,
       use_fast=True):
    """One network n1 with #a. `held` nicks are taken by other users before
    ZNC connects, so ZNC registers as tim_ when tim is among them."""
    if networks is None:
        networks = [Network("n1", chans=["#a"], load_lines=[MOD] if load else [], monitor=monitor)]
    for n in networks:
        for h in held:
            n.ircd.take(h)
    reg = dict(FAST) if use_fast else {}
    reg.update(seed or {})
    regs = {n.name: {MOD: reg} for n in networks} if reg else None
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, {MOD: so}, networks, registries=regs, extra_env=EXTRA_ENV)
    open_zncs.append(z)
    if start:
        z.start()
    return z


SANITIZER_MARKERS = ("AddressSanitizer", "runtime error:", "LeakSanitizer", "SUMMARY:")


def finish(z):
    try:
        z.close()
    except Exception:
        pass
    out = z.output()
    for bad in SANITIZER_MARKERS + ("Segmentation",):
        if bad in out:
            check("sanitizer", "%s clean (%s)" % (os.path.basename(z.workdir), bad), False, out[-3000:])
    if z in open_zncs:
        open_zncs.remove(z)


def version_of(so):
    data = open(so, "rb").read()
    m = re.search(rb"local MONITOR retry state\) v(\d+\.\d+\.\d+)", data)
    return m.group(1).decode() if m else None


NEW_VER = version_of(NEW)


def unique_syms(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return set(l.split()[-1] for l in out.splitlines() if len(l.split()) >= 3 and l.split()[1] == "u")


def pinned(so):
    return bool(unique_syms(so) - unique_syms(ZNC_BIN))


# --------------------------------------------------------------------------
# Observation helpers
# --------------------------------------------------------------------------

BACKEND = ("ISON", "MONITOR", "NICK")


def traffic(d, since_t=None):
    """[(seconds since the last 001, line)] for ISON/MONITOR/NICK lines ZNC
    sent after registration."""
    t0 = d.welcomes[-1]
    lo = t0 if since_t is None else since_t
    with d.lock:
        rx = list(d.rx)
    return [(t - t0, l) for t, l in rx if t >= lo and l.split(" ")[0] in BACKEND]


def kinds(tr):
    return [l for _, l in tr]


def times(tr, prefix):
    return [t for t, l in tr if l.startswith(prefix)]


def spaced(ts, step):
    """Consecutive gaps all within step +- TOL."""
    return all(abs((b - a) - step) <= TOL for a, b in zip(ts, ts[1:]))


def numerics(lines, *codes):
    return [l for l in lines if len(l.split(" ")) > 1 and l.split(" ")[1] in codes]


def show(c):
    return c.cmd("*" + MOD, "Show")


def show_field(c, label):
    for ln in show(c):
        m = re.search(re.escape(label) + r": (\S+)", ln)
        if m:
            return m.group(1)
    return None


def store(z, net="n1"):
    return z.registry(net, MOD) or {}


def idle(seconds):
    time.sleep(seconds)


# --------------------------------------------------------------------------
# is_* / mon_* - the two backends from connect to reclaim
# --------------------------------------------------------------------------

def is_startup():
    """ISON-only server (MONITOR answered with 421), tim held by someone
    else. Nothing before StartDelay; then one MONITOR probe whose 421 the
    client never sees; then ISON every Interval, whose 303 replies the
    client never sees. When tim frees up silently, the next ISON tick sends
    NICK tim; afterwards the module sends nothing more."""
    scn = "is_startup"
    z = mk(scn, monitor=None)
    d = z.net().ircd
    c = z.client()
    nick_at_001 = d.nick
    idle(13)
    t_rel = time.monotonic() - d.welcomes[-1]
    d.release("tim")
    got = c.wait_for(lambda l: re.search(r" NICK :tim$", l) and l.startswith(":tim_"), 7)
    idle(11)
    tr = traffic(d)
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    probe = times(tr, "MONITOR + tim")
    isons = times(tr, "ISON tim")
    nicks = times(tr, "NICK tim")
    check(scn, "registered on the alternate nick", nick_at_001 == "tim_", nick_at_001)
    check(scn, "nothing before StartDelay", tr and tr[0][0] >= 2 - 0.2, repr(tr[:2]))
    check(scn, "first action is one MONITOR probe at StartDelay", len(probe) == 1 and abs(probe[0] - 2) <= TOL and kinds(tr)[0] == "MONITOR + tim", repr(tr))
    check(scn, "ISON every Interval after the probe", len(isons) >= 3 and abs(isons[0] - 7) <= TOL and spaced(isons, 5), repr(isons))
    check(scn, "client never sees the 421 or the 303s", numerics(seen, "421", "303") == [], repr(numerics(seen, "421", "303")))
    check(scn, "one NICK tim, at the first ISON after the release", len(nicks) == 1 and nicks[0] - t_rel <= 5 + TOL and any(abs(nicks[0] - i) < 0.1 for i in isons), repr((t_rel, nicks, isons)))
    check(scn, "client sees the nick change", got is not None, "")
    check(scn, "silent after the reclaim", [x for x in tr if x[0] > nicks[0] + 0.05] == [] if nicks else False, repr(tr))


def mon_startup():
    """MONITOR server, tim held. At StartDelay the module subscribes with
    MONITOR + tim (730: online) and never sends ISON or MONITOR S. When tim
    goes offline the server's 731 triggers NICK tim at once; the next tick,
    now owning the nick, unsubscribes with MONITOR - tim."""
    scn = "mon_startup"
    z = mk(scn, monitor="ok")
    d = z.net().ircd
    c = z.client()
    idle(10)
    sh_before = show(c)
    t_rel = time.monotonic() - d.welcomes[-1]
    d.release("tim")
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 3)
    idle(6)
    tr = traffic(d)
    sh_after = show(c)
    c.quit(); finish(z)
    nicks = times(tr, "NICK tim")
    check(scn, "first action MONITOR + tim at StartDelay", tr and tr[0][1] == "MONITOR + tim" and abs(tr[0][0] - 2) <= TOL, repr(tr))
    check(scn, "no ISON, no MONITOR S, nothing else until the release",
          [x for x in tr if x[0] < t_rel] == [tr[0]], repr(tr))
    check(scn, "NICK tim within 0.5 s of the 731", len(nicks) == 1 and 0 <= nicks[0] - t_rel <= 0.5, repr((t_rel, nicks)))
    check(scn, "MONITOR - tim at the next tick", kinds(tr)[-1] == "MONITOR - tim" and len(tr) == 3, repr(tr))
    check(scn, "Show before: effective MONITOR, limit 100, active",
          "  Backend mode: Auto   |   Effective: MONITOR" in sh_before and
          "  MONITOR advertised/detected: yes   |   Limit: 100   |   Active: yes   |   Live probe sent: yes" in sh_before, repr(sh_before))
    check(scn, "Show after: current tim, subscription dropped",
          "  Primary: tim   |   Current: tim" in sh_after and
          "  MONITOR advertised/detected: yes   |   Limit: 100   |   Active: no   |   Live probe sent: no" in sh_after, repr(sh_after))


def mon_refused():
    """MONITOR server; tim goes offline (731) but every NICK tim is refused,
    first with 433, then with 437. Each refusal is swallowed; the module keeps
    retrying once per tick on its remembered offline state, without MONITOR
    S, until the refusal stops."""
    scn = "mon_refused"
    z = mk(scn, monitor="ok")
    d = z.net().ircd
    c = z.client()
    idle(3)
    d.refuse["tim"] = "433"
    t_rel = time.monotonic() - d.welcomes[-1]
    d.release("tim")
    idle(6)
    d.refuse["tim"] = "437"
    idle(5)
    del d.refuse["tim"]
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 7)
    idle(0.5)
    tr = traffic(d)
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    nicks = times(tr, "NICK tim")
    check(scn, "first attempt at the 731", nicks and 0 <= nicks[0] - t_rel <= 0.5, repr((t_rel, nicks)))
    check(scn, "one attempt per tick after that", len(nicks) == 4 and spaced(nicks[1:], 5), repr(nicks))
    check(scn, "no MONITOR S polling", not any(l.startswith("MONITOR S") for _, l in tr), repr(tr))
    check(scn, "client saw neither 433 nor 437", numerics(seen, "433", "437") == [], repr(numerics(seen, "433", "437")))
    check(scn, "the last attempt succeeded", d.nick == "tim", d.nick)


def is_refused_437():
    """ISON server; tim is free but locked (437), as after a netsplit on
    ratbox-style servers. Each attempt's 437 is swallowed; the module retries
    every tick and takes the nick when the lock ends."""
    scn = "is_refused_437"
    z = mk(scn, monitor=None, held=())
    d = z.net().ircd
    # ZNC registered as tim; take it away so the module has work to do.
    d.refuse["tim"] = "437"
    d.note(":%s NICK :tim_" % d.me())
    d.nick = "tim_"
    c = z.client()
    idle(13)
    del d.refuse["tim"]
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 7)
    tr = traffic(d)
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    nicks = times(tr, "NICK tim")
    check(scn, "an attempt per ISON tick", len(nicks) >= 3 and spaced(nicks, 5), repr(tr))
    check(scn, "client saw no 437", numerics(seen, "437") == [], repr(numerics(seen, "437")))
    check(scn, "reclaimed once the lock ended", d.nick == "tim", d.nick)


# --------------------------------------------------------------------------
# ev_* - reacting to NICK and QUIT the module can see
# --------------------------------------------------------------------------

def ev_visible():
    """StartDelay 600, so no tick runs. The holder of tim, seen in a shared
    channel, changes nick (network n1) or quits (n2): NICK tim follows at
    once. Unrelated NICK and QUIT events, and someone else taking tim, cause
    nothing."""
    scn = "ev_visible"
    nets = [Network("n1", chans=["#a"], load_lines=[MOD]), Network("n2", chans=["#a"], load_lines=[MOD])]
    z = mk(scn, networks=nets, seed={"StartDelaySec": "600"})
    d1, d2 = z.net("n1").ircd, z.net("n2").ircd
    for d in (d1, d2):
        d.note(":bob!u@other.host NICK :carol")
        d.note(":alice!u@other.host QUIT :bye")
        d.note(":dave!u@other.host NICK :tim")
        d.sync()
    idle(1)
    quiet = traffic(d1) + traffic(d2)
    t1 = time.monotonic()
    d1.release("tim", visible="nick", newnick="tim_away")
    t2 = time.monotonic()
    d2.release("tim", visible="quit")
    idle(1)
    n1, n2 = times(traffic(d1), "NICK tim"), times(traffic(d2), "NICK tim")
    base1, base2 = d1.welcomes[-1], d2.welcomes[-1]
    finish(z)
    check(scn, "unrelated events and a new holder cause nothing", quiet == [], repr(quiet))
    check(scn, "holder's NICK change: NICK tim at once", len(n1) == 1 and 0 <= n1[0] - (t1 - base1) <= 0.5, repr(n1))
    check(scn, "holder's QUIT: NICK tim at once", len(n2) == 1 and 0 <= n2[0] - (t2 - base2) <= 0.5, repr(n2))
    check(scn, "both networks hold tim now", (d1.nick, d2.nick) == ("tim", "tim"), repr((d1.nick, d2.nick)))


def ev_mingap():
    """MinGap 3, tim refused with 433. Three QUIT events for tim within half
    a second give one NICK attempt; another after 3 s gives a second. The
    dedupe timer then restarts the scheduler, so ISON polling begins about
    MinGap after an attempt; it re-arms the scheduler only when no tick is
    pending. Here the StartDelay tick (600 s) is pending, so no polling
    starts early."""
    scn = "ev_mingap"
    z = mk(scn, seed={"StartDelaySec": "600", "MinGapSec": "3"})
    d = z.net().ircd
    d.refuse["tim"] = "433"
    base = d.welcomes[-1]
    t1 = time.monotonic() - base
    for _ in range(3):
        d.note(":tim!u@other.host QUIT :bye")
        time.sleep(0.15)
    idle(3.4)
    t2 = time.monotonic() - base
    d.note(":tim!u@other.host QUIT :bye")
    idle(9)
    tr = traffic(d)
    finish(z)
    nicks = times(tr, "NICK tim")
    check(scn, "one attempt for the burst, one after MinGap", len(nicks) == 2 and nicks[0] - t1 <= 0.5 and 0 <= nicks[1] - t2 <= 0.5, repr((t1, t2, nicks)))
    check(scn, "no polling while the StartDelay tick is pending", [l for _, l in tr if not l.startswith("NICK")] == [], repr(tr))


# --------------------------------------------------------------------------
# ac_* - activity, idle gap, and what the client's own traffic does
# --------------------------------------------------------------------------

def ac_idle_gap():
    """IdleGap 4. While the client sends a channel message every second, no
    tick runs; once it stops, ticks resume. Client keepalive PINGs count as
    activity in exactly the same way (characterised: a client that pings
    more often than IdleGap suppresses every tick)."""
    scn = "ac_idle_gap"
    res = {}
    for kind in ("PRIVMSG #a :typing", "PING :keepalive"):
        z = mk("%s_%s" % (scn, kind.split()[0].lower()), seed={"IdleGapSec": "4"})
        d = z.net().ircd
        c = z.client()
        base = d.welcomes[-1]
        end = time.time() + 13
        while time.time() < end:
            c.send(kind)
            time.sleep(1)
        stop = time.monotonic() - base
        idle(10)
        res[kind] = (stop, traffic(d))
        c.quit(); finish(z)
    for kind, (stop, tr) in res.items():
        during = [x for x in tr if x[0] < stop]
        after = [x for x in tr if x[0] >= stop]
        check(scn, "%s: nothing while active" % kind.split()[0], during == [], repr(tr))
        check(scn, "%s: ticks resume once idle" % kind.split()[0], len(after) >= 1 and after[0][0] - stop <= 5 + 4 + TOL, repr(tr))


def ac_user_traffic():
    """The client's own ISON and NICK. A user ISON reply passes through to
    the client. Characterised: because the module treats any 303 without its
    nick as 'nick free', a user ISON for other nicks also fires a NICK tim
    attempt (its 433 swallowed). A user ISON that includes tim does not. A
    manual NICK tim that fails shows its 433 to the client."""
    scn = "ac_user_traffic"
    z = mk(scn, seed={"StartDelaySec": "600"})
    d = z.net().ircd
    c = z.client()
    base = d.welcomes[-1]
    m0 = time.monotonic() - base
    c.send("ISON tim")
    r1 = c.wait_for(lambda l: " 303 " in l, 3)
    idle(0.5)
    a = [x for x in traffic(d) if x[0] > m0]
    m1 = time.monotonic() - base
    c.send("ISON bob")
    r2 = c.wait_for(lambda l: " 303 " in l, 3)
    idle(0.5)
    b = [x for x in traffic(d) if x[0] > m1]
    c.sync()
    c.send("NICK tim")
    r3 = c.wait_for(lambda l: " 433 " in l, 3)
    c.quit(); finish(z)
    check(scn, "user ISON tim: reply reaches the client", r1 is not None and r1.endswith(":tim"), repr(r1))
    check(scn, "user ISON tim: module does nothing", a == [(a[0][0], "ISON tim")] if a else False, repr(a))
    check(scn, "user ISON bob: reply reaches the client", r2 is not None and r2.endswith(":"), repr(r2))
    check(scn, "user ISON bob: module fires NICK tim (characterised)", kinds(b) == ["ISON bob", "NICK tim"], repr(b))
    check(scn, "manual NICK tim: 433 reaches the client", r3 is not None and " 433 tim_ tim " in r3, repr(r3))


# --------------------------------------------------------------------------
# cfg_* - commands, settings and their persistence
# --------------------------------------------------------------------------

def cfg_commands():
    """Every setting command at its bounds, out of range, and with
    non-numeric input; Backend; unknown commands; the NV store afterwards.
    Characterised: non-numeric input reads as 0, which the 0-based settings
    accept, and values that wrap past 2^32 are accepted as their wrapped
    value."""
    scn = "cfg_commands"
    z = mk(scn, held=(), seed={"StartDelaySec": "600"})
    c = z.client()
    cases = [
        ("Interval 4", ["Usage: Interval <seconds 5-300> — Default 5s."]),
        ("Interval 301", ["Usage: Interval <seconds 5-300> — Default 5s."]),
        ("Interval abc", ["Usage: Interval <seconds 5-300> — Default 5s."]),
        ("Interval 300", ["Interval set to 300s (ISON frequency when backend resolves to ISON and you don't own the nick)."]),
        ("Interval 4294967301", ["Interval set to 5s (ISON frequency when backend resolves to ISON and you don't own the nick)."]),
        ("IdleGap 31", ["Usage: IdleGap <seconds 0-30> — Default 2s."]),
        ("IdleGap 30", ["IdleGap set to 30s. Skip a backend tick if you've sent anything in the last IdleGap seconds."]),
        ("IdleGap abc", ["IdleGap set to 0s. Skip a backend tick if you've sent anything in the last IdleGap seconds."]),
        ("StartDelay 601", ["Usage: StartDelay <seconds 0-600> — Default 90s."]),
        ("StartDelay 600", ["StartDelay set to 600s. First backend action after connect (join-safe). Effective next connect."]),
        ("Jitter 11", ["Usage: Jitter <seconds 0-10> — Default 0s."]),
        ("Jitter 10", ["Jitter set to 0..10s. Random extra wait added to each scheduled backend tick."]),
        ("Jitter x", ["Jitter set to 0..0s. Random extra wait added to each scheduled backend tick."]),
        ("MinGap 31", ["Usage: MinGap <seconds 0-30> — Default 3s."]),
        ("MinGap 30", ["MinGap set to 30s. Minimum spacing between NICK attempts (dedupe)."]),
        ("Backend bogus", ["Usage: Backend <Auto|Ison|Monitor> — Default Auto."]),
        ("Backend ISON", ["Backend mode set to Ison. Effective backend now: ISON."]),
        ("Backend monitor", ["Backend mode set to Monitor. Effective backend now: ISON."]),
        ("Backend Auto", ["Backend mode set to Auto. Effective backend now: ISON."]),
        ("SetNick", ["Usage: SetNick <nick> — Set & persist the primary nick to reclaim."]),
        ("Frobnicate", ["Unknown command. Try: Help"]),
        ("Disable", ["Disabled. No backend checks or reclaim attempts."]),
        ("Enable", ["Enabled. Backend checks run only when you don't own the nick."]),
    ]
    for i, (cmd, exp) in enumerate(cases):
        got = c.cmd("*" + MOD, cmd)
        check(scn, "%02d %s" % (i, cmd), got == exp, "got %r want %r" % (got, exp))
    sh = show(c)
    st = store(z)
    help_ = c.cmd("*" + MOD, "Help")
    empty = c.cmd("*" + MOD, "")
    c.quit(); finish(z)
    check(scn, "Show reflects the settings",
          sh[1] == "Version: %s" % NEW_VER and
          "  Interval: 5s   (default 5s)" in sh and "  IdleGap: 0s     (default 2s)" in sh and
          "  StartDelay: 600s (default 90s; first backend action after connect)" in sh and
          "  Jitter: 0..0s      (default 0s; random extra wait)" in sh and
          "  MinGap: 30s        (default 3s; dedupe NICK attempts)" in sh, repr(sh))
    check(scn, "Help and an empty command show the same as Show", help_ == sh and empty == sh, repr((help_[:2], empty[:2])))
    check(scn, "NV holds every setting", st == {"Enabled": "1", "BackendMode": "Auto", "IntervalSec": "5", "IdleGapSec": "0",
                                                "StartDelaySec": "600", "JitterSec": "0", "MinGapSec": "30", "PrimaryNick": "tim"}, repr(st))


def cfg_setnick():
    """SetNick validation, and a new primary nick that is held: the module
    starts work on it at once (KickBackendNow), and reclaims it when free."""
    scn = "cfg_setnick"
    z = mk(scn, held=("other",), seed={"StartDelaySec": "600"})
    d = z.net().ircd
    c = z.client()
    rej = ("Rejected: nick must not be empty, start with ':', or contain spaces, commas, or control characters. "
           "(These would be interpreted as IRC framing when the module concatenates the nick into NICK / ISON / MONITOR commands.)")
    for bad in (":x", "a,b", "a b", "a\tb"):
        got = c.cmd("*" + MOD, "SetNick " + bad)
        check(scn, "SetNick %r rejected" % bad, got == [rej], repr(got))
    base = d.welcomes[-1]
    m = time.monotonic() - base
    got = c.cmd("*" + MOD, "SetNick other")
    idle(1)
    early = [x for x in traffic(d) if x[0] > m]
    d.release("other")
    c.wait_for(lambda l: re.search(r" NICK :other$", l), 8)
    idle(0.3)
    st = store(z)
    prim = show_field(c, "Primary")
    c.quit(); finish(z)
    check(scn, "SetNick other accepted", got == ["Primary nick set to: other"], repr(got))
    check(scn, "work starts at once: probe then ISON", kinds(early)[:1] == ["MONITOR + other"] and early[0][0] - m <= 0.5, repr(early))
    check(scn, "reclaims the new nick", d.nick == "other", d.nick)
    check(scn, "persisted and shown", st.get("PrimaryNick") == "other" and prim == "other", repr((st, prim)))


def cfg_load_args():
    """Primary nick sources in order: a valid module argument (stored), the
    stored PrimaryNick, the configured user nick. An invalid argument and an
    invalid stored value are warned about; the stored one is deleted."""
    scn = "cfg_load_args"
    warn_arg = "Warning: module argument nick contains invalid characters (control/space/comma/leading-':') and was ignored."
    warn_nv = "Warning: stored PrimaryNick contained invalid characters and has been cleared."
    cases = [
        ("bob", {}, "bob", [], "bob"),
        (":bad", {"PrimaryNick": "carol"}, "carol", [warn_arg], "carol"),
        ("", {"PrimaryNick": "a b"}, "tim", [warn_nv], None),
        (":bad", {"PrimaryNick": "a,b"}, "tim", [warn_arg, warn_nv], None),
        ("", {}, "tim", [], None),
    ]
    for i, (arg, seed, want, warns, want_nv) in enumerate(cases):
        z = mk("%s_%d" % (scn, i), load=False, held=(), use_fast=False)
        z.write_registry("n1", MOD, dict(seed, StartDelaySec="600"))
        c = z.client()
        c.sync()
        c.send("PRIVMSG *status :LoadMod %s %s" % (MOD, arg))
        out = modmsgs(c.sync(), "*" + MOD)
        prim = show_field(c, "Primary")
        st = store(z)
        c.quit(); finish(z)
        label = "arg %r, stored %r" % (arg, seed.get("PrimaryNick"))
        check(scn, label + ": primary " + want, prim == want, repr(prim))
        check(scn, label + ": warnings", out == warns, repr(out))
        check(scn, label + ": stored PrimaryNick", st.get("PrimaryNick") == want_nv, repr(st))


def cfg_enable_disable():
    """Disable stops all backend traffic and reactive reclaim, survives a
    restart, and Enable checks at once."""
    scn = "cfg_enable_disable"
    z = mk(scn, monitor=None)
    d = z.net().ircd
    c = z.client()
    c.cmd("*" + MOD, "Disable")
    m = time.monotonic() - d.welcomes[-1]
    idle(8)
    d.release("tim", visible="quit")
    idle(1)
    silent = [x for x in traffic(d) if x[0] > m]
    c.quit()
    z.stop()
    d.take("tim")
    z.start()
    c = z.client()
    st_shown = show_field(c, "Status")
    idle(7)
    silent2 = traffic(d)
    d.release("tim")
    m2 = time.monotonic() - d.welcomes[-1]
    en = c.cmd("*" + MOD, "Enable")
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 8)
    after = [x for x in traffic(d) if x[0] >= m2 - 0.05]
    st = store(z)
    c.quit(); finish(z)
    check(scn, "disabled: no backend traffic, no reaction to a visible QUIT", silent == [], repr(silent))
    check(scn, "still disabled after a restart", st_shown == "DISABLED" and silent2 == [], repr((st_shown, silent2)))
    check(scn, "Enable checks at once and reclaims", en == ["Enabled. Backend checks run only when you don't own the nick."]
          and after and after[0][0] - m2 <= 0.5 and d.nick == "tim", repr((en, after)))
    check(scn, "NV Enabled=1", st.get("Enabled") == "1", repr(st))


def cfg_persist():
    """Settings changed by command survive a restart and take effect."""
    scn = "cfg_persist"
    z = mk(scn, held=(), seed={"StartDelaySec": "600"})
    c = z.client()
    for cmd in ("Interval 7", "IdleGap 3", "StartDelay 4", "Jitter 1", "MinGap 2", "Backend Ison", "SetNick nobody"):
        c.cmd("*" + MOD, cmd)
    c.quit()
    z.stop()
    z.start()
    c = z.client()
    sh = show(c)
    c.quit(); finish(z)
    for want in ("  Primary: nobody   |   Current: tim", "  Backend mode: Ison   |   Effective: ISON", "  Interval: 7s   (default 5s)",
                 "  IdleGap: 3s     (default 2s)", "  StartDelay: 4s (default 90s; first backend action after connect)",
                 "  Jitter: 0..1s      (default 0s; random extra wait)", "  MinGap: 2s        (default 3s; dedupe NICK attempts)"):
        check(scn, want.strip().split(":")[0], want in sh, repr(sh))


# --------------------------------------------------------------------------
# be_* - backend selection and MONITOR edge cases
# --------------------------------------------------------------------------

def be_modes():
    """Backend Ison on a MONITOR server never uses MONITOR. Backend Monitor
    on a server without it probes, gets 421 (swallowed) and falls back to
    ISON."""
    scn = "be_modes"
    z = mk(scn + "_ison", monitor="ok", seed={"BackendMode": "Ison"})
    d = z.net().ircd
    idle(13)
    tr = traffic(d)
    finish(z)
    check(scn, "Ison on a MONITOR server: ISON only", tr and set(kinds(tr)) == {"ISON tim"} and spaced(times(tr, "ISON"), 5), repr(tr))
    z = mk(scn + "_monitor", monitor=None, seed={"BackendMode": "Monitor"})
    d = z.net().ircd
    c = z.client()
    idle(13)
    tr = traffic(d)
    eff = show_field(c, "Effective")
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    check(scn, "Monitor without server support: probe, then ISON", kinds(tr)[:1] == ["MONITOR + tim"] and "ISON tim" in kinds(tr) and eff == "ISON", repr((tr, eff)))
    check(scn, "Monitor without server support: 421 swallowed", numerics(seen, "421") == [], repr(seen[-5:]))


def be_monitor_full():
    """MONITOR list full (734): the module says so in its window and falls
    back to ISON at the next Interval. The 734 itself reaches the client."""
    scn = "be_monitor_full"
    z = mk(scn, monitor="full")
    d = z.net().ircd
    c = z.client()
    msg = c.wait_for(lambda l: "*%s!" % MOD in l, 5)
    idle(11)
    tr = traffic(d)
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    check(scn, "module explains the fallback", msg is not None and msg.endswith(":MONITOR list full/unusable for this target on this connection; falling back to ISON."), repr(msg))
    isons = times(tr, "ISON")
    check(scn, "ISON about Interval after the 734, then every Interval", isons and abs(isons[0] - 7) <= TOL and spaced(isons, 5) and kinds(tr).count("MONITOR + tim") == 1, repr(tr))
    check(scn, "734 passed through to the client", len(numerics(seen, "734")) == 1, repr(numerics(seen, "734")))


def be_monitor_silent():
    """Characterised: a server that advertises MONITOR but never answers it.
    The module sends one MONITOR + tim, then waits for a 730/731 that never
    comes: it never falls back to ISON and never reclaims a nick that frees
    up silently. Poke does not get it out of that state; Backend Ison does.
    See TESTING.md, Known gaps."""
    scn = "be_monitor_silent"
    z = mk(scn, monitor="silent")
    d = z.net().ircd
    c = z.client()
    idle(9)
    d.release("tim")
    idle(8)
    stalled = traffic(d)
    m = time.monotonic() - d.welcomes[-1]
    c.cmd("*" + MOD, "Poke")
    idle(1)
    after_poke = [x for x in traffic(d) if x[0] > m]
    m = time.monotonic() - d.welcomes[-1]
    c.cmd("*" + MOD, "Backend Ison")
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 3)
    after_ison = [x for x in traffic(d) if x[0] > m]
    c.quit(); finish(z)
    check(scn, "one probe, then nothing (no ISON fallback, no reclaim)", kinds(stalled) == ["MONITOR + tim"], repr(stalled))
    check(scn, "Poke sends nothing", after_poke == [], repr(after_poke))
    check(scn, "Backend Ison polls and reclaims at once", kinds(after_ison) == ["ISON tim", "NICK tim"] and d.nick == "tim", repr(after_ison))


def be_monitor_unrelated():
    """A 730 for a nick the module did not ask about does not mark its own
    subscription active (match-gated)."""
    scn = "be_monitor_unrelated"
    z = mk(scn, monitor="ok", seed={"StartDelaySec": "600"})
    d = z.net().ircd
    c = z.client()
    d.note(":irc.test 730 tim_ :bob!u@other.host")
    d.sync()
    line = [l for l in show(c) if "MONITOR advertised" in l]
    c.quit(); finish(z)
    check(scn, "Active stays no", line == ["  MONITOR advertised/detected: yes   |   Limit: 100   |   Active: no   |   Live probe sent: no"], repr(line))


# --------------------------------------------------------------------------
# lc_* - lifecycle: hot load, reconnect, StartDelay 0, Poke, jitter
# --------------------------------------------------------------------------

def lc_hot_load():
    """LoadMod into a live session uses one Interval, not StartDelay (90 by
    default). On a MONITOR server whose 005 the module never saw, the first
    tick probes MONITOR even when ZNC already owns the nick, so detection
    works; the next tick unsubscribes. On an ISON server with tim held, the
    first tick probes, the 421 is swallowed, and ISON follows."""
    scn = "lc_hot_load"
    z = mk(scn + "_monitor", monitor="ok", held=(), load=False, use_fast=False)
    d = z.net().ircd
    c = z.client()
    m = time.monotonic() - d.welcomes[-1]
    c.cmd("*status", "LoadMod " + MOD)
    idle(11.5)
    tr = [x for x in traffic(d) if x[0] > m]
    det = [l for l in show(c) if "MONITOR advertised" in l]
    c.quit(); finish(z)
    check(scn, "MONITOR server, nick owned: probe after one Interval, then unsubscribe",
          kinds(tr) == ["MONITOR + tim", "MONITOR - tim"] and abs(tr[0][0] - m - 5) <= TOL and abs(tr[1][0] - tr[0][0] - 5) <= TOL, repr((m, tr)))
    check(scn, "MONITOR detected", det and det[0].startswith("  MONITOR advertised/detected: yes"), repr(det))
    z = mk(scn + "_ison", monitor=None, load=False, use_fast=False)
    d = z.net().ircd
    c = z.client()
    m = time.monotonic() - d.welcomes[-1]
    c.cmd("*status", "LoadMod " + MOD)
    idle(11.5)
    tr = [x for x in traffic(d) if x[0] > m]
    c.sync()  # read everything ZNC sent while the test slept
    seen = c.lines
    c.quit(); finish(z)
    check(scn, "ISON server, nick held: probe after one Interval, ISON after another",
          kinds(tr)[:2] == ["MONITOR + tim", "ISON tim"] and abs(tr[0][0] - m - 5) <= TOL and abs(tr[1][0] - tr[0][0] - 5) <= TOL, repr((m, tr)))
    check(scn, "421 swallowed", numerics(seen, "421") == [], repr(numerics(seen, "421")))


def lc_reconnect():
    """After a reconnect, StartDelay applies again and there is exactly one
    tick chain (no duplicate ISONs from a timer left over from before)."""
    scn = "lc_reconnect"
    z = mk(scn, monitor=None, seed={"BackendMode": "Ison"})
    d = z.net().ircd
    idle(8)
    before = traffic(d)
    d.drop()
    d.wait_ready(["#a"])
    idle(15)
    after = traffic(d)
    finish(z)
    isons = times(after, "ISON")
    check(scn, "polling before the drop", len(times(before, "ISON")) >= 2, repr(before))
    check(scn, "first ISON at StartDelay after reconnect", isons and abs(isons[0] - 2) <= TOL, repr(after))
    check(scn, "single chain: one ISON per Interval", len([t for t in isons if t < 12.5]) == 3 and spaced(isons, 5), repr(after))


def lc_startdelay_zero():
    """StartDelay 0, module loaded from znc.conf. After 001 the first tick
    runs at once, then one per Interval (no tight loop). Characterised: the
    tick that OnLoad armed fires while ZNC is still connecting, and because
    a tick does not check that registration has finished, ISON goes out as
    the first line on the socket, before CAP/NICK/USER; a real server
    answers it with 451. See TESTING.md, Known gaps."""
    scn = "lc_startdelay_zero"
    z = mk(scn, monitor=None, seed={"StartDelaySec": "0", "BackendMode": "Ison"})
    d = z.net().ircd
    idle(6)
    tr = traffic(d)
    with d.lock:
        first = [l for _, l in d.rx][:2]
    finish(z)
    isons = times(tr, "ISON")
    check(scn, "ISON at once after 001, then after Interval", len(isons) == 2 and isons[0] <= 0.5 and abs(isons[1] - isons[0] - 5) <= TOL, repr(tr))
    check(scn, "stray ISON before registration (characterised)", first == ["ISON tim", "CAP LS 302"], repr(first))


def lc_poke():
    """StartDelay 600. Poke runs a check at once (probe on an ISON server);
    after the nick frees up, a second Poke reclaims it at once."""
    scn = "lc_poke"
    z = mk(scn, monitor=None, seed={"StartDelaySec": "600"})
    d = z.net().ircd
    c = z.client()
    base = d.welcomes[-1]
    p1 = c.cmd("*" + MOD, "Poke")
    idle(0.5)
    a = traffic(d)
    d.release("tim")
    m = time.monotonic() - base
    c.cmd("*" + MOD, "Poke")
    c.wait_for(lambda l: re.search(r" NICK :tim$", l), 3)
    b = [x for x in traffic(d) if x[0] > m]
    c.quit(); finish(z)
    check(scn, "reply", p1 == ["Poke: forcing an immediate backend check now."], repr(p1))
    check(scn, "first Poke: immediate probe", kinds(a) == ["MONITOR + tim"], repr(a))
    check(scn, "second Poke: immediate ISON and NICK", kinds(b) == ["ISON tim", "NICK tim"] and b[-1][0] - m <= 0.5, repr(b))


def lc_jitter():
    """Jitter 2: each ISON gap lies between Interval and Interval + 2."""
    scn = "lc_jitter"
    z = mk(scn, monitor=None, seed={"JitterSec": "2", "BackendMode": "Ison"})
    d = z.net().ircd
    idle(24)
    isons = times(traffic(d), "ISON")
    finish(z)
    gaps = [round(b - a, 2) for a, b in zip(isons, isons[1:])]
    check(scn, "gaps within [5, 7]", len(gaps) >= 2 and all(5 - 0.2 <= g <= 7 + TOL for g in gaps), repr(gaps))


# --------------------------------------------------------------------------
# ty_*, up_*, sym_*
# --------------------------------------------------------------------------

def listmods_sections(c):
    out = c.cmd("*status", "ListMods")
    sections, cur = [], None
    for x in out:
        m = re.match(r"^(Global|User|Network) modules:", x)
        if m:
            cur = m.group(1)
        elif MOD in x and cur and cur not in sections:
            sections.append(cur)
    return sections


def ty_runtime_load():
    """Network-only: --type=user and --type=global are refused."""
    scn = "ty_runtime_load"
    for i, cmd in enumerate(["LoadMod %s" % MOD, "LoadMod --type=user %s" % MOD, "LoadMod --type=global %s" % MOD]):
        z = mk("%s_%d" % (scn, i), load=False, held=(), use_fast=False)
        c = z.client()
        reply = c.cmd("*status", cmd)
        sections = listmods_sections(c)
        c.quit(); finish(z)
        if i == 0:
            check(scn, cmd, sections == ["Network"], repr((reply, sections)))
        else:
            check(scn, cmd + " refused", sections == [] and any("does not support" in x for x in reply), repr((reply, sections)))


def up_inplace():
    """UpdateMod to the PROBE build: Show reports the probe's version (or the
    old one if glibc cannot unload the build, decided with nm), and the
    stored settings carry over."""
    scn = "up_inplace"
    if not PROBE:
        check(scn, "PROBE build provided", False, "set PROBE; see TESTING.md")
        return
    z = mk(scn, held=(), seed={"StartDelaySec": "600"})
    c = z.client()
    c.cmd("*" + MOD, "Interval 9")
    z.install_modules({MOD: PROBE})
    upd = c.cmd("*status", "UpdateMod " + MOD)
    sh = show(c)
    c.quit(); finish(z)
    stuck = pinned(NEW)
    print("    info: build under test unloadable by glibc in this znc: %s" % ("no (pinned)" if stuck else "yes"))
    exp = NEW_VER if stuck else version_of(PROBE)
    check(scn, "UpdateMod reports Done", upd[-1:] == ["Done"], repr(upd))
    check(scn, "version after UpdateMod is %s" % exp, len(sh) > 1 and sh[1] == "Version: %s" % exp, repr(sh[:2]))
    check(scn, "settings carried over", "  Interval: 9s   (default 5s)" in sh, repr(sh))


def sym_unique():
    scn = "sym_unique"
    extra = sorted(unique_syms(NEW) - unique_syms(ZNC_BIN))
    check(scn, "no unique symbols beyond znc's", not extra, repr(extra))


SCENARIOS = [is_startup, mon_startup, mon_refused, is_refused_437, ev_visible, ev_mingap, ac_idle_gap,
             ac_user_traffic, cfg_commands, cfg_setnick, cfg_load_args, cfg_enable_disable, cfg_persist,
             be_modes, be_monitor_full, be_monitor_silent, be_monitor_unrelated, lc_hot_load, lc_reconnect,
             lc_startdelay_zero, lc_poke, lc_jitter, ty_runtime_load, up_inplace, sym_unique]


def selected(fn):
    if not ONLY:
        return True
    name = fn.__name__
    return name in ONLY or name.split("_")[0] in ONLY


def main():
    print("ZNC: %s | new: %s (%s) | probe: %s | preload: %s"
          % (ZNC_BIN, NEW, NEW_VER, PROBE or None, EXTRA_ENV.get("LD_PRELOAD")))
    for fn in SCENARIOS:
        if not selected(fn):
            continue
        print("  running %s" % fn.__name__)
        try:
            fn()
        except Exception:
            check(fn.__name__, "scenario raised", False, traceback.format_exc())
        finally:
            for z in list(open_zncs):
                finish(z)
    by = {}
    for scn, _, ok, _ in results:
        p, f = by.get(scn, (0, 0))
        by[scn] = (p + ok, f + (not ok))
    print()
    for scn, (p, f) in by.items():
        print("  %-36s %3d passed %3d failed" % (scn, p, f))
    tp = sum(p for p, _ in by.values())
    tf = sum(f for _, f in by.values())
    print("\nTOTAL: %d passed, %d failed" % (tp, tf))
    sys.exit(1 if tf else 0)


if __name__ == "__main__":
    main()
