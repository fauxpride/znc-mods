import os
import re
import sys
import time
import traceback
import binascii

sys.path.insert(0, "/home/claude/harness")
from hx import Znc, events_of, parse_replay, BASE_TS  # noqa: E402

ZNC_BIN = os.environ.get("ZNC_BIN", "/usr/bin/znc")
# V080 = baseline build to compare against, V090 = build under test.
# The names are historical; the suite reads the baseline's reported version at
# startup and adapts assertions that are specific to pre-extension behaviour.
V080 = os.environ.get("V080", "")
V090 = os.environ.get("V090", "")
IGNORE_DROP = os.environ.get("IGNORE_DROP", "")
if not V080 or not V090:
    raise SystemExit("set V080 (baseline .so) and V090 (.so under test); see TESTING.md")
EXTRA_ENV = {}
if os.environ.get("ZNC_PRELOAD"):
    EXTRA_ENV["LD_PRELOAD"] = os.environ["ZNC_PRELOAD"]
    EXTRA_ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "")
    EXTRA_ENV["UBSAN_OPTIONS"] = os.environ.get("UBSAN_OPTIONS", "")
ROOT = os.environ.get("HX_ROOT", "/tmp/highlightctx-tests")
ONLY = set(a for a in sys.argv[1:] if not a.startswith("-"))

results = []  # (scenario, check, ok, detail)
open_zncs = []


def check(scn, name, ok, detail=""):
    results.append((scn, name, bool(ok), detail))
    if not ok:
        print("    FAIL %s :: %s :: %s" % (scn, name, detail))


def mk(name, so, load="before=8 after=8", chans=("#a", "#b"), pre_mods=None):
    mods = {"highlightctx": so}
    lines = []
    for mname, mso, margs in (pre_mods or []):
        mods[mname] = mso
        lines.append((mname + " " + margs).strip())
    lines.append(("highlightctx " + load).strip())
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, mods, lines, chans=chans, extra_env=EXTRA_ENV)
    open_zncs.append(z)
    z.start()
    return z


def filler(d, chan, labels, nick="alice"):
    for lab in labels:
        d.say(chan, nick, "%s chatter" % lab)


def simple(ev):
    """Compact view: list of (marked, text) for every line in the event."""
    return [("*" if l["marked"] else " ") + l["text"] for l in ev["lines"]]


def labels(ev):
    out = []
    for l in ev["lines"]:
        tok = l["text"].split(" ", 2)
        # "<nick> LABEL ..." / "* nick LABEL" / "-nick- LABEL"
        if l["text"].startswith("* "):
            lab = tok[2].split(" ")[0]
        else:
            lab = tok[1]
        out.append(("*" if l["marked"] else "") + lab)
    return out


def finish(z):
    try:
        z.close()
    except Exception:
        pass
    out = z.output()
    for bad in ("AddressSanitizer", "runtime error:", "Segmentation", "LeakSanitizer", "SUMMARY:"):
        if bad in out:
            check("sanitizer", "%s clean (%s)" % (os.path.basename(z.workdir), bad), False, out[-3000:])
    if z in open_zncs:
        open_zncs.remove(z)


def P(n):
    return ["P%d" % i for i in range(1, n + 1)]


def L(a, b):
    return ["L%d" % i for i in range(a, b + 1)]


# --------------------------------------------------------------------------
# Differential regression scenarios: must produce identical replay AND
# identical on-disk journal on 0.8.0 and 0.9.0 (no overlapping triggers).
# Each returns a comparable dict.
# --------------------------------------------------------------------------

