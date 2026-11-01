"""keepchanbuffersize test suite: runs a real ZNC against fake IRC servers.

Every scenario asserts on what ZNC itself reports (ListChans, the module's
replies, the NV registry it wrote) or on what a client receives from buffer
playback. The control scenario runs the same channel cycle without the module,
so the suite shows that the behaviour it checks is the module's doing and not
ZNC's. See ../TESTING.md.
"""
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hx import Network, Znc, deliveries  # noqa: E402

MOD = "keepchanbuffersize"
ZNC_BIN = os.environ.get("ZNC_BIN", "/usr/bin/znc")
NEW = os.environ.get("NEW", "")        # keepchanbuffersize.so under test
PROBE = os.environ.get("PROBE", "")    # NEW rebuilt with a different version string
if not NEW:
    raise SystemExit("set NEW (the .so under test); see TESTING.md")
EXTRA_ENV = {}
if os.environ.get("ZNC_PRELOAD"):
    EXTRA_ENV["LD_PRELOAD"] = os.environ["ZNC_PRELOAD"]
    EXTRA_ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "")
    EXTRA_ENV["UBSAN_OPTIONS"] = os.environ.get("UBSAN_OPTIONS", "")
ROOT = os.environ.get("HX_ROOT", "/tmp/keepchanbuffersize-tests")
ONLY = set(a for a in sys.argv[1:] if not a.startswith("-"))

# Distinct values so a default, an explicit value and the ceiling can never
# be confused with one another.
DEFAULT_BUF = 7       # user ChanBufferSize
MAX_BUF = 300         # global MaxBufferSize
USER_SETTINGS = {"ChanBufferSize": str(DEFAULT_BUF)}
GLOBAL_SETTINGS = {"MaxBufferSize": str(MAX_BUF)}

results = []  # (scenario, check, ok, detail)
open_zncs = []


def check(scn, name, ok, detail=""):
    results.append((scn, name, bool(ok), detail))
    if not ok:
        print("    FAIL %s :: %s :: %s" % (scn, name, detail))


def mk(name, so=None, networks=None, registries=None, start=True, global_settings=None):
    """so=None runs ZNC without the module (control)."""
    if networks is None:
        networks = [Network("n1", chans=["#a", "#b"], load_lines=[MOD] if so else [])]
    gs = dict(GLOBAL_SETTINGS)
    gs.update(global_settings or {})
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, {MOD: so} if so else {}, networks,
            user_settings=USER_SETTINGS, registries=registries, extra_env=EXTRA_ENV, global_settings=gs)
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
    m = re.search(rb"\(incl\. /cycle\)\. v(\d+\.\d+\.\d+)", data)
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

def listchans(c):
    """{lowercased channel: (explicit, count)} from *status ListChans. A
    leading '*' in the Buffer column marks an explicitly set size."""
    out = {}
    for ln in c.cmd("*status", "ListChans"):
        cells = [x.strip() for x in ln.split("|")]
        if len(cells) < 7 or cells[1] in ("Index", "") or not cells[1].isdigit():
            continue
        buf = cells[5]
        out[cells[2].lower()] = (buf.startswith("*"), int(buf.lstrip("*")))
    return out


def store(z, net="n1"):
    """The module's NV store; {} when ZNC has removed the empty registry file."""
    return z.registry(net, MOD) or {}


def buf(c, chan):
    return listchans(c).get(chan.lower())


def wait_line(c, pred, what):
    ln = c.wait_for(pred, 5)
    if ln is None:
        raise RuntimeError("timed out waiting for " + what)
    return ln


def part(c, chans, reason=None):
    c.send("PART %s%s" % (chans, " :" + reason if reason else ""))
    last = chans.split(",")[-1]
    wait_line(c, lambda l: re.search(r" PART %s(\s|$)" % re.escape(last), l, re.I), "PART " + last)
    c.sync()


def join(c, chans):
    c.send("JOIN %s" % chans)
    last = chans.split(",")[-1]
    wait_line(c, lambda l: re.search(r" 366 \S+ %s " % re.escape(last), l, re.I), "JOIN " + last)
    c.sync()


