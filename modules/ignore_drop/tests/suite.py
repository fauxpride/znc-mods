"""ignore_drop test suite: runs a real ZNC against fake IRC servers.

Most scenarios run identical traffic through a baseline build and the build
under test and compare what reaches the client. Behaviour that is meant to
change between the two is asserted explicitly per build; everything else must
match exactly. See ../TESTING.md.
"""
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hx import Network, Znc, deliveries, tokens  # noqa: E402

ZNC_BIN = os.environ.get("ZNC_BIN", "/usr/bin/znc")
BASE = os.environ.get("BASE", "")      # baseline ignore_drop.so
NEW = os.environ.get("NEW", "")        # ignore_drop.so under test
PROBE = os.environ.get("PROBE", "")    # NEW rebuilt with a different version string
if not BASE or not NEW:
    raise SystemExit("set BASE (baseline .so) and NEW (.so under test); see TESTING.md")
EXTRA_ENV = {}
if os.environ.get("ZNC_PRELOAD"):
    EXTRA_ENV["LD_PRELOAD"] = os.environ["ZNC_PRELOAD"]
    EXTRA_ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "")
    EXTRA_ENV["UBSAN_OPTIONS"] = os.environ.get("UBSAN_OPTIONS", "")
ROOT = os.environ.get("HX_ROOT", "/tmp/ignore_drop-tests")
ONLY = set(a for a in sys.argv[1:] if not a.startswith("-"))

results = []  # (scenario, check, ok, detail)
open_zncs = []


def check(scn, name, ok, detail=""):
    results.append((scn, name, bool(ok), detail))
    if not ok:
        print("    FAIL %s :: %s :: %s" % (scn, name, detail))


def mk(name, so, networks=None, user_load=(), user_settings=None, registries=None, start=True,
       expect_ready=True, extra_mods=None, global_load=()):
    if networks is None:
        networks = [Network("n1", load_lines=["ignore_drop"])]
    mods = {"ignore_drop": so}
    mods.update(extra_mods or {})
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, mods, networks, user_load_lines=user_load,
            user_settings=user_settings, registries=registries, extra_env=EXTRA_ENV,
            global_load_lines=global_load)
    open_zncs.append(z)
    if start:
        z.start(expect_ready=expect_ready)
    return z


SANITIZER_MARKERS = ("AddressSanitizer", "runtime error:", "LeakSanitizer", "SUMMARY:")


def finish(z, expect_crash=False):
    try:
        z.close()
    except Exception:
        pass
    out = z.output()
    if not expect_crash:
        for bad in SANITIZER_MARKERS + ("Segmentation",):
            if bad in out:
                check("sanitizer", "%s clean (%s)" % (os.path.basename(z.workdir), bad), False, out[-3000:])
    if z in open_zncs:
        open_zncs.remove(z)


def version_of(so):
    """Version a build reports, read from its module description without
    loading it."""
    data = open(so, "rb").read()
    m = re.search(rb"server-side ignore \(v(\d+\.\d+\.\d+)\)", data)
    return m.group(1).decode() if m else None


def ver_tuple(v):
    return tuple(int(x) for x in v.split(".")) if v else None