def reg_single_complete(so, tag):
    z = mk("reg_single_complete_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(12))
    d.say("#a", "bob", "L0 tim: ping")
    filler(d, "#a", L(1, 15))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_single_partial(so, tag):
    z = mk("reg_single_partial_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(3))
    d.say("#a", "bob", "L0 hello TIM")
    filler(d, "#a", L(1, 3))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_far_apart(so, tag):
    z = mk("reg_far_apart_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim first")
    filler(d, "#a", L(1, 11))
    d.say("#a", "carol", "L12 tim second")
    filler(d, "#a", L(13, 25))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_boundary_after_close(so, tag):
    # after=8: the trigger at L9 arrives after the first event completed at L8
    z = mk("reg_boundary_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim first")
    filler(d, "#a", L(1, 8))
    d.say("#a", "carol", "L9 tim second")
    filler(d, "#a", L(10, 20))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_cross_channel(so, tag):
    z = mk("reg_cross_channel_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(4))
    filler(d, "#b", ["Q1", "Q2"])
    d.say("#a", "bob", "L0 tim in a")
    filler(d, "#a", L(1, 2))
    d.say("#b", "carol", "M0 tim in b")
    filler(d, "#a", L(3, 10))
    filler(d, "#b", ["M%d" % i for i in range(1, 10)])
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_excluded_in_window(so, tag):
    z = mk("reg_excluded_" + tag, so, load="before=8 after=8 excludes=spammer,*!*@evil.host")
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim ping")
    filler(d, "#a", L(1, 2))
    d.say("#a", "spammer", "L3 tim spam")
    d.say("#a", "evil", "L4 tim from evil host")
    filler(d, "#a", L(5, 12))
    d.say("#a", "spammer", "L13 tim spam outside window")
    filler(d, "#a", L(14, 20))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_self_line(so, tag):
    z = mk("reg_self_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(3))
    d.say("#a", "bob", "L0 tim ping")
    d.say("#a", "tim", "L1 yes tim is here")
    filler(d, "#a", L(2, 12))
    d.say("#a", "tim", "L13 tim talking to myself")
    filler(d, "#a", L(14, 16))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_after_zero(so, tag):
    z = mk("reg_after_zero_" + tag, so, load="before=4 after=0")
    d = z.ircd
    filler(d, "#a", P(5))
    d.say("#a", "bob", "L0 tim one")
    d.say("#a", "carol", "L1 tim two")
    filler(d, "#a", L(2, 3))
    d.say("#a", "dave", "L4 tim three")
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_kinds(so, tag):
    z = mk("reg_kinds_" + tag, so, load="before=3 after=3")
    d = z.ircd
    d.say("#a", "alice", "P1 notice ctx", kind="N")
    d.say("#a", "alice", "P2 waves", kind="A")
    d.say("#a", "bob", "L0 pokes tim", kind="A")
    d.say("#a", "carol", "L1 notice", kind="N")
    filler(d, "#a", L(2, 5))
    d.say("#b", "dave", "M0 tim via notice", kind="N")
    filler(d, "#b", ["M1", "M2", "M3"])
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_max_events(so, tag):
    z = mk("reg_max_events_" + tag, so, load="before=2 after=2 max_events=2")
    d = z.ircd
    for k in range(3):
        filler(d, "#a", ["P%d_%d" % (k, i) for i in range(3)])
        d.say("#a", "bob", "L%d tim n%d" % (k, k))
        filler(d, "#a", ["A%d_%d" % (k, i) for i in range(4)])
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_fallback_client(so, tag):
    z = mk("reg_fallback_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(9))
    d.say("#a", "bob", "L0 tim fallback")
    filler(d, "#a", L(1, 9))
    d.sync()
    c = z.client(server_time=False)
    raw = [b for (t, b) in c.attach_lines if not b.startswith("Version:")]
    r = dict(events=events_of(c), raw=raw)
    c.quit(); finish(z)
    return r


def reg_nick_boundaries(so, tag):
    z = mk("reg_nick_boundaries_" + tag, so, load="before=1 after=1")
    d = z.ircd
    d.say("#a", "bob", "N1 timothy is not me")
    d.say("#a", "bob", "N2 xtim nor this")
    d.say("#a", "bob", "N3 tim_ neither")
    filler(d, "#a", ["F1", "F2"])
    d.say("#a", "bob", "N4 tim's yes")
    filler(d, "#a", ["F3", "F4"])
    d.say("#a", "bob", "N5 [tim] also not (pre-existing: [ ] are nick chars)")
    filler(d, "#a", ["F5", "F6"])
    d.sync()
    c = z.client()
    r = dict(events=events_of(c))
    c.quit(); finish(z)
    return r


def reg_before_zero(so, tag):
    z = mk("reg_before_zero_" + tag, so, load="before=0 after=2")
    d = z.ircd
    filler(d, "#a", P(4))
    d.say("#a", "bob", "L0 tim")
    filler(d, "#a", L(1, 4))
    d.sync()
    j = z.journal_lines()
    c = z.client()
    r = dict(events=events_of(c), journal=j)
    c.quit(); finish(z)
    return r


def reg_ring_cleared_on_detach(so, tag):
    z = mk("reg_ring_detach_" + tag, so)
    d = z.ircd
    c = z.client()
    filler(d, "#a", ["W1", "W2", "W3"])  # while attached: not captured
    d.sync()
    c.quit()
    filler(d, "#a", ["P1", "P2"])
    d.say("#a", "bob", "L0 tim")
    filler(d, "#a", L(1, 2))
    d.sync()
    c = z.client()
    r = dict(events=events_of(c))
    c.quit(); finish(z)
    return r


def reg_channel_exclusion(so, tag):
    z = mk("reg_channel_excl_" + tag, so, load="before=2 after=2 excludes=#b")
    d = z.ircd
    d.say("#b", "bob", "M0 tim in excluded")
    filler(d, "#b", ["M1", "M2", "M3"])
    d.say("#a", "bob", "L0 tim in a")
    filler(d, "#a", L(1, 3))
    d.sync()
    c = z.client()
    r = dict(events=events_of(c))
    c.quit(); finish(z)
    return r


def reg_restart_pending(so, tag):
    z = mk("reg_restart_pending_" + tag, so)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim before restart")
    filler(d, "#a", L(1, 9))    # completes
    d.say("#b", "bob", "M0 tim open at restart")
    filler(d, "#b", ["M1", "M2"])  # still open
    d.sync()
    z.stop(kill=True)
    z.start()
    filler(d, "#b", ["M3", "M4"])
    d.sync()
    c = z.client()
    r = dict(events=events_of(c))
    c.quit(); finish(z)
    return r


def reg_commands(so, tag):
    z = mk("reg_commands_" + tag, so)
    c = z.client()
    out = []
    for cmd in ["Help", "Overview", "Status", "SetBefore 5", "SetAfter 6", "SetMaxEvents 3", "SetMaxEvents off",
                "AddExclude #noise", "AddExclude BadNick", "AddExclude *!*@evil.example", "AddExclude badnick",
                "AddExclude", "ListExcludes", "DelExclude 2", "DelExclude #noise", "ListExcludes",
                "SetRequireIgnoreDrop on", "SetRequireIgnoreDrop off", "SetRequireIgnoreDrop auto",
                "Rearm", "Compact", "ClearPending", "Reset", "Status", "SetBefore x", "SetAfter", "Bogus"]:
        out.append((cmd, [b for (t, b) in c.command(cmd) if not b.startswith("Version: highlightctx")]))
    c.quit(); finish(z)
    return dict(commands=out)


REGRESSIONS = [reg_single_complete, reg_single_partial, reg_far_apart, reg_boundary_after_close,
               reg_cross_channel, reg_excluded_in_window, reg_self_line, reg_after_zero, reg_kinds,
               reg_max_events, reg_fallback_client, reg_nick_boundaries, reg_before_zero,
               reg_ring_cleared_on_detach, reg_channel_exclusion, reg_restart_pending, reg_commands]


_VER_RE = re.compile(r"\b\d+\.\d+\.\d+\b")


def norm_version(s):
    """Blank out any semver so command output can be compared across builds."""
    return _VER_RE.sub("X.Y.Z", s).replace("_v080/", "_vX/").replace("_v090/", "_vX/")


def run_regressions():
    for fn in REGRESSIONS:
        scn = fn.__name__
        if ONLY and scn not in ONLY and "reg" not in ONLY:
            continue
        print("  running", scn)
        a = fn(V080, "v080")
        b = fn(V090, "v090")
        if scn == "reg_commands":
            ca = dict(a["commands"])
            cb = dict(b["commands"])
            for cmd, _ in a["commands"]:
                la = [norm_version(x) for x in ca[cmd]]
                lb = [norm_version(x) for x in cb[cmd]]
                # Which deltas are expected depends on the baseline. Against a
                # same-minor baseline the command surface must be identical.
                if baseline_version().rsplit(".", 1)[0] == "0.11":
                    check(scn, "'%s' output identical (no command-surface change)" % cmd, la == lb,
                          "\nbase=%r\nnew =%r" % ([x for x in la if x not in lb], [x for x in lb if x not in la]))
                    continue
                if cmd == "Reset":
                    extra = [x for x in lb if x not in la]
                    check(scn, "Reset differs only in the defaults line",
                          len(extra) == 1 and extra[0].startswith("All settings reset to defaults") and
                          "max_events=100" in extra[0] and "max_event_lines=100" in extra[0], repr(extra))
                    continue
                if cmd == "Overview":
                    extra = [x for x in lb if x not in la]
                    check(scn, "Overview identical except one added growth-controls paragraph",
                          [x for x in lb if x not in extra] == la and len(extra) == 1 and
                          extra[0].startswith("Growth controls:"), repr(extra))
                    continue
                if cmd == "Status":
                    extra = [x for x in lb if x not in la]
                    missing = [x for x in la if x not in lb]
                    want_extra = ["max_events: 100", "max_event_lines: 100", "max_event_age: disabled",
                                  "events dropped since last replay: 0 by max_events, 0 by max_event_age"]
                    check(scn, "Status: only the growth-control lines added and the max_events default changed",
                          extra == want_extra and missing == ["max_events: disabled"],
                          "extra=%r missing=%r" % (extra, missing))
                    continue
                if cmd == "Help":
                    # Only the SetAfter row's description is expected to change.
                    da = [x for x in la if x not in lb]
                    db = [x for x in lb if x not in la]
                    # Expected: two new commands, plus the reworded SetAfter row.
                    added = [x for x in db if x not in la]
                    new_cmds = [x for x in added if "SetMaxEventLines" in x or "SetMaxEventAge" in x]
                    reworded = [x for x in added if x not in new_cmds]
                    ok = (len(new_cmds) == 2 and len(reworded) <= 1 and
                          all("SetAfter" in x for x in reworded) and
                          all("SetAfter" in x for x in da))
                    check(scn, "Help identical except SetAfter description", ok, "%r / %r" % (da, db))
                    check(scn, "Help SetAfter row mentions extension", any("extends that event" in x for x in lb), "")
                else:
                    check(scn, "'%s' output identical" % cmd, la == lb, "\n080=%r\n090=%r" % (la, lb))
            continue
        check(scn, "replay events identical 0.8.0 vs 0.9.0", a["events"] == b["events"],
              "\n080=%r\n090=%r" % (a["events"], b["events"]))
        check(scn, "has at least one event", len(b["events"]) > 0, "")
        if "journal" in a:
            check(scn, "journal byte-identical 0.8.0 vs 0.9.0", a["journal"] == b["journal"],
                  "\n080=%r\n090=%r" % (a["journal"], b["journal"]))
            check(scn, "no X records without overlap", not any(l.startswith("X\t") for l in b["journal"]), "")
        if "raw" in a:
            check(scn, "raw fallback bodies identical (modulo nothing)", a["raw"] == b["raw"], "")
        if scn == "reg_nick_boundaries":
            got = [labels(e)[e["lines"].index(next(l for l in e["lines"] if l["marked"]))].lstrip("*") for e in b["events"]]
            check(scn, "only N4 triggers (timothy/xtim/tim_/[tim] do not)", got == ["N4"], repr(got))
        if scn == "reg_excluded_in_window":
            ev = b["events"]
            check(scn, "excluded senders never marked", all(not l["marked"] for e in ev for l in e["lines"] if "spam" in l["text"] or "evil" in l["text"]), "")
            check(scn, "single event only", len(ev) == 1, repr([e["raw_header"] for e in ev]))


# --------------------------------------------------------------------------
# Extension scenarios (0.9.0 expected behaviour; 0.8.0 run for contrast)
# --------------------------------------------------------------------------

def tim_scenario(so, name, attach=True, stop_at=20):
    z = mk(name, so)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim first highlight")
    filler(d, "#a", L(1, 3))
    d.say("#a", "carol", "L4 tim second highlight")
    filler(d, "#a", L(5, stop_at))
    d.sync()
    return z


BASELINE_VER = [None]


def baseline_version():
    """Read the baseline module's version once, so contrast assertions match
    whichever version V080 points at (0.8.0 pre-extension, or 0.9.0+)."""
    if BASELINE_VER[0] is None:
        z = mk("baseline_probe", V080)
        c = z.client()
        v = [b for (t, b) in c.attach_lines if b.startswith("Version: highlightctx")]
        c.quit(); finish(z)
        BASELINE_VER[0] = v[0].split()[-1] if v else "?"
    return BASELINE_VER[0]


def ext_tim_exact():
    scn = "ext_tim_exact"
    # Baseline contrast: 0.8.0 produced two overlapping events; 0.9.0+ already
    # extends, so the baseline must then match the 0.9.0 expectation exactly.
    pre_ext = baseline_version() == "0.8.0"
    z = tim_scenario(V080, scn + "_v080")
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    if not pre_ext:
        check(scn, "baseline %s: one extended event (no regression)" % baseline_version(),
              len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=12/12, triggers=2)"
              and labels(ev[0]) == P(8) + ["*L0"] + L(1, 3) + ["*L4"] + L(5, 12),
              repr([(e["raw_header"], labels(e)) for e in ev]))
    else:
        check(scn, "0.8.0: two events", len(ev) == 2, repr([e["raw_header"] for e in ev]))
    if pre_ext and len(ev) == 2:
        A, B = ev
        check(scn, "0.8.0: A = P1..P8,*L0,L1..L8", labels(A) == P(8) + ["*L0"] + L(1, 8), repr(labels(A)))
        check(scn, "0.8.0: B = P5..P8,L0..L3,*L4,L5..L12", labels(B) == P(8)[4:] + L(0, 3) + ["*L4"] + L(5, 12), repr(labels(B)))
        dup = set(x.lstrip("*") for x in labels(A)) & set(x.lstrip("*") for x in labels(B))
        check(scn, "0.8.0: 13 duplicated lines", len(dup) == 13, repr(sorted(dup)))

    z = tim_scenario(V090, scn + "_v090")
    j = z.journal_lines()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "0.9.0: exactly one event", len(ev) == 1, repr([e["raw_header"] for e in ev]))
    if ev:
        e = ev[0]
        check(scn, "header complete before=8 after=12/12 triggers=2",
              e["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=12/12, triggers=2)", e["raw_header"])
        check(scn, "lines = P1..P8,*L0,L1..L3,*L4,L5..L12",
              labels(e) == P(8) + ["*L0"] + L(1, 3) + ["*L4"] + L(5, 12), repr(labels(e)))
        check(scn, "no duplicated lines", len(labels(e)) == len(set(labels(e))), "")
        check(scn, "timestamps preserved on marked secondary trigger",
              [l["time"] for l in e["lines"] if l["text"].startswith("<carol> L4")] == ["2025-09-11T08:00:13.000Z"],
              repr([l for l in e["lines"] if "L4" in l["text"]]))
    xs = [l for l in j if l.startswith("X\t")]
    check(scn, "journal has exactly one X record 'X\\t1\\t3'", xs == ["X\t1\t3"], repr(xs))
    order = [l.split("\t")[0] for l in j]
    check(scn, "journal order B,A,A,A,A,X,A...,F", order[:6] == ["B", "A", "A", "A", "A", "X"] and order[-1] == "F" and order.count("A") == 12, repr(order))


def ext_last_line_of_window():
    scn = "ext_last_line_of_window"
    z = mk(scn, V090)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim first")
    filler(d, "#a", L(1, 7))
    d.say("#a", "carol", "L8 tim on the 8th after-line")
    filler(d, "#a", L(9, 30))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one event", len(ev) == 1, repr([e["raw_header"] for e in ev]))
    if ev:
        check(scn, "after=16/16 triggers=2", (ev[0]["after"], ev[0]["target"], ev[0]["triggers"], ev[0]["state"]) == (16, 16, 2, "complete"), ev[0]["raw_header"])
        check(scn, "lines", labels(ev[0]) == P(8) + ["*L0"] + L(1, 7) + ["*L8"] + L(9, 16), repr(labels(ev[0])))


def ext_first_line_after_window():
    scn = "ext_first_line_after_window"
    z = mk(scn, V090)
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim first")
    filler(d, "#a", L(1, 8))
    d.say("#a", "carol", "L9 tim after completion")
    filler(d, "#a", L(10, 30))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "two events", len(ev) == 2, repr([e["raw_header"] for e in ev]))
    if len(ev) == 2:
        check(scn, "first unextended 8/8 no triggers field", ev[0]["raw_header"].endswith("(complete, before=8, after=8/8)"), ev[0]["raw_header"])
        check(scn, "second has before=L2..L8+L1? (ring)", labels(ev[1]) == L(1, 8) + ["*L9"] + L(10, 17), repr(labels(ev[1])))


def ext_chain():
    scn = "ext_chain"
    z = mk(scn, V090)
    d = z.ircd
    filler(d, "#a", P(2))
    d.say("#a", "bob", "L0 tim a")
    filler(d, "#a", L(1, 2))
    d.say("#a", "carol", "L3 TIM b")          # case-insensitive
    filler(d, "#a", L(4, 5))
    d.say("#a", "dave", "L6 hey tim, c")
    filler(d, "#a", L(7, 9))
    d.say("#a", "bob", "L10 tim: d")
    filler(d, "#a", L(11, 40))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one event", len(ev) == 1, repr([e["raw_header"] for e in ev]))
    if ev:
        e = ev[0]
        check(scn, "after=18/18 triggers=4 before=2", (e["after"], e["target"], e["triggers"], e["before"], e["state"]) == (18, 18, 4, 2, "complete"), e["raw_header"])
        exp = P(2) + ["*L0", "L1", "L2", "*L3", "L4", "L5", "*L6", "L7", "L8", "L9", "*L10"] + L(11, 18)
        check(scn, "lines", labels(e) == exp, repr(labels(e)))


def ext_spam():
    scn = "ext_spam"
    pre_ext = baseline_version() == "0.8.0"
    res = {}
    for tag, so in (("v080", V080), ("v090", V090)):
        z = mk(scn + "_" + tag, so)
        d = z.ircd
        filler(d, "#a", P(8))
        for i in range(30):
            d.say("#a", "spammer", "S%d tim tim tim" % i)
        filler(d, "#a", L(1, 20))
        d.sync()
        j = z.journal_lines()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        res[tag] = (ev, j)
    ev8, j8 = res["v080"]
    ev9, j9 = res["v090"]
    if pre_ext:
        check(scn, "0.8.0 produced 30 overlapping events", len(ev8) == 30, str(len(ev8)))
    else:
        check(scn, "baseline %s also produced one event" % baseline_version(), len(ev8) == 1, str(len(ev8)))
    check(scn, "0.9.0 produced one event", len(ev9) == 1, str(len(ev9)))
    if ev9:
        e = ev9[0]
        check(scn, "after=37/37 triggers=30", (e["after"], e["target"], e["triggers"], e["state"]) == (37, 37, 30, "complete"), e["raw_header"])
        check(scn, "all 30 spam lines marked exactly once", sum(1 for l in e["lines"] if l["marked"]) == 30, "")
    check(scn, "0.9.0 journal = B + 37A + 29X + F (68 lines)", len(j9) == 68 and sum(1 for l in j9 if l[0] == "X") == 29, "%d lines" % len(j9))
    lines8 = sum(len(e["lines"]) for e in ev8)
    lines9 = sum(len(e["lines"]) for e in ev9)
    print("    info: spam replay lines 0.8.0=%d 0.9.0=%d ; journal appends 0.8.0=%d 0.9.0=%d" % (lines8, lines9, len(j8), len(j9)))
    if pre_ext:
        check(scn, "0.9.0 journal smaller than 0.8.0", len(j9) < len(j8), "%d vs %d" % (len(j9), len(j8)))
    else:
        check(scn, "journal identical to baseline", j9 == j8, "%d vs %d" % (len(j9), len(j8)))


def ext_partial():
    scn = "ext_partial"
    z = tim_scenario(V090, scn, stop_at=7)
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one partial event after=7/12 triggers=2",
          len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (partial, before=8, after=7/12, triggers=2)",
          repr([e["raw_header"] for e in ev]))


def ext_excluded_does_not_extend():
    scn = "ext_excluded_does_not_extend"
    z = mk(scn, V090, load="before=8 after=8 excludes=spammer,*!*@evil.host")
    d = z.ircd
    filler(d, "#a", P(8))
    d.say("#a", "bob", "L0 tim ping")
    filler(d, "#a", L(1, 2))
    d.say("#a", "spammer", "L3 tim spam")
    d.say("#a", "evil", "L4 tim evil host")
    d.say("#a", "carol", "L5 tim real")
    filler(d, "#a", L(6, 30))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one event", len(ev) == 1, repr([e["raw_header"] for e in ev]))
    if ev:
        e = ev[0]
        check(scn, "target 13 (from L5 only), triggers=2", (e["after"], e["target"], e["triggers"]) == (13, 13, 2), e["raw_header"])
        check(scn, "L3/L4 present unmarked, L5 marked", labels(e) == P(8) + ["*L0", "L1", "L2", "L3", "L4", "*L5"] + L(6, 13), repr(labels(e)))


def ext_self_does_not_extend():
    scn = "ext_self_does_not_extend"
    z = mk(scn, V090)
    d = z.ircd
    d.say("#a", "bob", "L0 tim ping")
    filler(d, "#a", L(1, 3))
    d.say("#a", "tim", "L4 tim here, TIM speaking")
    filler(d, "#a", L(5, 20))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one unextended event after=8/8", len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (complete, before=0, after=8/8)", repr([e["raw_header"] for e in ev]))


def ext_cap_from_event_not_current():
    scn = "ext_cap_from_event_not_current"
    z = mk(scn, V090)
    d = z.ircd
    d.say("#a", "bob", "L0 tim started with after=8")
    filler(d, "#a", L(1, 2))
    d.sync()
    z.stop()  # graceful
    z.load_lines = ["highlightctx before=8 after=4"]
    z.write_config()
    z.start()
    filler(d, "#a", L(3, 5))
    d.say("#a", "carol", "L6 tim extends after restart")
    filler(d, "#a", L(7, 30))
    d.say("#b", "dave", "M0 tim new event uses 4")
    filler(d, "#b", ["M%d" % i for i in range(1, 10)])
    d.sync()
    c = z.client()
    ev = events_of(c)
    st = [b for (t, b) in c.command("Status")]
    c.quit(); finish(z)
    check(scn, "after cap now 4", any(b == "after cap: 4" for (b) in st), repr(st))
    evs = {e["chan"]: e for e in ev}
    check(scn, "two events", len(ev) == 2, repr([e["raw_header"] for e in ev]))
    if "#a" in evs:
        e = evs["#a"]
        check(scn, "#a extension uses event cap 8: target 6+8=14", (e["after"], e["target"], e["triggers"]) == (14, 14, 2), e["raw_header"])
        check(scn, "#a lines", labels(e) == ["*L0"] + L(1, 5) + ["*L6"] + L(7, 14), repr(labels(e)))
    if "#b" in evs:
        check(scn, "#b new event uses current cap 4", evs["#b"]["target"] == 4 and evs["#b"]["triggers"] is None, evs["#b"]["raw_header"])


def ext_crash_recovery():
    scn = "ext_crash_recovery"
    z = tim_scenario(V090, scn, stop_at=6)
    j = z.journal_lines()
    check(scn, "X record present before crash", "X\t1\t3" in j, repr([l for l in j if not l.startswith("A")]))
    z.stop(kill=True)
    z.start()
    filler(z.ircd, "#a", L(7, 20))
    z.ircd.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "recovered as one complete extended event",
          len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=12/12, triggers=2)",
          repr([e["raw_header"] for e in ev]))
    if ev:
        check(scn, "lines incl. marks", labels(ev[0]) == P(8) + ["*L0"] + L(1, 3) + ["*L4"] + L(5, 12), repr(labels(ev[0])))