def cycle(c, chan):
    part(c, chan)
    join(c, chan)


def playback_count(z, chan, n=60):
    """Detach, have the server send n lines to `chan`, reattach, and count
    how many the buffer played back. This is the size ZNC actually applies,
    independent of what ListChans reports."""
    d = z.net().ircd
    for i in range(1, n + 1):
        d.say(chan, "alice", "P%d line" % i)
    d.sync()
    c2 = z.client()
    got = [t for nick, _, tgt, t in deliveries(c2.attach_lines) if tgt.lower() == chan.lower()]
    c2.quit()
    return len(got)


# --------------------------------------------------------------------------
# ctl_* - the problem the module exists for, without the module
# --------------------------------------------------------------------------

def ctl_without_module():
    """No module loaded. An explicit buffer size set with SetBuffer is lost
    when the channel is parted and rejoined: ZNC deletes the channel object
    on PART and creates a new one with the user default. A channel with a
    size above MaxBufferSize from znc.conf loses it the same way. This is
    what every cy_* scenario must turn around."""
    scn = "ctl_without_module"
    z = mk(scn, None, networks=[Network("n1", chans=["#a", "#big"], chan_settings={"#big": {"Buffer": "450"}})])
    c = z.client()
    set_reply = c.cmd("*status", "SetBuffer #a 42")
    before = listchans(c)
    cycle(c, "#a")
    cycle(c, "#big")
    after = listchans(c)
    c.quit()
    pb = playback_count(z, "#a")
    finish(z)
    check(scn, "SetBuffer sets an explicit size", set_reply == ["Size of every buffer was set to 42 lines"] and before["#a"] == (True, 42), repr((set_reply, before)))
    check(scn, "config can set a size above MaxBufferSize", before["#big"] == (True, 450), repr(before))
    check(scn, "without the module the explicit size is lost on cycle", after["#a"] == (False, DEFAULT_BUF), repr(after))
    check(scn, "and the over-limit size too", after["#big"] == (False, DEFAULT_BUF), repr(after))
    check(scn, "playback holds only the default number of lines", pb == DEFAULT_BUF, repr(pb))


# --------------------------------------------------------------------------
# cy_* - cycles with the module loaded
# --------------------------------------------------------------------------

def cy_part_join():
    """PART, wait for the server's PART, JOIN: the explicit size comes back,
    playback uses it, the NV store holds it, and the untouched channel keeps
    the default without gaining a stored entry."""
    scn = "cy_part_join"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 42")
    cycle(c, "#a")
    cycle(c, "#b")
    after = listchans(c)
    nv = store(z)
    c.quit()
    pb = playback_count(z, "#a")
    finish(z)
    check(scn, "explicit size restored after cycle", after["#a"] == (True, 42), repr(after))
    check(scn, "default channel stays default", after["#b"] == (False, DEFAULT_BUF), repr(after))
    check(scn, "NV holds exactly the explicit channel", nv == {"buf:#a": "42"}, repr(nv))
    check(scn, "playback holds 42 lines", pb == 42, repr(pb))


def cy_fast_cycle():
    """PART and JOIN in one write, as a /cycle alias sends them, and three
    back-to-back cycles: the size is restored every time."""
    scn = "cy_fast_cycle"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 33")
    seen = []
    for _ in range(3):
        c.send_many(["PART #a :cycling", "JOIN #a"])
        wait_line(c, lambda l: re.search(r" 366 \S+ #a ", l), "rejoin")
        c.sync()
        seen.append(buf(c, "#a"))
    c.quit()
    finish(z)
    check(scn, "restored on each of three fast cycles", seen == [(True, 33)] * 3, repr(seen))