def unique_syms(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return set(l.split()[-1] for l in out.splitlines() if len(l.split()) >= 3 and l.split()[1] == "u")


def pinned(so):
    """True if glibc will refuse to unload this build inside ZNC_BIN: it
    defines an STB_GNU_UNIQUE symbol the znc binary does not."""
    return bool(unique_syms(so) - unique_syms(ZNC_BIN))


BASE_VER, NEW_VER = version_of(BASE), version_of(NEW)
# Releases whose module type is network by default (the fix in 1.1.1).
BASE_TYPE_FIXED = BASE_VER is not None and ver_tuple(BASE_VER) >= (1, 1, 1)


def both(fn):
    """Run fn on both builds; returns (baseline_result, new_result)."""
    return fn(BASE, "base"), fn(NEW, "new")


def say_all(d, who, marker_prefix, target_chan="#a", me="tim", kinds="TNA"):
    """One line of each kind, to the channel and privately, all carrying
    markers <prefix>1..<prefix>6."""
    i = 0
    for target in (target_chan, me):
        for k in kinds:
            i += 1
            d.say(target, who, "%s%d %s" % (marker_prefix, i, "text"), kind=k)


# --------------------------------------------------------------------------
# lv_* - live filtering while a client is attached
# --------------------------------------------------------------------------

def lv_kinds():
    """An always-scope nick mask drops channel and private PRIVMSG, NOTICE and
    ACTION from the sender; other senders pass. Other CTCP requests and
    membership changes are not filtered (the module hooks messages only),
    which is recorded so a change shows up here."""
    scn = "lv_kinds"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        add = c.cmd("*ignore_drop", "Add mallory always")
        say_all(d, "mallory", "M")
        say_all(d, "alice", "A")
        d.say("#a", "mallory", "HXTEST C1", kind="C")
        d.say("tim", "mallory", "HXTEST C2", kind="C")
        d.raw(":mallory!mallory@mallory.host PART #a :J1 bye")
        d.raw(":mallory!mallory@mallory.host JOIN #a")
        d.sync()
        got = c.sync()
        c.quit(); finish(z)
        return dict(add=add, toks=tokens(got),
                    part="J1" in " ".join(got),
                    join=any(" JOIN " in l and l.startswith(":mallory") for l in got))

    b, n = both(run)
    check(scn, "Add confirms", n["add"] == ["Added: mallory [always]"], repr(n["add"]))
    exp = ["A1", "A2", "A3", "A4", "A5", "A6", "C1", "C2"]
    check(scn, "only alice's messages and mallory's CTCP requests delivered", n["toks"] == exp, repr(n["toks"]))
    check(scn, "PART and JOIN from an ignored sender are not filtered", n["part"] and n["join"], repr(n))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def lv_masks():
    """Nick-only masks match the nick only; masks with ! or @ match the full
    nick!ident@host; * and ? wildcards."""
    scn = "lv_masks"
    masks = ["mal*", "*!*@bad.host", "*!~evil@*", "sp?m", "bad"]
    senders = [
        ("mallory!m@x.host", True),     # mal*
        ("malice!m@x.host", True),      # mal*
        ("amal!a@x.host", False),       # mal* is anchored
        ("carol!c@bad.host", True),     # host mask
        ("bob!b@notbad.host", False),   # host mask is anchored after @
        ("dave!~evil@y.host", True),    # ident mask
        ("dave!evil@y.host", False),    # ~ is literal
        ("spam!s@z.host", True),        # sp?m
        ("spaam!s@z.host", False),      # ? is exactly one character
        ("spm!s@z.host", False),
        ("erin!e@bad", False),          # nick-only "bad" never looks at the host
        ("bad!e@ok.host", True),        # ... only at the nick
    ]

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        for m in masks:
            c.cmd("*ignore_drop", "Add %s" % m)
        for i, (who, _) in enumerate(senders):
            d.say("#a", who, "S%d" % (i + 1))
        d.sync()
        toks = tokens(c.sync())
        tests = [c.cmd("*ignore_drop", "Test %s" % who) for who, _ in senders]
        c.quit(); finish(z)
        return dict(toks=toks, tests=tests)

    b, n = both(run)
    exp = ["S%d" % (i + 1) for i, (_, drop) in enumerate(senders) if not drop]
    check(scn, "exactly the non-matching senders delivered", n["toks"] == exp, "got=%r exp=%r" % (n["toks"], exp))
    exp_t = [["Live: DROP | Playback: DROP"] if drop else ["Live: pass | Playback: pass"] for _, drop in senders]
    check(scn, "Test agrees with live filtering", n["tests"] == exp_t, repr(n["tests"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def lv_casefold():
    """RFC 1459 folding on both sides: A-Z/a-z and [ ] \\ ~ / { } | ^."""
    scn = "lv_casefold"
    masks = ["Mallory", "[bot]*", "a\\b", "z~", "*!*@CAPS.HOST"]
    senders = [
        ("MALLORY", True), ("mallory", True), ("{BOT}x", True), ("[bot]y", True),
        ("bot", False), ("A|B", True), ("a\\b", True), ("Z^", True), ("z~", True),
        ("zz^", False), ("pat!p@caps.host", True), ("pat!p@caps.hosts", False),
    ]

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        adds = [c.cmd("*ignore_drop", "Add %s" % m) for m in masks]
        lst = c.cmd("*ignore_drop", "List")
        for i, (who, _) in enumerate(senders):
            d.say("#a", who, "F%d" % (i + 1))
        d.sync()
        toks = tokens(c.sync())
        c.quit(); finish(z)
        return dict(adds=adds, lst=lst, toks=toks)

    b, n = both(run)
    exp = ["F%d" % (i + 1) for i, (_, drop) in enumerate(senders) if not drop]
    check(scn, "folded matches dropped, others delivered", n["toks"] == exp, "got=%r exp=%r" % (n["toks"], exp))
    check(scn, "List shows the folded masks",
          n["lst"][:5] == ["1) mallory [always|nick]", "2) {bot}* [always|nick]", "3) a|b [always|nick]",
                           "4) z^ [always|nick]", "5) *!*@caps.host [always|host]"], repr(n["lst"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


# --------------------------------------------------------------------------
# sc_* - scopes, attach state and playback
# --------------------------------------------------------------------------

def sc_detached_live():
    """detached-scope rules drop live traffic only while no client is
    attached; always-scope rules drop it in both states. What is dropped while
    detached never reaches the buffer, so it is absent from playback."""
    scn = "sc_detached_live"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        c.cmd("*ignore_drop", "Add bob detached")
        c.cmd("*ignore_drop", "Add mallory always")
        d.say("#a", "bob", "B1 attached")
        d.say("#a", "mallory", "M1 attached")
        d.say("#a", "alice", "A1 attached")
        d.say("tim", "bob", "B2 attached private")
        d.sync()
        live = tokens(c.sync())
        c.quit()
        d.say("#a", "bob", "B3 detached")
        d.say("#a", "mallory", "M2 detached")
        d.say("#a", "alice", "A2 detached")
        d.say("tim", "bob", "B4 detached private")
        d.say("tim", "alice", "A3 detached private")
        d.sync()
        c = z.client()
        play = tokens(c.attach_lines)
        c.quit(); finish(z)
        return dict(live=live, play=play)

    b, n = both(run)
    check(scn, "attached: detached-scope sender passes, always-scope dropped", n["live"] == ["B1", "A1", "B2"], repr(n["live"]))
    check(scn, "detached: both dropped, so playback holds alice only", n["play"] == ["A2", "A3"], repr(n["play"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def sc_playback():
    """Playback filtering of lines already in the buffer. always-scope rules
    always hide; detached-scope rules hide only with SetDetachedPlayback on.
    AutoClearChanBuffer/AutoClearQueryBuffer are off so lines are buffered
    while a client is attached, before the rules exist."""
    scn = "sc_playback"

    def run(so, tag):
        z = mk(scn + "_" + tag, so, user_settings={"AutoClearChanBuffer": "false", "AutoClearQueryBuffer": "false"})
        d = z.net().ircd
        keep = z.client()   # stays attached throughout
        d.say("#a", "mallory", "P1")
        d.say("#a", "bob", "P2")
        d.say("#a", "alice", "P3")
        d.say("tim", "mallory", "Q1")
        d.say("tim", "bob", "Q2")
        d.say("tim", "alice", "Q3")
        d.sync()
        live = tokens(keep.sync())
        keep.cmd("*ignore_drop", "Add mallory always")
        keep.cmd("*ignore_drop", "Add bob detached")
        c1 = z.client()
        off = tokens(c1.attach_lines)
        c1.quit()
        flag = keep.cmd("*ignore_drop", "SetDetachedPlayback on")
        c2 = z.client()
        on = tokens(c2.attach_lines)
        c2.quit()
        keep.quit(); finish(z)
        return dict(live=live, off=off, on=on, flag=flag)

    b, n = both(run)
    check(scn, "all six delivered live before any rule", n["live"] == ["P1", "P2", "P3", "Q1", "Q2", "Q3"], repr(n["live"]))
    check(scn, "playback, detached playback off: always hidden, detached shown",
          sorted(n["off"]) == ["P2", "P3", "Q2", "Q3"], repr(n["off"]))
    check(scn, "playback, detached playback on: both hidden", sorted(n["on"]) == ["P3", "Q3"], repr(n["on"]))
    check(scn, "SetDetachedPlayback confirms", n["flag"] == ["Detached playback set to on"], repr(n["flag"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def sc_setscope():
    """SetScope moves an entry between scopes and the live behaviour follows."""
    scn = "sc_setscope"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        out = [c.cmd("*ignore_drop", x) for x in ("Add bob", "Add *!*@h.host detached", "List")]
        d.say("#a", "bob", "X1")
        d.sync()
        t1 = tokens(c.sync())
        out += [c.cmd("*ignore_drop", x) for x in ("SetScope 1 detached", "List")]
        d.say("#a", "bob", "X2")
        d.sync()
        t2 = tokens(c.sync())
        out += [c.cmd("*ignore_drop", x) for x in ("SetScope 2 always", "List")]
        d.say("#a", "bob", "X3")
        d.say("#a", "zed!z@h.host", "X4")
        d.sync()
        t3 = tokens(c.sync())
        c.quit(); finish(z)
        return dict(out=out, t=[t1, t2, t3])

    b, n = both(run)
    check(scn, "always drops, detached passes while attached, back to always",
          n["t"] == [[], ["X2"], ["X3"]], repr(n["t"]))
    check(scn, "List after moves",
          n["out"][-1] == ["1) *!*@h.host [always|host]", "2) bob [detached|nick]", "Detached playback: off"], repr(n["out"][-1]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def sc_multi_client():
    """The network counts as attached while any client is connected to it."""
    scn = "sc_multi_client"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c1 = z.client()
        c2 = z.client()
        c1.cmd("*ignore_drop", "Add bob detached")
        c1.quit()
        d.say("#a", "bob", "B1 one client left")
        d.sync()
        t = tokens(c2.sync())
        c2.quit()
        d.say("#a", "bob", "B2 none left")
        d.say("#a", "alice", "A1 none left")
        d.sync()
        c3 = z.client()
        p = tokens(c3.attach_lines)
        c3.quit(); finish(z)
        return dict(t=t, p=p)

    b, n = both(run)
    check(scn, "still attached with one client", n["t"] == ["B1"], repr(n["t"]))
    check(scn, "detached with none", n["p"] == ["A1"], repr(n["p"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


# --------------------------------------------------------------------------
# cmd_* - command surface
# --------------------------------------------------------------------------

SURFACE = ["Help", "Version", "List", "Add", "Add A|x", "Add d|x", "Add mallory", "Add Mallory!*@* detached",
           "Add *!*@bad.host always", "Add foo bogus", "Add bar a", "Add baz d", "Add qux both", "List",
           "Test", "Test mallory!m@h", "Test x!y@bad.host", "Test nobody!n@n", "Test baz!b@h",
           "Del", "Del 99", "Del 0", "Del nosuch", "Del MALLORY", "List",
           "SetScope", "SetScope x always", "SetScope 1", "SetScope 1 bogus", "SetScope 99 always",
           "SetScope 1 detached", "List", "SetDetachedPlayback", "SetDetachedPlayback maybe",
           "SetDetachedPlayback on", "List", "Test baz!b@h", "SetDetachedPlayback off", "Clear", "List", "Bogus"]


def norm(lines):
    return [re.sub(r"\b\d+\.\d+\.\d+\b", "X.Y.Z", x) for x in lines]


def cmd_surface():
    """Every command, valid and invalid, with output compared line by line.
    Only the version number may differ."""
    scn = "cmd_surface"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        c = z.client()
        out = [(x, c.cmd("*ignore_drop", x)) for x in SURFACE]
        c.quit(); finish(z)
        return out

    b, n = both(run)
    for (cmd, lb), (_, ln) in zip(b, n):
        check(scn, "'%s' identical" % cmd, norm(lb) == norm(ln), "base=%r new=%r" % (lb, ln))
    ver = dict(n)["Version"]
    check(scn, "Version reports the build's version", ver == ["ignore_drop version %s" % NEW_VER], repr(ver))
    check(scn, "Help lists every command",
          all(any(w in x for x in dict(n)["Help"]) for w in
              ("Add", "Del", "List", "Clear", "SetScope", "SetDetachedPlayback", "Test", "Version")), repr(dict(n)["Help"]))


def cmd_index_edge():
    """Indexes SetScope and Del must reject. Up to 1.1.0, SetScope 0 read
    element [-1] and crashed ZNC, and CString::ToUInt wrapped 4294967296 to 0
    (crash) and 4294967297 to 1 (acts on entry #1)."""
    scn = "cmd_index_edge"

    def crash_probe(so, tag, cmd):
        z = mk("%s_%s_%s" % (scn, tag, cmd.split()[1]), so)
        c = z.client()
        c.cmd("*ignore_drop", "Add bob")
        try:
            out = c.cmd("*ignore_drop", cmd)
        except (RuntimeError, OSError):
            out = None
        time.sleep(0.3)
        alive = z.alive()
        if alive:
            c.quit()
        finish(z, expect_crash=not alive)
        return out, alive

    def wrap_probe(so, tag):
        z = mk("%s_%s_wrap" % (scn, tag), so)
        c = z.client()
        c.cmd("*ignore_drop", "Add bob")
        c.cmd("*ignore_drop", "Add carol")
        out = [c.cmd("*ignore_drop", x) for x in ("SetScope 4294967297 detached", "Del 4294967297", "Del 01", "List")]
        c.quit(); finish(z)
        return out

    for cmd in ("SetScope 0 always", "SetScope 4294967296 always"):
        out, alive = crash_probe(NEW, "new", cmd)
        check(scn, "'%s' rejected, ZNC alive" % cmd, alive and out == ["Index out of range."], repr((out, alive)))
        if not BASE_TYPE_FIXED:
            out, alive = crash_probe(BASE, "base", cmd)
            check(scn, "baseline: '%s' crashes ZNC (the bug fixed here)" % cmd, not alive, repr((out, alive)))
    n = wrap_probe(NEW, "new")
    check(scn, "wrapped indexes rejected, plain leading zeros accepted",
          n == [["Index out of range."], ["Not found."], ["Deleted."], ["1) carol [always|nick]", "Detached playback: off"]], repr(n))
    if not BASE_TYPE_FIXED:
        b = wrap_probe(BASE, "base")
        check(scn, "baseline: wrapped indexes act on entry #1", b[0] == ["Scope updated."] and b[1] == ["Deleted."], repr(b))


# --------------------------------------------------------------------------
# ps_* - persistence
# --------------------------------------------------------------------------

def ps_restart():
    """Masks and the playback flag survive a graceful restart and a SIGKILL,
    stored folded with their scope prefix."""
    scn = "ps_restart"

    def run(so, tag):
        z = mk(scn + "_" + tag, so)
        d = z.net().ircd
        c = z.client()
        for x in ("Add Mallory", "Add *!*@BAD.host detached", "Add [x]", "SetDetachedPlayback on"):
            c.cmd("*ignore_drop", x)
        before = c.cmd("*ignore_drop", "List")
        c.quit()
        reg = z.registry("n1")
        z.stop()
        z.start()
        c = z.client()
        after_restart = c.cmd("*ignore_drop", "List")
        c.cmd("*ignore_drop", "Del 3")
        c.quit()
        z.stop(kill=True)
        z.start()
        c = z.client()
        after_kill = c.cmd("*ignore_drop", "List")
        d.say("#a", "MALLORY", "M1")
        d.say("#a", "alice", "A1")
        d.sync()
        toks = tokens(c.sync())
        c.quit(); finish(z)
        return dict(before=before, reg=reg, after_restart=after_restart, after_kill=after_kill, toks=toks)

    b, n = both(run)
    check(scn, "registry stores folded masks with scope prefixes",
          n["reg"] == {"masks": "A|mallory\nA|{x}\nD|*!*@bad.host", "detached_playback": "1"}, repr(n["reg"]))
    check(scn, "List unchanged by graceful restart", n["after_restart"] == n["before"], repr(n))
    check(scn, "Del persisted across SIGKILL",
          n["after_kill"] == ["1) mallory [always|nick]", "2) {x} [always|nick]", "Detached playback: on"], repr(n["after_kill"]))
    check(scn, "rules active after restart", n["toks"] == ["A1"], repr(n["toks"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


def ps_legacy():
    """Loading NV written by older versions: unprefixed rows (1.0.0) load as
    always and are re-folded; rows with an unknown prefix are skipped; the
    stored text is only rewritten by the next change."""
    scn = "ps_legacy"
    legacy = "Mallory\nD|Bob\nZ|bad\n\nA|[Bot]*\nX|\nA|"

    def run(so, tag):
        z = mk(scn + "_" + tag, so, registries={"n1": {"ignore_drop": {"masks": legacy, "detached_playback": "1"}}})
        c = z.client()
        lst = c.cmd("*ignore_drop", "List")
        reg_loaded = z.registry("n1")
        c.cmd("*ignore_drop", "Add carol")
        reg_saved = z.registry("n1")
        c.quit(); finish(z)
        return dict(lst=lst, reg_loaded=reg_loaded, reg_saved=reg_saved)

    b, n = both(run)
    check(scn, "legacy rows loaded, malformed ones skipped",
          n["lst"] == ["1) mallory [always|nick]", "2) {bot}* [always|nick]", "3) bob [detached|nick]", "Detached playback: on"],
          repr(n["lst"]))
    check(scn, "load does not rewrite storage", n["reg_loaded"]["masks"] == legacy, repr(n["reg_loaded"]))
    check(scn, "next change rewrites it normalised",
          n["reg_saved"]["masks"] == "A|mallory\nA|{bot}*\nA|carol\nD|bob", repr(n["reg_saved"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


# --------------------------------------------------------------------------
# ty_* - module type (the fix in 1.1.1)
# --------------------------------------------------------------------------

def listmods_sections(c):
    """Which ListMods sections name ignore_drop: a list of section headers."""
    out = c.cmd("*status", "ListMods")
    sections, cur = [], None
    for x in out:
        m = re.match(r"^(Global|User|Network) modules:", x)
        if m:
            cur = m.group(1)
        elif "ignore_drop" in x and cur and cur not in sections:
            sections.append(cur)
    return sections, out


def ty_runtime_load():
    """Runtime LoadMod with each --type, and without one. The module must be
    network-only: plain LoadMod gives a network instance, user and global are
    refused."""
    scn = "ty_runtime_load"
    cases = ["LoadMod ignore_drop", "LoadMod --type=network ignore_drop",
             "LoadMod --type=user ignore_drop", "LoadMod --type=global ignore_drop"]

    def run(so, tag):
        res = {}
        for i, cmd in enumerate(cases):
            z = mk("%s_%s_%d" % (scn, tag, i), so, networks=[Network("n1")])
            c = z.client()
            # The reply can name the module path, which contains the tag.
            reply = [x.replace("_%s_" % tag, "_X_") for x in c.cmd("*status", cmd)]
            sections, raw = listmods_sections(c)
            works = None
            if sections:
                d = z.net().ircd
                c.cmd("*ignore_drop", "Add mallory")
                d.say("#a", "mallory", "M1")
                d.say("#a", "alice", "A1")
                d.sync()
                works = tokens(c.sync()) == ["A1"]
            c.quit(); finish(z)
            res[cmd] = dict(reply=reply, sections=sections, works=works)
        return res

    b, n = both(run)
    check(scn, "plain LoadMod loads a network instance", n[cases[0]]["sections"] == ["Network"] and n[cases[0]]["works"], repr(n[cases[0]]))
    check(scn, "--type=network loads a network instance", n[cases[1]]["sections"] == ["Network"] and n[cases[1]]["works"], repr(n[cases[1]]))
    check(scn, "--type=user refused", n[cases[2]]["sections"] == [] and any("does not support" in x for x in n[cases[2]]["reply"]), repr(n[cases[2]]))
    check(scn, "--type=global refused", n[cases[3]]["sections"] == [] and any("does not support" in x for x in n[cases[3]]["reply"]), repr(n[cases[3]]))
    check(scn, "--type=network identical to baseline", b[cases[1]] == n[cases[1]], "base=%r new=%r" % (b[cases[1]], n[cases[1]]))
    check(scn, "--type=global identical to baseline", b[cases[3]] == n[cases[3]], "base=%r new=%r" % (b[cases[3]], n[cases[3]]))
    if not BASE_TYPE_FIXED:
        check(scn, "baseline: plain LoadMod gives a user instance (the bug)", b[cases[0]]["sections"] == ["User"], repr(b[cases[0]]))
        check(scn, "baseline: --type=user accepted (the bug)", b[cases[2]]["sections"] == ["User"], repr(b[cases[2]]))


def loadmodule_lines(cfg):
    """(user-level line present, number of network-level lines) for
    ignore_drop in a znc.conf, whatever its indentation."""
    user_line, net_lines, in_net = False, 0, False
    for ln in cfg.splitlines():
        t = ln.strip()
        if t.lower().startswith("<network "):
            in_net = True
        elif t.lower() == "</network>":
            in_net = False
        elif re.match(r"^LoadModule\s*=\s*ignore_drop\b", t):
            if in_net:
                net_lines += 1
            else:
                user_line = True
    return user_line, net_lines


def ty_conf_user_line():
    """A user-level 'LoadModule = ignore_drop' in znc.conf, as left behind by a
    user-scope instance. ZNC must start; with a network-only module it loads
    one instance per network, copying the user-level NV into each network
    that has none of its own. SaveConfig then records network-level lines."""
    scn = "ty_conf_user_line"

    def run(so, tag):
        nets = [Network("n1"), Network("n2")]
        z = mk(scn + "_" + tag, so, networks=nets, user_load=["ignore_drop"],
               registries={"user": {"ignore_drop": {"masks": "A|mallory"}},
                           "n2": {"ignore_drop": {"masks": "A|carol"}}})
        res = {"notice": "is a network module, loading module for all networks" in z.output()}
        for net in ("n1", "n2"):
            d = z.net(net).ircd
            c = z.client(net)
            res[net + "_sections"] = listmods_sections(c)[0]
            for who in ("mallory", "carol", "alice"):
                d.say("#a", who, "%s1" % who[0].upper())
            d.sync()
            res[net + "_toks"] = tokens(c.sync())
            if net == "n1":
                c.cmd("*status", "SaveConfig")
            c.quit()
        res["user_line"], res["net_lines"] = loadmodule_lines(z.config_text())
        res["n1_reg"] = (z.registry("n1") or {}).get("masks")
        if res["net_lines"] == 2:
            z.stop()
            z.start()
            c = z.client("n1")
            res["after_restart"] = c.cmd("*ignore_drop", "List")
            c.quit()
        finish(z)
        return res

    b, n = both(run)
    check(scn, "ZNC migrates the line, logging that it is a network module", n["notice"], repr(n))
    check(scn, "an instance on each network", n["n1_sections"] == ["Network"] and n["n2_sections"] == ["Network"], repr(n))
    check(scn, "network without its own NV gets the user-level masks", n["n1_toks"] == ["C1", "A1"] and n["n1_reg"] == "A|mallory", repr(n))
    check(scn, "network with its own NV keeps it", n["n2_toks"] == ["M1", "A1"], repr(n["n2_toks"]))
    check(scn, "SaveConfig writes network-level lines only", not n["user_line"] and n["net_lines"] == 2, repr(n))
    check(scn, "migrated config restarts cleanly", n.get("after_restart") == ["1) mallory [always|nick]", "Detached playback: off"], repr(n.get("after_restart")))
    if not BASE_TYPE_FIXED:
        check(scn, "baseline: one user-scope instance shared by both networks (the bug)",
              b["n1_sections"] == ["User"] and b["n1_toks"] == ["C1", "A1"] and b["n2_toks"] == ["C1", "A1"] and b["user_line"], repr(b))


def ty_isolation():
    """Each network has its own instance and its own rules."""
    scn = "ty_isolation"

    def run(so, tag):
        nets = [Network("n1", load_lines=["ignore_drop"]), Network("n2", load_lines=["ignore_drop"])]
        z = mk(scn + "_" + tag, so, networks=nets)
        c1 = z.client("n1")
        c2 = z.client("n2")
        c1.cmd("*ignore_drop", "Add mallory")
        c2.cmd("*ignore_drop", "Add carol detached")
        lists = (c1.cmd("*ignore_drop", "List"), c2.cmd("*ignore_drop", "List"))
        c2.quit()
        res = dict(lists=lists)
        for net in ("n1", "n2"):
            d = z.net(net).ircd
            for who in ("mallory", "carol", "alice"):
                d.say("#a", who, "%s1" % who[0].upper())
            d.sync()
        res["n1"] = tokens(c1.sync())
        c2 = z.client("n2")
        res["n2_play"] = tokens(c2.attach_lines)
        c1.quit(); c2.quit(); finish(z)
        return res

    b, n = both(run)
    check(scn, "n1 drops only its own rule while n2 is detached", n["n1"] == ["C1", "A1"], repr(n["n1"]))
    check(scn, "n2 (detached) drops only its own rule", n["n2_play"] == ["M1", "A1"], repr(n["n2_play"]))
    check(scn, "identical to baseline", b == n, "base=%r new=%r" % (b, n))


# --------------------------------------------------------------------------
# up_* - /znc updatemod
# --------------------------------------------------------------------------

def swap_and_update(z, c, so):
    z.install_modules({"ignore_drop": so})
    return c.cmd("*status", "UpdateMod ignore_drop")


def up_inplace():
    """UpdateMod replaces the running code: Version reports the new build and
    rules survive. Needs PROBE, the build under test recompiled with another
    version string."""
    scn = "up_inplace"
    if not PROBE:
        check(scn, "PROBE build provided", False, "set PROBE; see TESTING.md")
        return
    z = mk(scn, NEW)
    d = z.net().ircd
    c = z.client()
    c.cmd("*ignore_drop", "Add mallory")
    v0 = c.cmd("*ignore_drop", "Version")
    upd = swap_and_update(z, c, PROBE)
    v1 = c.cmd("*ignore_drop", "Version")
    lst = c.cmd("*ignore_drop", "List")
    d.say("#a", "mallory", "M1")
    d.say("#a", "alice", "A1")
    d.sync()
    toks = tokens(c.sync())
    sections = listmods_sections(c)[0]
    c.quit(); finish(z)
    check(scn, "UpdateMod reported success", upd[-1:] == ["Done"], repr(upd))
    check(scn, "new code running after UpdateMod", v0 != v1 and v1 == ["ignore_drop version %s" % version_of(PROBE)], repr((v0, v1)))
    check(scn, "rules and type survive", lst[0] == "1) mallory [always|nick]" and toks == ["A1"] and sections == ["Network"], repr((lst, toks, sections)))


def up_from_baseline():
    """Upgrading a running baseline with UpdateMod. If the baseline defines an
    STB_GNU_UNIQUE symbol that the znc binary lacks (1.1.0 built with GCC 11
    does), glibc never unloads it: UpdateMod reports success but the old code
    stays, until ZNC restarts. Asserted per build rather than assumed."""
    scn = "up_from_baseline"
    z = mk(scn, BASE)
    c = z.client()
    c.cmd("*ignore_drop", "Add mallory")
    upd = swap_and_update(z, c, NEW)
    v1 = c.cmd("*ignore_drop", "Version")
    c.quit()
    z.stop()
    z.start()
    c = z.client()
    v2 = c.cmd("*ignore_drop", "Version")
    lst = c.cmd("*ignore_drop", "List")
    c.quit(); finish(z)
    stuck = pinned(BASE)
    print("    info: baseline %s unloadable by glibc in this znc: %s" % (BASE_VER, "no (pinned)" if stuck else "yes"))
    exp_v1 = BASE_VER if stuck else NEW_VER
    check(scn, "UpdateMod runs %s code (%s)" % (exp_v1, "baseline pinned" if stuck else "baseline unloadable"),
          upd[-1:] == ["Done"] and v1 == ["ignore_drop version %s" % exp_v1], repr((upd, v1)))
    check(scn, "after a restart the new code runs with the rules intact",
          v2 == ["ignore_drop version %s" % NEW_VER] and lst[0] == "1) mallory [always|nick]", repr((v2, lst)))


def up_pinned_by_later_module():
    """ZNC loads modules RTLD_GLOBAL, and every module built from ZNC's
    headers defines the same weak template code (from Info.AddType, among
    others). A module loaded later binds those references to the first copy
    in the global scope. If no global module was loaded first, that copy is
    ignore_drop's, the later module then depends on it, and glibc cannot
    unload it: UpdateMod reports success and the old code keeps running. A
    global module such as corecaps or webadmin, loaded first, provides the
    copy instead. Uses ZNC's own keepnick as the later module."""
    scn = "up_pinned_by_later_module"
    if not PROBE:
        check(scn, "PROBE build provided", False, "set PROBE; see TESTING.md")
        return
    res = {}
    for glob in ((), ("corecaps",)):
        tag = glob[0] if glob else "noglobal"
        z = mk("%s_%s" % (scn, tag), NEW, networks=[Network("n1", load_lines=["ignore_drop", "keepnick"])],
               global_load=glob)
        c = z.client()
        upd = swap_and_update(z, c, PROBE)
        res[tag] = (upd[-1:], c.cmd("*ignore_drop", "Version"))
        c.quit(); finish(z)
    # Workaround without a restart: unload the later module first.
    z = mk("%s_unload_first" % scn, NEW, networks=[Network("n1", load_lines=["ignore_drop", "keepnick"])])
    c = z.client()
    c.cmd("*status", "UnloadMod keepnick")
    upd = swap_and_update(z, c, PROBE)
    res["unload_first"] = (upd[-1:], c.cmd("*ignore_drop", "Version"))
    c.cmd("*status", "LoadMod --type=network keepnick")
    res["reloaded"] = listmods_sections(c)[1]
    c.quit(); finish(z)
    probe_ver = ["ignore_drop version %s" % version_of(PROBE)]
    check(scn, "no global module: UpdateMod reports Done but the old code keeps running",
          res["noglobal"] == (["Done"], ["ignore_drop version %s" % NEW_VER]), repr(res["noglobal"]))
    check(scn, "corecaps loaded first: UpdateMod replaces the code", res["corecaps"] == (["Done"], probe_ver), repr(res["corecaps"]))
    check(scn, "unloading the later module first lets UpdateMod replace the code",
          res["unload_first"] == (["Done"], probe_ver) and any("keepnick" in x for x in res["reloaded"]), repr(res))


def up_user_instance():
    """A user-scope instance (only possible with a baseline that has the type
    bug) is reloaded by UpdateMod as a user module, which the fixed build
    refuses. Characterises what happens so the upgrade notes can be exact."""
    scn = "up_user_instance"
    if BASE_TYPE_FIXED:
        print("    info: baseline already network-only; skipped")
        return
    z = mk(scn, BASE, networks=[Network("n1")])
    c = z.client()
    c.cmd("*status", "LoadMod ignore_drop")
    before = listmods_sections(c)[0]
    upd = swap_and_update(z, c, NEW)
    after = listmods_sections(c)[0]
    ver = c.cmd("*ignore_drop", "Version") if after else []
    c.quit()
    alive = z.alive()
    finish(z)
    stuck = pinned(BASE)
    print("    info: UpdateMod reply %r; sections before=%r after=%r version=%r" % (upd, before, after, ver))
    check(scn, "ZNC survives", alive, "")
    if stuck:
        check(scn, "pinned baseline: the old user-scope instance keeps running", after == ["User"] and ver == ["ignore_drop version %s" % BASE_VER], repr((after, ver)))
    else:
        check(scn, "unloadable baseline: the user-scope instance is not reloaded", after == [], repr((upd, after)))


# --------------------------------------------------------------------------
# sym_* - symbol hygiene (inspects the .so, no ZNC run)
# --------------------------------------------------------------------------

def sym_unique():
    scn = "sym_unique"
    znc_u = unique_syms(ZNC_BIN)
    new_u = unique_syms(NEW) - znc_u
    base_u = unique_syms(BASE) - znc_u
    print("    info: unique symbols not in znc: base=%d new=%d" % (len(base_u), len(new_u)))
    check(scn, "build under test defines no STB_GNU_UNIQUE symbol absent from znc", not new_u, repr(sorted(new_u)))
    check(scn, "no worse than baseline", len(new_u) <= len(base_u), "")
    dem = subprocess.run(["c++filt"], input="\n".join(unique_syms(NEW)), capture_output=True, text=True).stdout
    check(scn, "__to_chars_10_impl not unique-exported", "__to_chars_10_impl" not in dem, dem)


SCENARIOS = [lv_kinds, lv_masks, lv_casefold, sc_detached_live, sc_playback, sc_setscope, sc_multi_client,
             cmd_surface, cmd_index_edge, ps_restart, ps_legacy, ty_runtime_load, ty_conf_user_line,
             ty_isolation, up_inplace, up_from_baseline, up_pinned_by_later_module, up_user_instance, sym_unique]


def selected(fn):
    if not ONLY:
        return True
    name = fn.__name__
    return name in ONLY or name.split("_")[0] in ONLY


def main():
    print("ZNC: %s | base: %s (%s) | new: %s (%s) | probe: %s | preload: %s"
          % (ZNC_BIN, BASE, BASE_VER, NEW, NEW_VER, PROBE or None, EXTRA_ENV.get("LD_PRELOAD")))
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