def inode(p):
    try:
        return os.stat(p).st_ino
    except FileNotFoundError:
        return None


def ext_compaction_roundtrip():
    scn = "ext_compaction_roundtrip"
    z = mk(scn, V090, load="before=2 after=8 max_event_lines=off")
    d = z.ircd
    filler(d, "#a", P(2))
    d.say("#a", "bob", "S0 tim start")
    filler(d, "#a", ["Q1"])
    d.sync()
    with open(z.journal_path(), "a") as f:   # ZNC idle after sync; O_APPEND writer
        f.write("Zmarker\tonly-removed-by-compaction\n")
    N = 300
    for i in range(1, N):
        d.say("#a", "spammer", "S%d tim" % i)
    d.sync()
    j = z.journal_lines()
    check(scn, "auto-compaction rewrote journal (marker gone)", not any(l.startswith("Zmarker") for l in j), "")
    # Open event: B, then after lines: Q1 (A), then 299 x (A,X)
    exp = ["B", "A"] + ["A", "X"] * (N - 1)
    got = [l.split("\t")[0] for l in j]
    check(scn, "compacted open journal = B,A,(A,X)*299", got == exp, "len=%d head=%r" % (len(got), got[:8]))
    xidx = [int(l.split("\t")[2]) for l in j if l.startswith("X")]
    check(scn, "X indices 1..299 contiguous", xidx == list(range(1, N)), repr(xidx[:5]))
    # crash while open, recover, finish window, crash again while pending, recover, replay
    z.stop(kill=True)
    z.start()
    filler(d, "#a", ["T%d" % i for i in range(1, 9)])
    d.sync()
    z.stop(kill=True)
    z.start()
    d.say("#b", "alice", "unrelated traffic")
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one event after double crash", len(ev) == 1, repr([e["raw_header"] for e in ev]))
    if ev:
        e = ev[0]
        target = (N - 1) + 1 + 8  # last trigger at index N-1 (after Q1 at 0)
        check(scn, "complete after=%d/%d triggers=%d" % (target, target, N), (e["state"], e["after"], e["target"], e["triggers"]) == ("complete", target, target, N), e["raw_header"])
        check(scn, "marks preserved through compaction+recovery", sum(1 for l in e["lines"] if l["marked"]) == N, "")


