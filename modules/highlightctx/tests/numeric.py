"""Numeric input scenarios for highlightctx 0.14.1.

Up to 0.14.0, counts, caps and list indexes were parsed with CString::ToUInt,
which wraps modulo 2^32 instead of rejecting values that do not fit:
"SetBefore 4294967304" set a cap of 8, and "DelExclude 4294967297" removed
exclusion #1. The same parser handled the before, after, max_events and
max_event_lines load arguments. 0.14.1 rejects such values.

A related overflow is fixed too: the "natural size" note printed by
SetMaxEventLines added before + 1 + after in 32 bits, so with a very large
before cap the sum wrapped and the note was wrong or missing.

Imported by suite.py; every function follows the suite's check() convention.
"""
import re


def install(ns):
    mk = ns["mk"]
    check = ns["check"]
    finish = ns["finish"]
    V090 = ns["V090"]          # build under test
    V080 = ns["V080"]          # baseline
    baseline_version = ns["baseline_version"]

    def old_baseline():
        """True for a baseline older than 0.14.1, which wraps."""
        try:
            return tuple(int(x) for x in baseline_version().split(".")) < (0, 14, 1)
        except ValueError:
            return True

    def run(c, cmd):
        return [b for (t, b) in c.command(cmd) if not b.startswith("Version: highlightctx")]

    def field(lines, prefix):
        for x in lines:
            if x.startswith(prefix):
                return x[len(prefix):]
        return None

    def caps(c):
        st = run(c, "Status")
        return {k: field(st, k + ": ") for k in ("before cap", "after cap", "max_events", "max_event_lines")}

    def status_raw(c, cmd):
        """A *status command whose reply is wanted while *highlightctx may be
        unloaded: *status Version (ZNC's own tag) marks the end."""
        def text(l):
            return l.split(" :", 1)[1] if l.startswith(":*status!") and " :" in l else None

        def is_tag(l):
            t = text(l)
            return t is not None and t.startswith("ZNC ") and "znc.in" in t

        start = len(c.lines)
        c.send("PRIVMSG *status :" + cmd)
        c.send("PRIVMSG *status :Version")
        c._wait(is_tag)
        return [text(l) for l in c.lines[start:]
                if text(l) is not None and not is_tag(l) and not text(l).startswith("IPv6: ")]

    WRAPPING = [
        # command, wraps to (what 0.14.0 did), reply 0.14.1 gives
        ("SetBefore 4294967304", "before cap set to 8", "Usage: SetBefore <count>"),
        ("SetAfter 4294967305", "after cap set to 9", "Usage: SetAfter <count>"),
        ("SetMaxEvents 4294967396", "max_events set to 100", "Usage: SetMaxEvents <count|0|off>"),
        ("SetMaxEventLines 4294967297", "max_event_lines set to 1 ", "Usage: SetMaxEventLines <count|0|off>"),
    ]

    def num_wrapping_rejected():
        """Values past 2^32 are rejected rather than wrapped, and leave the
        setting unchanged. The baseline is asserted to wrap, so the scenario
        is known to discriminate."""
        scn = "num_wrapping_rejected"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk("%s_%s" % (scn, tag), so)
            c = z.client()
            run(c, "SetBefore 5")
            run(c, "SetAfter 6")
            run(c, "SetMaxEvents 7")
            run(c, "SetMaxEventLines 50")
            replies = [run(c, cmd) for cmd, _, _ in WRAPPING]
            res[tag] = dict(replies=replies, caps=caps(c))
            c.quit(); finish(z)
        n, b = res["under_test"], res["baseline"]
        for (cmd, _, want), got in zip(WRAPPING, n["replies"]):
            check(scn, "'%s' rejected" % cmd, len(got) == 1 and got[0].startswith(want), repr(got))
        check(scn, "settings unchanged", n["caps"] == {"before cap": "5", "after cap": "6", "max_events": "7", "max_event_lines": "50"},
              repr(n["caps"]))
        if old_baseline():
            for (cmd, wrapped, _), got in zip(WRAPPING, b["replies"]):
                check(scn, "baseline: '%s' wraps" % cmd, len(got) >= 1 and got[0].startswith(wrapped), repr(got))
        else:
            check(scn, "baseline %s rejects the same way" % baseline_version(), b == n, repr(b))

    def num_delexclude_index():
        """All-digit DelExclude arguments are indexes. 0 and numbers too large
        for an index are out of range; before 0.14.1, 4294967297 wrapped to 1
        and removed the first exclusion, and 0 fell through to the nick/mask
        lookup."""
        scn = "num_delexclude_index"
        probes = ["DelExclude 4294967297", "DelExclude 0", "DelExclude 99999999999999999999", "DelExclude 3"]
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk("%s_%s" % (scn, tag), so)
            c = z.client()
            run(c, "AddExclude #x")
            run(c, "AddExclude badnick")
            replies = [run(c, p) for p in probes]
            after = run(c, "ListExcludes")
            ok2 = run(c, "DelExclude 2")
            ok1 = run(c, "DelExclude 01")
            res[tag] = dict(replies=replies, after=after, ok=(ok2, ok1))
            c.quit(); finish(z)
        n, b = res["under_test"], res["baseline"]
        check(scn, "rejected as out of range",
              n["replies"] == [["Index 4294967297 is out of range."], ["Index 0 is out of range."],
                               ["Index 99999999999999999999 is out of range."], ["Index 3 is out of range."]],
              repr(n["replies"]))
        check(scn, "nothing removed", any("#x" in x for x in n["after"]) and any("badnick" in x for x in n["after"]),
              repr(n["after"]))
        check(scn, "valid indexes still work, leading zeros included",
              n["ok"] == (["Removed nick exclusion: badnick"], ["Removed channel exclusion: #x"]), repr(n["ok"]))
        if old_baseline():
            check(scn, "baseline: 4294967297 removed entry #1, 0 went to the nick/mask lookup",
                  b["replies"][0] == ["Removed channel exclusion: #x"] and b["replies"][1] == ["That nick/mask is not currently excluded."],
                  repr(b["replies"]))
        else:
            check(scn, "baseline %s behaves the same" % baseline_version(), b == n, repr(b))

    def num_boundaries():
        """The largest accepted value is 4294967295 (UINT_MAX); one more is
        rejected. Leading zeros are accepted however many there are; signs,
        exponents and other text are not."""
        scn = "num_boundaries"
        z = mk(scn, V090)
        c = z.client()
        out = {cmd: run(c, cmd) for cmd in (
            "SetMaxEvents 4294967295", "SetMaxEvents 4294967296", "SetBefore 0008",
            "SetAfter 00000000000000000000000009", "SetBefore -1", "SetBefore +5", "SetBefore 1e3", "SetBefore 5x")}
        cp = caps(c)
        c.quit(); finish(z)
        check(scn, "UINT_MAX accepted", out["SetMaxEvents 4294967295"][:1] and out["SetMaxEvents 4294967295"][0].startswith("max_events set to 4294967295 "),
              repr(out["SetMaxEvents 4294967295"]))
        check(scn, "UINT_MAX + 1 rejected", out["SetMaxEvents 4294967296"][:1] and out["SetMaxEvents 4294967296"][0].startswith("Usage: SetMaxEvents"),
              repr(out["SetMaxEvents 4294967296"]))
        check(scn, "leading zeros", out["SetBefore 0008"] == ["before cap set to 8"]
              and out["SetAfter 00000000000000000000000009"][:1] and out["SetAfter 00000000000000000000000009"][0].startswith("after cap set to 9 "),
              repr(out))
        check(scn, "signs, exponents, trailing text rejected",
              all(out[k] == ["Usage: SetBefore <count>"] for k in ("SetBefore -1", "SetBefore +5", "SetBefore 1e3", "SetBefore 5x")), repr(out))
        check(scn, "final caps", cp["before cap"] == "8" and cp["after cap"] == "9" and cp["max_events"] == "4294967295", repr(cp))

    def num_natural_size_note():
        """SetMaxEventLines warns when the cap is below before + 1 + after.
        With before at its maximum that sum needs more than 32 bits: the
        baseline wrapped it to 8 and printed no note."""
        scn = "num_natural_size_note"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk("%s_%s" % (scn, tag), so)
            c = z.client()
            run(c, "SetBefore 4294967295")
            run(c, "SetAfter 8")
            res[tag] = run(c, "SetMaxEventLines 100")
            run(c, "SetBefore 8")
            c.quit(); finish(z)
        n, b = res["under_test"], res["baseline"]
        check(scn, "note shows the full sum", any("= 4294967304 lines" in x for x in n), repr(n))
        if old_baseline():
            check(scn, "baseline: sum wrapped, no note", not any(x.startswith("note:") for x in b), repr(b))

    def num_load_args():
        """The before, after, max_events and max_event_lines load arguments
        use the same parser. A runtime LoadMod with a wrapping value fails with
        the existing error message instead of loading with the wrapped value."""
        scn = "num_load_args"
        args = [("before=4294967304", "Invalid value for before."), ("after=4294967304", "Invalid value for after."),
                ("max_events=4294967396", "Invalid value for max_events"),
                ("max_event_lines=4294967297", "Invalid value for max_event_lines")]
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk("%s_%s" % (scn, tag), so)
            c = z.client()
            out = []
            for a, _ in args:
                status_raw(c, "UnloadMod highlightctx")
                out.append(status_raw(c, "LoadMod --type=network highlightctx " + a))
            status_raw(c, "UnloadMod highlightctx")
            status_raw(c, "LoadMod --type=network highlightctx")
            res[tag] = dict(out=out, caps=caps(c))
            c.quit(); finish(z)
        n, b = res["under_test"], res["baseline"]
        for (a, err), got in zip(args, n["out"]):
            check(scn, "'%s' refused with the existing message" % a, any(err in x for x in got) and not any("Loaded highlightctx" in x for x in got),
                  repr(got))
        check(scn, "a plain reload afterwards keeps the earlier settings",
              n["caps"] == {"before cap": "8", "after cap": "8", "max_events": "100", "max_event_lines": "100"}, repr(n["caps"]))
        if old_baseline():
            check(scn, "baseline: loads with the wrapped values",
                  all(any("Loaded highlightctx" in x for x in got) for got in b["out"])
                  and any("before=8 " in x for x in b["out"][0]), repr(b["out"]))

    def num_conf_load_arg():
        """The same value in znc.conf. A network module that refuses to load
        is fatal while ZNC reads its config, so 0.14.1 stops ZNC from starting
        where 0.14.0 started with the wrapped value. Recorded because it is the
        one way this fix can bite an existing setup."""
        scn = "num_conf_load_arg"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            try:
                z = mk("%s_%s" % (scn, tag), so, load="before=4294967304 after=8")
                started = True
            except RuntimeError:
                started = False
                z = ns["open_zncs"][-1]
            out = z.output()
            res[tag] = dict(started=started, invalid="Invalid value for before." in out,
                            fatal="Unrecoverable config error" in out)
            finish(z)
        n, b = res["under_test"], res["baseline"]
        check(scn, "ZNC refuses to start, naming the argument", not n["started"] and n["invalid"] and n["fatal"], repr(n))
        if old_baseline():
            check(scn, "baseline: starts with the wrapped value", b["started"], repr(b))

    def num_version_marker():
        """The module reports 0.14.1."""
        scn = "num_version_marker"
        z = mk(scn, V090)
        c = z.client()
        out = run(c, "Version") + [b for (t, b) in c.attach_lines if b.startswith("Version: ")]
        c.quit(); finish(z)
        check(scn, "Version reports 0.14.1", any(re.search(r"highlightctx 0\.14\.1\b", b) for b in out), repr(out))

    ns["NUMERIC"] = [num_wrapping_rejected, num_delexclude_index, num_boundaries, num_natural_size_note,
                     num_load_args, num_conf_load_arg, num_version_marker]