def cy_multi_target():
    """PART #a,#b,#c and JOIN #a,#b,#c: each explicit size is restored to its
    own channel; the default channel stays default and gets no entry."""
    scn = "cy_multi_target"
    z = mk(scn, NEW, networks=[Network("n1", chans=["#a", "#b", "#c"], load_lines=[MOD])])
    c = z.client()
    c.cmd("*status", "SetBuffer #a 11")
    c.cmd("*status", "SetBuffer #b 22")
    part(c, "#a,#b,#c")
    gone = listchans(c)
    join(c, "#a,#b,#c")
    after = listchans(c)
    nv = store(z)
    c.quit()
    finish(z)
    check(scn, "all three channels removed by the multi-target PART", gone == {}, repr(gone))
    check(scn, "sizes restored per channel", (after.get("#a"), after.get("#b"), after.get("#c")) == ((True, 11), (True, 22), (False, DEFAULT_BUF)), repr(after))
    check(scn, "NV holds the two explicit channels", nv == {"buf:#a": "11", "buf:#b": "22"}, repr(nv))


def cy_join0():
    """JOIN 0 makes the server part every channel. Rejoining restores each
    explicit size. ZNC itself creates a channel object for the literal
    target "0" (listed as #0) when a client sends JOIN 0; that is recorded,
    not asserted, since it is ZNC's behaviour."""
    scn = "cy_join0"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 15")
    c.cmd("*status", "SetBuffer #b 25")
    c.send("JOIN 0")
    wait_line(c, lambda l: " PART #b" in l, "PART #b")
    c.sync()
    gone = listchans(c)
    join(c, "#a,#b")
    after = listchans(c)
    c.quit()
    finish(z)
    print("    info: channels listed after JOIN 0: %r" % sorted(gone))
    check(scn, "JOIN 0 left every channel", "#a" not in gone and "#b" not in gone, repr(gone))
    check(scn, "both sizes restored", (after.get("#a"), after.get("#b")) == ((True, 15), (True, 25)), repr(after))


def cy_over_max():
    """A channel whose size comes from znc.conf and exceeds MaxBufferSize
    (450 > 300) is restored at 450: restore forces the size, as the config
    loader does. Playback confirms 450 lines are kept."""
    scn = "cy_over_max"
    z = mk(scn, NEW, networks=[Network("n1", chans=["#big"], load_lines=[MOD], chan_settings={"#big": {"Buffer": "450"}})])
    c = z.client()
    before = buf(c, "#big")
    cycle(c, "#big")
    after = buf(c, "#big")
    nv = store(z)
    c.quit()
    pb = playback_count(z, "#big", n=500)
    finish(z)
    check(scn, "450 from znc.conf before the cycle", before == (True, 450), repr(before))
    check(scn, "450 restored after the cycle", after == (True, 450), repr(after))
    check(scn, "NV holds 450", nv == {"buf:#big": "450"}, repr(nv))
    check(scn, "playback holds 450 lines", pb == 450, repr(pb))


def cy_casefold():
    """Keys are RFC 1459 folded. #Foo[x] parted and rejoined as #FOO{X}
    (a different spelling of the same channel) gets its size back, and the
    key is buf:#foo{x}."""
    scn = "cy_casefold"
    z = mk(scn, NEW, networks=[Network("n1", chans=["#Foo[x]"], load_lines=[MOD])])
    c = z.client()
    c.cmd("*status", "SetBuffer #Foo[x] 18")
    part(c, "#Foo[x]")
    nv = store(z)
    join(c, "#FOO{X}")
    after = listchans(c)
    c.quit()
    finish(z)
    check(scn, "key folded with RFC 1459 rules", nv == {"buf:#foo{x}": "18"}, repr(nv))
    check(scn, "restored under the other spelling", after.get("#foo{x}") == (True, 18), repr(after))


# --------------------------------------------------------------------------
# fb_* - fallback paths
# --------------------------------------------------------------------------