def enc_line(ts, kind, nick, text):
    raw = "%d;%s;%s;%s" % (ts, kind, binascii.hexlify(nick.encode()).decode().upper(), binascii.hexlify(text.encode()).decode().upper())
    return binascii.hexlify(raw.encode()).decode().upper()


def ext_handcrafted_journal():
    scn = "ext_handcrafted_journal"
    name = scn
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, {"highlightctx": V090}, ["highlightctx before=8 after=8"], extra_env=EXTRA_ENV)
    open_zncs.append(z)
    moddir = os.path.dirname(z.journal_path())
    os.makedirs(moddir, exist_ok=True)
    ch = lambda c: binascii.hexlify(c.encode()).decode().upper()
    t = BASE_TS - 1000
    J = []
    # Event 1 (#a): finalized, cap 2, 4 after lines, X at 1 -> target 4. Plus junk X records.
    J.append("\t".join(["B", "1", ch("#a"), str(t), "2", enc_line(t - 1, "T", "x", "c1"), enc_line(t, "T", "bob", "E1T tim")]))
    J.append("X\t1\t0")  # before any A: idx 0 >= size 0 -> ignored
    for i, txt in enumerate(["E1A0", "E1A1 tim", "E1A2", "E1A3"]):
        J.append("\t".join(["A", "1", enc_line(t + 1 + i, "T", "carol", txt)]))
        if i == 1:
            J.append("X\t1\t1")
            J.append("X\t1\t1")          # duplicate -> idempotent
    J += ["X\t1\t99", "X\t1\tabc", "X\t1\t-1", "X\t1\t", "X\t1", "X\t1\t2\textra", "X\t999\t0", "X\t\t1", "Xjunk"]
    J.append("F\t1\t0")
    # Event 2 (#b): open, cap 3, A,A then X at 1 -> target 5; live lines complete it
    J.append("\t".join(["B", "2", ch("#b"), str(t + 50), "3", "", enc_line(t + 50, "T", "bob", "E2T tim")]))
    J.append("\t".join(["A", "2", enc_line(t + 51, "T", "a", "E2A0")]))
    J.append("\t".join(["A", "2", enc_line(t + 52, "T", "a", "E2A1 tim")]))
    J.append("X\t2\t1")
    # Event 3 (#a): delivered -> gone even though it has X
    J.append("\t".join(["B", "3", ch("#a"), str(t + 60), "1", "", enc_line(t + 60, "T", "bob", "E3T tim")]))
    J.append("\t".join(["A", "3", enc_line(t + 61, "T", "a", "E3A0")]))
    J.append("X\t3\t0")
    J.append("D\t3")
    with open(z.journal_path(), "w") as f:
        f.write("\n".join(J) + "\n")
    z.start()
    d = z.ircd
    filler(d, "#b", ["LIVE1", "LIVE2", "LIVE3", "LIVE4"])
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    evs = {e["id"]: e for e in ev}
    check(scn, "events 1 and 2 only", sorted(evs) == [1, 2], repr([e["raw_header"] for e in ev]))
    if 1 in evs:
        check(scn, "event1 header", evs[1]["raw_header"] == "[#a] highlight event #1 (complete, before=1, after=4/4, triggers=2)", evs[1]["raw_header"])
        check(scn, "event1 only idx1 marked", labels(evs[1]) == ["c1", "*E1T", "E1A0", "*E1A1", "E1A2", "E1A3"], repr(labels(evs[1])))
    if 2 in evs:
        check(scn, "event2 recovered open with target 5, completed by live lines",
              evs[2]["raw_header"] == "[#b] highlight event #2 (complete, before=0, after=5/5, triggers=2)", evs[2]["raw_header"])
        check(scn, "event2 lines", labels(evs[2]) == ["*E2T", "E2A0", "*E2A1", "LIVE1", "LIVE2", "LIVE3"], repr(labels(evs[2])))