def fb_server_part():
    """The server parts ZNC on its own (no client PART), as SAPART does: the
    channel object goes away, the size is remembered from the server's PART,
    and a later JOIN restores it."""
    scn = "fb_server_part"
    z = mk(scn, NEW)
    d = z.net().ircd
    c = z.client()
    c.cmd("*status", "SetBuffer #a 27")
    d.force_part("#a")
    d.sync()
    c.sync()
    gone = "#a" not in listchans(c)
    nv = store(z)
    join(c, "#a")
    after = buf(c, "#a")
    c.quit()
    finish(z)
    check(scn, "channel removed by the server's PART", gone, "")
    check(scn, "size remembered from the server's PART", nv == {"buf:#a": "27"}, repr(nv))
    check(scn, "restored on JOIN", after == (True, 27), repr(after))


def fb_kick_then_part():
    """A KICK leaves the channel object in place (disabled, not joined) with
    its size, and the module stores the size. A client PART of the kicked
    channel makes ZNC remove it locally without asking the server; a later
    JOIN restores the size."""
    scn = "fb_kick_then_part"
    z = mk(scn, NEW)
    d = z.net().ircd
    c = z.client()
    c.cmd("*status", "SetBuffer #a 31")
    d.kick("#a")
    d.sync()
    c.sync()
    kicked = buf(c, "#a")
    nv_kick = store(z)
    c.send("PART #a")
    wait_line(c, lambda l: "Removing channel #a" in l, "Removing channel")
    c.sync()
    gone = "#a" not in listchans(c)
    join(c, "#a")
    after = buf(c, "#a")
    c.quit()
    finish(z)
    check(scn, "kicked channel keeps its object and size", kicked == (True, 31), repr(kicked))
    check(scn, "size stored on KICK", nv_kick == {"buf:#a": "31"}, repr(nv_kick))
    check(scn, "client PART removes the kicked channel", gone, "")
    check(scn, "restored on JOIN", after == (True, 31), repr(after))


def fb_invalid_values():
    """Stored values that are not a number from 1 to 4294967295 (possible
    only by editing the registry) are never applied: 0, a number beyond 32
    bits that would wrap to 1, text, and a negative number. The next PART
    of each channel, which then has no explicit size, deletes its entry."""
    scn = "fb_invalid_values"
    chans = ["#a", "#b", "#c", "#d"]
    seed = {"buf:#a": "0", "buf:#b": "4294967297", "buf:#c": "abc", "buf:#d": "-5"}
    z = mk(scn, NEW, networks=[Network("n1", chans=chans, load_lines=[MOD])], registries={"n1": {MOD: seed}})
    c = z.client()
    before = listchans(c)
    for ch in chans:
        part(c, ch)
    nv = store(z)
    c.quit()
    finish(z)
    check(scn, "none applied", before == {ch: (False, DEFAULT_BUF) for ch in chans}, repr(before))
    check(scn, "entries removed when the default channels are parted", nv == {}, repr(nv))


# --------------------------------------------------------------------------
# cmd_* - commands
# --------------------------------------------------------------------------