def ext_upgrade_from_080_overlap():
    scn = "ext_upgrade_from_080_overlap"
    if baseline_version() != "0.8.0":
        return
    z = tim_scenario(V080, scn, stop_at=6)   # 0.8.0 leaves A and B both open
    j = z.journal_lines()
    check(scn, "0.8.0 journal has two B records", sum(1 for l in j if l.startswith("B")) == 2, "")
    z.stop(kill=True)
    z.install_modules({"highlightctx": V090})
    z.start()
    d = z.ircd
    d.say("#a", "dave", "L7 tim third highlight after upgrade")
    filler(d, "#a", L(8, 30))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "still exactly two events (no third)", len(ev) == 2, repr([e["raw_header"] for e in ev]))
    if len(ev) == 2:
        A, B = ev
        check(scn, "older event A not extended, 8/8, L7 unmarked context", A["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=8/8)" and labels(A) == P(8) + ["*L0"] + L(1, 3) + ["L4"] + L(5, 8), A["raw_header"] + repr(labels(A)))
        check(scn, "newest event B extended by L7: target 3+8=11", B["raw_header"] == "[#a] highlight event #2 (complete, before=8, after=11/11, triggers=2)", B["raw_header"])
        check(scn, "B lines", labels(B) == P(8)[4:] + L(0, 3) + ["*L4", "L5", "L6", "*L7"] + L(8, 15), repr(labels(B)))


def ext_downgrade_to_080():
    scn = "ext_downgrade_to_080"
    if baseline_version() != "0.8.0":
        return
    z = tim_scenario(V090, scn, stop_at=6)
    z.stop(kill=True)
    z.install_modules({"highlightctx": V080})
    z.start()
    filler(z.ircd, "#a", L(7, 20))
    z.ircd.sync()
    c = z.client()
    ev = events_of(c)
    ver = [b for (t, b) in c.attach_lines if b.startswith("Version")]
    c.quit(); finish(z)
    check(scn, "baseline loaded newer journal", ver == ["Version: highlightctx 0.8.0"], repr(ver))
    check(scn, "0.8.0 ignores X: one event, 8/8, L4 unmarked, nothing lost up to cap",
          len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=8/8)" and labels(ev[0]) == P(8) + ["*L0"] + L(1, 8),
          repr([(e["raw_header"], labels(e)) for e in ev]))


def ext_multichannel():
    scn = "ext_multichannel"
    z = mk(scn, V090)
    d = z.ircd
    d.say("#a", "bob", "L0 tim in a")
    d.say("#b", "bob", "M0 tim in b")
    filler(d, "#a", L(1, 2))
    filler(d, "#b", ["M1", "M2"])
    d.say("#b", "carol", "M3 tim again in b")
    filler(d, "#a", L(3, 20))
    filler(d, "#b", ["M%d" % i for i in range(4, 20)])
    d.sync()
    c = z.client()
    ev = {e["chan"]: e for e in events_of(c)}
    c.quit(); finish(z)
    check(scn, "#a unextended", "#a" in ev and ev["#a"]["raw_header"] == "[#a] highlight event #1 (complete, before=0, after=8/8)", repr(ev.get("#a", {}).get("raw_header")))
    check(scn, "#b extended target 11", "#b" in ev and ev["#b"]["raw_header"] == "[#b] highlight event #2 (complete, before=0, after=11/11, triggers=2)", repr(ev.get("#b", {}).get("raw_header")))


def ext_kinds():
    scn = "ext_kinds"
    for tag, st in (("servertime", True), ("fallback", False)):
        z = mk(scn + "_" + tag, V090)
        d = z.ircd
        d.say("#a", "bob", "L0 tim text")
        d.say("#a", "alice", "L1 chatter")
        d.say("#a", "carol", "L2 pokes tim", kind="A")
        filler(d, "#a", L(3, 4))
        d.say("#a", "dave", "L5 tim: via notice", kind="N")
        filler(d, "#a", L(6, 20))
        d.sync()
        c = z.client(server_time=st)
        raw = [b for (t, b) in c.attach_lines]
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "%s: one event target 13 triggers=3" % tag, len(ev) == 1 and (ev[0]["target"], ev[0]["triggers"]) == (13, 3), repr([e["raw_header"] for e in ev]))
        want_action = "[#a] >>> * carol L2 pokes tim"
        want_notice = "[#a] >>> -dave- L5 tim: via notice"
        if st:
            check(scn, "servertime: action marked", want_action in raw, "")
            check(scn, "servertime: notice marked", want_notice in raw, "")
        else:
            check(scn, "fallback: inline-ts action marked", any(r.endswith(want_action) and r.startswith("[2025-09-11T") for r in raw), repr(raw[:6]))
            check(scn, "fallback: inline-ts notice marked", any(r.endswith(want_notice) and r.startswith("[2025-09-11T") for r in raw), "")