CMD_CASES = [
    ("Status", ["Remembered channels: 0"]),
    ("List", ["No remembered channels."]),
    ("Version", ["keepchanbuffersize version %s" % NEW_VER]),
    ("Set", ["Usage: Set <#chan> <lines>"]),
    ("Set #c", ["Usage: Set <#chan> <lines>"]),
    ("Set c 5", ["Invalid channel name: c (must begin with a channel prefix such as '#')."]),
    ("Set &c 5", ["Invalid channel name: &c (must begin with a channel prefix such as '#')."]),
    ("Set #c -1", ["Invalid buffer size: -1 (must be a positive integer, digits only)."]),
    ("Set #c 10abc", ["Invalid buffer size: 10abc (must be a positive integer, digits only)."]),
    ("Set #c +5", ["Invalid buffer size: +5 (must be a positive integer, digits only)."]),
    ("Set #c 0", ["Invalid buffer size (must be greater than zero)."]),
    ("Set #c 301", ["Invalid buffer size: 301 exceeds ZNC's MaxBufferSize of 300."]),
    # Numbers beyond 32 bits are rejected, never wrapped; the reply echoes
    # what was typed.
    ("Set #c 4294967295", ["Invalid buffer size: 4294967295 exceeds ZNC's MaxBufferSize of 300."]),
    ("Set #c 4294967296", ["Invalid buffer size: 4294967296 exceeds ZNC's MaxBufferSize of 300."]),
    ("Set #w 4294967297", ["Invalid buffer size: 4294967297 exceeds ZNC's MaxBufferSize of 300."]),
    ("Set #c 99999999999999999999", ["Invalid buffer size: 99999999999999999999 exceeds ZNC's MaxBufferSize of 300."]),
    ("Set #c 300", ["Remembered: #c => 300"]),
    ("Set #c 00012", ["Remembered: #c => 12"]),
    ("Set #c 000000000000000000000012", ["Remembered: #c => 12"]),
    ("Set #C[X] 5", ["Remembered: #c{x} => 5"]),
    ("Status", ["Remembered channels: 2"]),
    ("List", ["+---------+--------+", "| Channel | Buffer |", "+---------+--------+",
              "| #c      | 12     |", "| #c{x}   | 5      |", "+---------+--------+"]),
    ("Forget", ["Usage: Forget <#chan>"]),
    ("Forget c", ["Invalid channel name: c (must begin with a channel prefix such as '#')."]),
    ("Forget #nope", ["Nothing stored for #nope"]),
    ("Forget #C{x}", ["Forgot stored buffer size for #c{x}"]),
    ("Forget #c{x}", ["Nothing stored for #c{x}"]),
    ("Enable", ["Unknown command!"]),
    ("Disable", ["Unknown command!"]),
]


def cmd_surface():
    """Every command and its error paths, reply by reply, and the NV store
    the sequence leaves behind. #c and #w are not joined, so nothing is
    applied immediately."""
    scn = "cmd_surface"
    z = mk(scn, NEW)
    c = z.client()
    for i, (cmd, exp) in enumerate(CMD_CASES):
        got = c.cmd("*" + MOD, cmd)
        check(scn, "%02d %s" % (i, cmd), got == exp, "got %r want %r" % (got, exp))
    help_ = c.cmd("*" + MOD, "Help")
    nv = store(z)
    c.quit()
    finish(z)
    names = [re.sub(r"\x02|\x0f", "", h).split(" ")[0].split(":")[0] for h in help_]
    check(scn, "Help lists every command", sorted(names) == ["Forget", "Help", "List", "Set", "Status", "Version"], repr(help_))
    check(scn, "NV after the sequence", nv == {"buf:#c": "12"}, repr(nv))


def cmd_max_zero():
    """MaxBufferSize = 0 allows no buffer size at all in ZNC: its own
    SetBuffer refuses every size. Set refuses in the same way and stores
    nothing. Sizes that ZNC accepted by other routes (znc.conf, a stored
    value) are still restored, because restore forces the size as the
    config loader does."""
    scn = "cmd_max_zero"
    z = mk(scn, NEW, global_settings={"MaxBufferSize": "0"},
           networks=[Network("n1", chans=["#a", "#big"], load_lines=[MOD], chan_settings={"#big": {"Buffer": "450"}})],
           registries={"n1": {MOD: {"buf:#later": "40"}}})
    c = z.client()
    zsb = c.cmd("*status", "SetBuffer #a 5")
    set1 = c.cmd("*" + MOD, "Set #a 5")
    set2 = c.cmd("*" + MOD, "Set #a 1")
    nv0 = store(z)
    cycle(c, "#big")
    big = buf(c, "#big")
    join(c, "#later")
    later = buf(c, "#later")
    c.quit()
    finish(z)
    check(scn, "ZNC's own SetBuffer refuses", zsb == ["Setting buffer size failed for 1 buffer", "Maximum buffer size is 0 lines"], repr(zsb))
    check(scn, "Set refuses", set1 == ["Invalid buffer size: 5 exceeds ZNC's MaxBufferSize of 0."] and
          set2 == ["Invalid buffer size: 1 exceeds ZNC's MaxBufferSize of 0."], repr((set1, set2)))
    check(scn, "nothing stored by Set", "buf:#a" not in nv0, repr(nv0))
    check(scn, "size from znc.conf still restored after a cycle", big == (True, 450), repr(big))
    check(scn, "stored size still applied on JOIN", later == (True, 40), repr(later))


def cmd_set_applies():
    """Set on a joined channel applies at once (ListChans and playback), and
    the value then survives a cycle."""
    scn = "cmd_set_applies"
    z = mk(scn, NEW)
    c = z.client()
    rep = c.cmd("*" + MOD, "Set #A 23")
    now = buf(c, "#a")
    cycle(c, "#a")
    after = buf(c, "#a")
    c.quit()
    pb = playback_count(z, "#a")
    finish(z)
    check(scn, "reply", rep == ["Remembered: #a => 23", "Applied immediately to existing channel object."], repr(rep))
    check(scn, "applied at once", now == (True, 23), repr(now))
    check(scn, "survives a cycle", after == (True, 23), repr(after))
    check(scn, "playback holds 23 lines", pb == 23, repr(pb))


def cmd_set_then_join():
    """Set for a channel ZNC is not in; joining it later applies the value."""
    scn = "cmd_set_then_join"
    z = mk(scn, NEW)
    c = z.client()
    rep = c.cmd("*" + MOD, "Set #new 44")
    join(c, "#new")
    after = buf(c, "#new")
    c.quit()
    finish(z)
    check(scn, "reply has no apply line", rep == ["Remembered: #new => 44"], repr(rep))
    check(scn, "applied on JOIN", after == (True, 44), repr(after))


def cmd_forget_stops_restore():
    """Forget, then a cycle in which the module stores the size again from
    the PART: Forget only clears what is stored now; a channel that still has
    an explicit size is remembered again at its next PART. Recorded so the
    interaction is visible."""
    scn = "cmd_forget_stops_restore"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 29")
    cycle(c, "#a")
    fg = c.cmd("*" + MOD, "Forget #a")
    nv_after_forget = store(z)
    cycle(c, "#a")
    after = buf(c, "#a")
    c.quit()
    finish(z)
    check(scn, "Forget clears the entry", fg == ["Forgot stored buffer size for #a"] and nv_after_forget == {}, repr((fg, nv_after_forget)))
    check(scn, "the next PART stores the live explicit size again", after == (True, 29), repr(after))


# --------------------------------------------------------------------------
# ps_* - persistence and stored-data migration
# --------------------------------------------------------------------------

def ps_restart():
    """Stored sizes survive a graceful restart and a SIGKILL, and are applied
    when the channel is joined after the restart."""
    scn = "ps_restart"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*" + MOD, "Set #later 37")
    c.quit()
    res = []
    for kill in (False, True):
        z.stop(kill=kill)
        z.start()
        c = z.client()
        join(c, "#later")
        res.append(buf(c, "#later"))
        part(c, "#later")
        c.quit()
    finish(z)
    check(scn, "applied after graceful restart", res[0] == (True, 37), repr(res))
    check(scn, "applied after SIGKILL", res[1] == (True, 37), repr(res))


def ps_legacy():
    """A registry written by 1.0.x/1.1.x: the Enabled key is removed, keys
    folded with ASCII rules are rewritten with RFC 1459 rules, and where both
    forms exist the RFC 1459 one wins. Restored values follow."""
    scn = "ps_legacy"
    seed = {"Enabled": "1", "buf:#foo[bar]": "12", "buf:#y[z]": "1", "buf:#y{z}": "2", "buf:#a": "9"}
    z = mk(scn, NEW, networks=[Network("n1", chans=["#a", "#foo[bar]", "#y{z}"], load_lines=[MOD])],
           registries={"n1": {MOD: seed}})
    c = z.client()
    nv = store(z)
    lc = listchans(c)
    c.quit()
    finish(z)
    check(scn, "NV migrated", nv == {"buf:#a": "9", "buf:#foo{bar}": "12", "buf:#y{z}": "2"}, repr(nv))
    check(scn, "migrated values applied", (lc.get("#a"), lc.get("#foo[bar]"), lc.get("#y{z}")) == ((True, 9), (True, 12), (True, 2)), repr(lc))