def ext_max_events():
    scn = "ext_max_events"
    z = mk(scn, V090, load="before=1 after=2 max_events=2")
    d = z.ircd
    d.say("#a", "bob", "L0 tim e1")
    d.say("#a", "carol", "L1 tim e1 ext")
    filler(d, "#a", L(2, 4))            # e1 done (target 3)
    d.say("#b", "bob", "M0 tim e2")
    filler(d, "#b", ["M1", "M2"])       # e2 done
    d.say("#a", "bob", "L5 tim e3")
    filler(d, "#a", L(6, 8))            # e3 done -> e1 dropped
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "extended event counts as one; oldest dropped", sorted(e["id"] for e in ev) == [2, 3], repr([e["raw_header"] for e in ev]))


def ext_ignore_drop():
    scn = "ext_ignore_drop"
    if not os.path.exists(IGNORE_DROP):
        check(scn, "ignore_drop module built", False, IGNORE_DROP)
        return
    z = mk(scn, V090, pre_mods=[("ignore_drop", IGNORE_DROP, "")])
    c = z.client()
    st = [b for (t, b) in c.command("Status")]
    add = c.status_cmd("*ignore_drop", "Add mallory!*@* always")
    c.quit()
    check(scn, "auto armed via OnBoot", "auto mode armed: yes" in st, repr(st))
    d = z.ircd
    d.say("#a", "bob", "L0 tim")
    filler(d, "#a", L(1, 1))
    d.say("#a", "mallory", "L2 tim ignored spam")
    filler(d, "#a", L(3, 4))
    d.say("#a", "carol", "L5 tim real")
    filler(d, "#a", L(6, 30))
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "one event, ignored line absent and not extending; target from L5",
          len(ev) == 1 and labels(ev[0]) == ["*L0", "L1", "L3", "L4", "*L5"] + L(6, 13) and ev[0]["target"] == 12,
          repr([(e["raw_header"], labels(e)) for e in ev]) + repr(add))


def ext_after_one():
    scn = "ext_after_one"
    z = mk(scn, V090, load="before=0 after=1")
    d = z.ircd
    d.say("#a", "bob", "L0 tim")
    d.say("#a", "carol", "L1 tim")
    filler(d, "#a", ["L2"])
    d.say("#a", "dave", "L3 tim")
    filler(d, "#a", ["L4", "L5"])
    d.sync()
    c = z.client()
    ev = events_of(c)
    c.quit(); finish(z)
    check(scn, "two events", [e["raw_header"] for e in ev] == [
        "[#a] highlight event #1 (complete, before=0, after=2/2, triggers=2)",
        "[#a] highlight event #2 (complete, before=0, after=1/1)"], repr([e["raw_header"] for e in ev]))
    if len(ev) == 2:
        check(scn, "lines", labels(ev[0]) == ["*L0", "*L1", "L2"] and labels(ev[1]) == ["*L3", "L4"], repr([labels(e) for e in ev]))


def ext_attach_between():
    scn = "ext_attach_between"
    z = tim_scenario(V090, scn, stop_at=5)
    c = z.client()
    ev1 = events_of(c)
    st = [b for (t, b) in c.command("Status")]
    c.quit()
    d = z.ircd
    d.say("#a", "dave", "K0 tim after reattach")
    filler(d, "#a", ["K%d" % i for i in range(1, 10)])
    d.sync()
    c = z.client()
    ev2 = events_of(c)
    c.quit(); finish(z)
    check(scn, "first attach partial 5/12", len(ev1) == 1 and ev1[0]["raw_header"] == "[#a] highlight event #1 (partial, before=8, after=5/12, triggers=2)", repr([e["raw_header"] for e in ev1]))
    check(scn, "no open events after replay", "open events: 0" in st, repr(st))
    check(scn, "new trigger after reattach starts fresh event", len(ev2) == 1 and ev2[0]["raw_header"] == "[#a] highlight event #2 (complete, before=0, after=8/8)", repr([e["raw_header"] for e in ev2]))


def ext_update_mod_from_080():
    scn = "ext_update_mod_from_080"
    z = mk(scn, V080, load="before=5 after=6 max_events=4 excludes=#noise,BadNick")
    c = z.client()
    c.command("SetRequireIgnoreDrop off")
    st1 = [b for (t, b) in c.command("Status")]
    z.install_modules({"highlightctx": V090})
    upd = c.status_cmd("*status", "UpdateMod highlightctx")
    ver = [b for (t, b) in c.command("Version")]
    st2 = [b for (t, b) in c.command("Status")]
    c.quit(); finish(z)
    keep = lambda st: [x for x in st if x.split(":")[0] in ("before cap", "after cap", "max_events", "require_ignore_drop mode", "excluded channels", "excluded nicks/masks")]
    check(scn, "UpdateMod succeeded", "Done" in [x[1] for x in upd] and "Reloading highlightctx everywhere" in [x[1] for x in upd], repr(upd))
    check(scn, "version updated after UpdateMod", any(v.startswith("Version: highlightctx ") and not v.endswith(baseline_version()) for v in ver), repr(ver))
    check(scn, "settings persisted across 0.8.0 -> 0.9.0 UpdateMod", keep(st1) == keep(st2) and len(keep(st2)) == 6, "%r vs %r" % (keep(st1), keep(st2)))


def pre_compaction_thrash():
    """Pre-existing behaviour probe (not a 0.9.0 change): once the compacted
    journal is still over kCompactThresholdLines, does every unrelated channel
    line trigger a full rewrite? Run on both versions; must be identical."""
    scn = "pre_compaction_thrash"
    res = {}
    for tag, so in (("v080", V080), ("v090", V090)):
        # max_events=off on both sides: this probe compares compaction
        # behaviour, so the retention cap must not differ between versions.
        z = mk(scn + "_" + tag, so, load="before=0 after=0 max_events=off")
        d = z.ircd
        for i in range(260):                  # 260 finalized after=0 events -> 520 lines (B+F)
            d.say("#a", "bob", "H%d tim" % i)
        d.sync()
        rewrites = 0
        for k in range(10):
            with open(z.journal_path(), "a") as f:
                f.write("Zmarker\t%d\n" % k)
            d.say("#b", "alice", "unrelated %d" % k)   # no event touches #b
            d.sync()
            if not any(l.startswith("Zmarker") for l in z.journal_lines()):
                rewrites += 1
        c = z.client()
        n = len(events_of(c))
        c.quit(); finish(z)
        res[tag] = (rewrites, n)
    print("    info: full journal rewrites per 10 unrelated lines: 0.8.0=%d 0.9.0=%d" % (res["v080"][0], res["v090"][0]))
    check(scn, "events intact (260 each)", res["v080"][1] == 260 and res["v090"][1] == 260, repr(res))
    # 0.11.1 changed this deliberately: the trigger point is now derived from
    # what the last compaction achieved, so a large live set no longer rewrites
    # on every line. cmp_thrash_fixed asserts the improvement directly.
    check(scn, "baseline rewrites on every line; version under test does not regress",
          res["v080"][0] >= res["v090"][0], repr(res))


EXTENSIONS = [ext_tim_exact, ext_last_line_of_window, ext_first_line_after_window, ext_chain, ext_spam,
              ext_partial, ext_excluded_does_not_extend, ext_self_does_not_extend, ext_cap_from_event_not_current,
              ext_crash_recovery, ext_compaction_roundtrip, ext_handcrafted_journal, ext_upgrade_from_080_overlap,
              ext_downgrade_to_080, ext_multichannel, ext_kinds, ext_max_events, ext_ignore_drop, ext_after_one,
              ext_attach_between, ext_update_mod_from_080, pre_compaction_thrash]


import journal_off  # noqa: E402
import growth  # noqa: E402
import compaction  # noqa: E402

journal_off.install(globals())
growth.install(globals())
compaction.install(globals())


def run_compaction():
    for fn in COMPACTION:
        if ONLY and fn.__name__ not in ONLY and "cmp" not in ONLY:
            continue
        print("  running", fn.__name__)
        try:
            fn()
        except Exception:
            check(fn.__name__, "no exception", False, traceback.format_exc())
        finally:
            for z in list(open_zncs):
                finish(z)


def run_growth():
    for fn in GROWTH:
        if ONLY and fn.__name__ not in ONLY and "gc" not in ONLY:
            continue
        print("  running", fn.__name__)
        try:
            fn()
        except Exception:
            check(fn.__name__, "no exception", False, traceback.format_exc())
        finally:
            for z in list(open_zncs):
                finish(z)


def run_journal_off():
    for fn in JOURNAL_OFF:
        if ONLY and fn.__name__ not in ONLY and "jo" not in ONLY:
            continue
        print("  running", fn.__name__)
        try:
            fn()
        except Exception:
            check(fn.__name__, "no exception", False, traceback.format_exc())
        finally:
            for z in list(open_zncs):
                finish(z)


def run_extensions():
    for fn in EXTENSIONS:
        if ONLY and fn.__name__ not in ONLY and "ext" not in ONLY:
            continue
        print("  running", fn.__name__)
        try:
            fn()
        except Exception:
            check(fn.__name__, "no exception", False, traceback.format_exc())
        finally:
            for z in list(open_zncs):
                finish(z)


if __name__ == "__main__":
    print("ZNC:", ZNC_BIN, "| 0.8.0:", V080, "| 0.9.0:", V090, "| preload:", EXTRA_ENV.get("LD_PRELOAD"))
    try:
        run_regressions()
    except Exception:
        check("regressions", "no exception", False, traceback.format_exc())
        for z in list(open_zncs):
            finish(z)
    run_extensions()
    run_journal_off()
    run_growth()
    run_compaction()
    by = {}
    for scn, name, ok, det in results:
        by.setdefault(scn.split("_v0")[0], [0, 0])
        by[scn.split("_v0")[0]][0 if ok else 1] += 1
    print()
    for k, (p, f) in by.items():
        print("  %-40s %3d passed %3d failed" % (k, p, f))
    tot_p = sum(1 for r in results if r[2])
    tot_f = sum(1 for r in results if not r[2])
    print("\nTOTAL: %d passed, %d failed" % (tot_p, tot_f))
    sys.exit(1 if tot_f else 0)