def ps_load_args():
    """The removed enabled= argument prints the deprecation notice once,
    whatever its value; other arguments are ignored silently. The module is
    active either way."""
    scn = "ps_load_args"
    note = ("Note: the 'enabled=' load argument is deprecated in 1.2.0 and has no effect. The module is "
            "always active while loaded; use UnloadMod to stop it. Stored per-channel values are preserved.")
    res = {}
    for args in ("enabled=0", "ENABLED=off foo=bar", "foo=bar", ""):
        z = mk("%s_%d" % (scn, len(res)), NEW, networks=[Network("n1", chans=["#a"])])
        c = z.client()
        c.cmd("*status", "SetBuffer #a 21")
        c.sync()
        c.send("PRIVMSG *status :LoadMod %s %s" % (MOD, args))
        got = c.sync()
        cycle(c, "#a")
        res[args] = ([x for x in got if "*%s!" % MOD in x], buf(c, "#a"))
        c.quit()
        finish(z)
    for args, (lines, b) in res.items():
        want = 1 if "enabled" in args.lower() else 0
        n_note = sum(1 for l in lines if l.endswith(":" + note))
        check(scn, "LoadMod %r: %d notice(s)" % (args, want), n_note == want and len(lines) == want, repr(lines))
        check(scn, "LoadMod %r: active" % args, b == (True, 21), repr(b))


# --------------------------------------------------------------------------
# ld_* - loading and unloading
# --------------------------------------------------------------------------

def ld_apply_existing():
    """LoadMod while the channels are joined applies stored values at once."""
    scn = "ld_apply_existing"
    z = mk(scn, None, registries={"n1": {MOD: {"buf:#a": "19"}}})
    z.install_modules({MOD: NEW})
    c = z.client()
    before = listchans(c)
    c.cmd("*status", "LoadMod " + MOD)
    after = listchans(c)
    c.quit()
    finish(z)
    check(scn, "nothing explicit before the load", before == {"#a": (False, DEFAULT_BUF), "#b": (False, DEFAULT_BUF)}, repr(before))
    check(scn, "stored value applied on load", after == {"#a": (True, 19), "#b": (False, DEFAULT_BUF)}, repr(after))


def ld_startup_apply():
    """Module in znc.conf with a stored value: the channel comes up with it."""
    scn = "ld_startup_apply"
    z = mk(scn, NEW, registries={"n1": {MOD: {"buf:#b": "16"}}})
    c = z.client()
    lc = listchans(c)
    c.quit()
    finish(z)
    check(scn, "applied at startup", lc == {"#a": (False, DEFAULT_BUF), "#b": (True, 16)}, repr(lc))


def ld_unload_keeps():
    """UnloadMod stops the module (a cycle then loses the size) but keeps
    its NV store; LoadMod brings the stored value back."""
    scn = "ld_unload_keeps"
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 26")
    cycle(c, "#a")
    c.cmd("*status", "UnloadMod " + MOD)
    cycle(c, "#a")
    unloaded = buf(c, "#a")
    nv = store(z)
    c.cmd("*status", "LoadMod " + MOD)
    reloaded = buf(c, "#a")
    c.quit()
    finish(z)
    check(scn, "unloaded module does not restore", unloaded == (False, DEFAULT_BUF), repr(unloaded))
    check(scn, "NV kept while unloaded", nv == {"buf:#a": "26"}, repr(nv))
    check(scn, "LoadMod applies the kept value", reloaded == (True, 26), repr(reloaded))


# --------------------------------------------------------------------------
# ty_* - module type and network isolation
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
    """Network-only: plain LoadMod and --type=network give a network
    instance; --type=user and --type=global are refused."""
    scn = "ty_runtime_load"
    for i, cmd in enumerate(["LoadMod %s" % MOD, "LoadMod --type=network %s" % MOD,
                             "LoadMod --type=user %s" % MOD, "LoadMod --type=global %s" % MOD]):
        z = mk("%s_%d" % (scn, i), None)
        z.install_modules({MOD: NEW})
        c = z.client()
        reply = c.cmd("*status", cmd)
        sections = listmods_sections(c)
        c.quit()
        finish(z)
        if i < 2:
            check(scn, cmd, sections == ["Network"], repr((reply, sections)))
        else:
            check(scn, cmd + " refused", sections == [] and any("does not support" in x for x in reply), repr((reply, sections)))


def ty_isolation():
    """Two networks, same channel name, different sizes: each network's
    instance restores its own value."""
    scn = "ty_isolation"
    nets = [Network("n1", chans=["#a"], load_lines=[MOD]), Network("n2", chans=["#a"], load_lines=[MOD])]
    z = mk(scn, NEW, networks=nets)
    res = {}
    for net, size in (("n1", 13), ("n2", 14)):
        c = z.client(net)
        c.cmd("*status", "SetBuffer #a %d" % size)
        cycle(c, "#a")
        res[net] = (buf(c, "#a"), store(z, net))
        c.quit()
    finish(z)
    check(scn, "n1 keeps its own", res["n1"] == ((True, 13), {"buf:#a": "13"}), repr(res))
    check(scn, "n2 keeps its own", res["n2"] == ((True, 14), {"buf:#a": "14"}), repr(res))


# --------------------------------------------------------------------------
# up_* - /znc updatemod
# --------------------------------------------------------------------------

def up_inplace():
    """UpdateMod to the PROBE build (same code, other version string). If
    the build defines an STB_GNU_UNIQUE symbol the znc binary lacks, glibc
    cannot unload it and the old code keeps running; that is decided per
    build with nm and asserted, not assumed."""
    scn = "up_inplace"
    if not PROBE:
        check(scn, "PROBE build provided", False, "set PROBE; see TESTING.md")
        return
    z = mk(scn, NEW)
    c = z.client()
    c.cmd("*status", "SetBuffer #a 34")
    cycle(c, "#a")
    v0 = c.cmd("*" + MOD, "Version")
    z.install_modules({MOD: PROBE})
    upd = c.cmd("*status", "UpdateMod " + MOD)
    v1 = c.cmd("*" + MOD, "Version")
    cycle(c, "#a")
    after = buf(c, "#a")
    c.quit()
    finish(z)
    stuck = pinned(NEW)
    print("    info: build under test unloadable by glibc in this znc: %s" % ("no (pinned)" if stuck else "yes"))
    exp = NEW_VER if stuck else version_of(PROBE)
    check(scn, "UpdateMod reports Done", upd[-1:] == ["Done"], repr(upd))
    check(scn, "version after UpdateMod is %s" % exp, v0 == ["keepchanbuffersize version %s" % NEW_VER] and v1 == ["keepchanbuffersize version %s" % exp], repr((v0, v1)))
    check(scn, "stored size still restored after the update", after == (True, 34), repr(after))


def sym_unique():
    """The build defines no STB_GNU_UNIQUE symbol absent from znc (such a
    symbol makes /znc updatemod a silent no-op)."""
    scn = "sym_unique"
    extra = sorted(unique_syms(NEW) - unique_syms(ZNC_BIN))
    check(scn, "no unique symbols beyond znc's", not extra, repr(extra))


SCENARIOS = [ctl_without_module, cy_part_join, cy_fast_cycle, cy_multi_target, cy_join0, cy_over_max, cy_casefold,
             fb_server_part, fb_kick_then_part, fb_invalid_values, cmd_surface, cmd_max_zero, cmd_set_applies, cmd_set_then_join,
             cmd_forget_stops_restore, ps_restart, ps_legacy, ps_load_args, ld_apply_existing, ld_startup_apply,
             ld_unload_keeps, ty_runtime_load, ty_isolation, up_inplace, sym_unique]


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
