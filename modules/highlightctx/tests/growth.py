"""Growth-control scenarios for highlightctx 0.11.0.

Covers max_events defaults, max_event_lines (per-event size cap),
max_event_age (retention expiry), and drop reporting.

Imported by suite.py; every function follows the suite's check() convention.
"""
import os
import time


def install(ns):
    mk = ns["mk"]
    check = ns["check"]
    filler = ns["filler"]
    labels = ns["labels"]
    finish = ns["finish"]
    events_of = ns["events_of"]
    parse_replay = ns["parse_replay"]
    P = ns["P"]
    L = ns["L"]
    V090 = ns["V090"]          # module under test (0.11.0)

    def notes(client):
        """Module output lines that are drop notices rather than replay lines."""
        return [b for (t, b) in client.attach_lines if b.startswith("note: ")]

    # ---------------- defaults ----------------

    def gc_defaults():
        scn = "gc_defaults"
        z = mk(scn, V090)                      # no load args at all
        c = z.client()
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        out = z.output()
        finish(z)
        check(scn, "max_events defaults to 100", "max_events: 100" in st, repr([b for b in st if b.startswith("max_event")]))
        check(scn, "max_event_lines defaults to 100", "max_event_lines: 100" in st, repr([b for b in st if b.startswith("max_event")]))
        check(scn, "max_event_age defaults to disabled", "max_event_age: disabled" in st, repr([b for b in st if b.startswith("max_event")]))
        check(scn, "drop counters start at zero",
              any(b == "events dropped since last replay: 0 by max_events, 0 by max_event_age" for b in st), repr(st))
        check(scn, "load message states all three", "max_events=100" in out and "max_event_lines=100" in out and "max_event_age=disabled" in out,
              [l for l in out.splitlines() if "max_events=" in l][-1:])

    def gc_explicit_disable():
        scn = "gc_explicit_disable"
        z = mk(scn, V090, load="max_events=off max_event_lines=off max_event_age=off")
        c = z.client()
        st = [b for (t, b) in c.command("Status")]
        c.quit(); finish(z)
        check(scn, "all three can be disabled explicitly",
              "max_events: disabled" in st and "max_event_lines: disabled" in st and "max_event_age: disabled" in st,
              repr([b for b in st if b.startswith("max_event")]))

    # ---------------- max_event_lines ----------------

    def gc_lines_boundary():
        """before=8 after=8 cap=34.

        A trigger only extends while the event is still open, so each one must
        land inside the current window (after-index <= target-1):
          L0  -> target 8,  total 17
          L8  (idx 7)  -> target 16, total 25  <= 34  allowed
          L16 (idx 15) -> target 24, total 33  <= 34  allowed
          L24 (idx 23) -> target 32, total 41  >  34  SUPPRESSED
        """
        scn = "gc_lines_boundary"
        z = mk(scn, V090, load="before=8 after=8 max_event_lines=34")
        d = z.ircd
        filler(d, "#a", P(8))
        d.say("#a", "bob", "L0 tim t1")
        filler(d, "#a", L(1, 7))
        d.say("#a", "carol", "L8 tim t2")
        filler(d, "#a", L(9, 15))
        d.say("#a", "dave", "L16 tim t3")
        filler(d, "#a", L(17, 23))
        d.say("#a", "erin", "L24 tim t4")
        filler(d, "#a", L(25, 40))
        d.sync()
        j = z.journal_lines()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "one event", len(ev) == 1, repr([e["raw_header"] for e in ev]))
        if ev:
            e = ev[0]
            check(scn, "target stopped at 24, capped, all 4 triggers marked",
                  e["raw_header"] == "[#a] highlight event #1 (complete, before=8, after=24/24, triggers=4, capped)",
                  e["raw_header"])
            check(scn, "total captured lines (33) stays within the cap (34)", len(e["lines"]) == 33, str(len(e["lines"])))
            check(scn, "suppressed trigger still present and marked",
                  any(l["marked"] and l["text"].endswith("L24 tim t4") for l in e["lines"]), repr(labels(e)))
            check(scn, "last allowed extension took effect (L16 marked, target grew)",
                  any(l["marked"] and l["text"].endswith("L16 tim t3") for l in e["lines"]), repr(labels(e)))
        check(scn, "C record written once", [l for l in j if l.startswith("C\t")] == ["C\t1"], repr([l for l in j if l.startswith("C\t")]))

    def gc_lines_never_truncates_window():
        """A cap below the natural event size disables extension but keeps the window."""
        scn = "gc_lines_never_truncates_window"
        z = mk(scn, V090, load="before=8 after=8 max_event_lines=5")
        d = z.ircd
        filler(d, "#a", P(8))
        d.say("#a", "bob", "L0 tim t1")
        filler(d, "#a", L(1, 3))
        d.say("#a", "carol", "L4 tim t2")        # would extend; must be suppressed
        filler(d, "#a", L(5, 20))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "full configured window still captured (8 before + trigger + 8 after)",
              len(ev) == 1 and labels(ev[0]) == P(8) + ["*L0"] + L(1, 3) + ["*L4"] + L(5, 8),
              repr(labels(ev[0])) if ev else "")
        check(scn, "marked capped, target unchanged at 8", ev and ev[0]["target"] == 8 and "capped" in ev[0]["raw_header"], ev[0]["raw_header"] if ev else "")

    def gc_lines_next_event_is_fresh():
        """After a capped event finalizes, the next highlight starts a new event."""
        scn = "gc_lines_next_event_is_fresh"
        z = mk(scn, V090, load="before=2 after=3 max_event_lines=6")
        d = z.ircd
        filler(d, "#a", P(2))
        d.say("#a", "bob", "L0 tim t1")
        d.say("#a", "carol", "L1 tim t2")        # suppressed: would make total 2+1+5=8 > 6
        filler(d, "#a", L(2, 3))                 # event completes at target 3
        d.say("#a", "dave", "L4 tim t3")         # new event
        filler(d, "#a", L(5, 8))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "two events, first capped, second clean",
              [e["raw_header"] for e in ev] == [
                  "[#a] highlight event #1 (complete, before=2, after=3/3, triggers=2, capped)",
                  "[#a] highlight event #2 (complete, before=2, after=3/3)"],
              repr([e["raw_header"] for e in ev]))

    def gc_lines_command_applies_to_next_capture():
        """The cap is evaluated live, not captured per event at start.

        Note: while a client is attached there are never any open events —
        attaching finalizes and replays them — so the observable effect of
        changing the cap is on the capture that follows the next detach.
        """
        scn = "gc_lines_command_applies_to_next_capture"
        z = mk(scn, V090, load="before=2 after=4 max_event_lines=off")
        c = z.client()
        out = [b for (t, b) in c.command("SetMaxEventLines 7")]
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        d = z.ircd
        filler(d, "#a", P(2))
        d.say("#a", "bob", "L0 tim t1")           # target 4, total 7
        filler(d, "#a", ["L1"])
        d.say("#a", "carol", "L2 tim t2")         # idx 1 -> target 6 -> total 9 > 7: suppressed
        filler(d, "#a", L(3, 10))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "command confirms the new value", any("max_event_lines set to 7" in b for b in out), repr(out))
        check(scn, "command does not claim to affect open events",
              not any("applies to open events immediately" in b for b in out), repr(out))
        check(scn, "Status reflects it", "max_event_lines: 7" in st, repr([b for b in st if b.startswith("max_event_lines")]))
        check(scn, "next capture is capped by the new value",
              len(ev) == 1 and ev[0]["capped"] and ev[0]["target"] == 4,
              repr([e["raw_header"] for e in ev]))

    def gc_lines_survives_restart():
        """A capped event keeps its target and flag across SIGKILL recovery."""
        scn = "gc_lines_survives_restart"
        # before=4 after=6 cap=20:
        #   L0            -> target 6,  total 11
        #   L6  (idx 5)   -> target 12, total 17 <= 20  allowed
        #   L8  (idx 7)   -> target 14, total 19 <= 20  allowed
        #   L10 (idx 9)   -> target 16, total 21 >  20  SUPPRESSED, event still open
        z = mk(scn, V090, load="before=4 after=6 max_event_lines=20")
        d = z.ircd
        filler(d, "#a", P(4))
        d.say("#a", "bob", "L0 tim t1")
        filler(d, "#a", L(1, 5))
        d.say("#a", "carol", "L6 tim t2")
        filler(d, "#a", ["L7"])
        d.say("#a", "dave", "L8 tim t3")
        filler(d, "#a", ["L9"])
        d.say("#a", "erin", "L10 tim t4")
        d.sync()
        before = z.journal_lines()
        z.stop(kill=True)
        z.start()
        filler(d, "#a", L(11, 20))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "C record present before crash", any(l.startswith("C\t") for l in before), repr([l for l in before if not l.startswith("A")]))
        check(scn, "recovered with the same target (14), capped flag and all 4 triggers",
              len(ev) == 1 and ev[0]["raw_header"] == "[#a] highlight event #1 (complete, before=4, after=14/14, triggers=4, capped)",
              repr([e["raw_header"] for e in ev]))
        check(scn, "event was still open at the crash and completed afterwards",
              ev and labels(ev[0]) == P(4) + ["*L0"] + L(1, 5) + ["*L6", "L7", "*L8", "L9", "*L10"] + L(11, 14),
              repr(labels(ev[0])) if ev else "")

    def gc_lines_lowered_between_sessions():
        scn = "gc_lines_lowered_between_sessions"
        z = mk(scn, V090, load="before=2 after=5 max_event_lines=off")
        d = z.ircd
        filler(d, "#a", P(2))
        d.say("#a", "bob", "L0 tim t1")
        filler(d, "#a", L(1, 3))
        d.say("#a", "carol", "L4 tim t2")         # target 10, total 13
        d.sync()
        z.stop(kill=True)
        z.load_lines = ["highlightctx before=2 after=5 max_event_lines=8"]
        z.write_config()
        z.start()
        filler(d, "#a", L(5, 20))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "recovered event stops growing under the lowered cap",
              len(ev) == 1 and ev[0]["target"] <= 10 and "capped" in ev[0]["raw_header"],
              repr([e["raw_header"] for e in ev]))
        check(scn, "configured window still intact (target >= after cap)", ev and ev[0]["target"] >= 5, repr([e["raw_header"] for e in ev]))

    def gc_lines_validation():
        scn = "gc_lines_validation"
        z = mk(scn, V090)
        c = z.client()
        for val, want in (("60", "max_event_lines set to 60"), ("off", "disabled"), ("0", "disabled"),
                          ("5", "extension is effectively disabled"), ("abc", "Usage:"), ("", "Usage:")):
            out = [b for (t, b) in c.command("SetMaxEventLines " + val)]
            check(scn, "SetMaxEventLines %r -> %s" % (val, want), any(want in b for b in out), repr(out))
        c.quit(); finish(z)

    def gc_lines_bounds_a_flood():
        """The point of the feature: a sustained flood cannot grow one event without limit."""
        scn = "gc_lines_bounds_a_flood"
        z = mk(scn, V090, load="before=8 after=8 max_event_lines=50")
        d = z.ircd
        filler(d, "#a", P(8))
        for i in range(200):
            d.say("#a", "spammer", "S%d tim" % i)
        filler(d, "#a", L(1, 20))
        d.sync()
        j = z.journal_lines()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        oversize = [e for e in ev if len(e["lines"]) > 50]
        print("    info: 200 consecutive highlights, cap 50 -> %d events, max event %d lines, journal %d lines" % (
            len(ev), max(len(e["lines"]) for e in ev) if ev else 0, len(j)))
        check(scn, "no event exceeds the cap", not oversize, repr([e["raw_header"] for e in oversize]))
        check(scn, "highlights are not lost: several bounded events instead of one huge one", len(ev) >= 2, str(len(ev)))
        check(scn, "every event past the first is capped or complete", all(e["state"] in ("complete", "partial") for e in ev), "")

    # ---------------- max_event_age ----------------

    def gc_age_expires_old_events():
        """Events older than the limit are dropped without being replayed."""
        scn = "gc_age_expires_old_events"
        z = mk(scn, V090, load="before=2 after=2 max_event_age=off")
        d = z.ircd
        # The fake server stamps lines with its own clock; rewind it so these
        # events are genuinely old by wall-clock time.
        d.ts = int(time.time()) - 7 * 86400
        d.say("#a", "bob", "OLD0 tim week-old")
        filler(d, "#a", ["OLD1", "OLD2"])
        d.sync()
        d.ts = int(time.time()) - 60
        d.say("#a", "carol", "NEW0 tim recent")
        filler(d, "#a", ["NEW1", "NEW2"])
        d.sync()
        c = z.client()
        pre = events_of(c)
        c.quit()
        check(scn, "both events present with expiry off", len(pre) == 2, repr([e["raw_header"] for e in pre]))
        # same traffic again, this time with a 1d limit
        z2 = mk(scn + "_on", V090, load="before=2 after=2 max_event_age=1d")
        d2 = z2.ircd
        d2.ts = int(time.time()) - 7 * 86400
        d2.say("#a", "bob", "OLD0 tim week-old")
        filler(d2, "#a", ["OLD1", "OLD2"])
        d2.sync()
        d2.ts = int(time.time()) - 60
        d2.say("#a", "carol", "NEW0 tim recent")
        filler(d2, "#a", ["NEW1", "NEW2"])
        d2.sync()
        c2 = z2.client()
        post = events_of(c2)
        note = notes(c2)
        st = [b for (t, b) in c2.command("Status")]
        c2.quit()
        finish(z); finish(z2)
        check(scn, "only the recent event is replayed", len(post) == 1 and "NEW0" in "".join(l["text"] for l in post[0]["lines"]),
              repr([e["raw_header"] for e in post]))
        check(scn, "expiry is reported at replay", any("expired after max_event_age=1d" in n for n in note), repr(note))
        check(scn, "Status shows the configured age", "max_event_age: 1d" in st, repr([b for b in st if b.startswith("max_event_age")]))
        check(scn, "counters cleared after reporting",
              any(b == "events dropped since last replay: 0 by max_events, 0 by max_event_age" for b in st), repr(st))

    def gc_age_expiry_at_load():
        """Expiry runs on journal recovery, not only while running."""
        scn = "gc_age_expiry_at_load"
        z = mk(scn, V090, load="before=2 after=2 max_event_age=off")
        d = z.ircd
        d.ts = int(time.time()) - 30 * 86400
        d.say("#a", "bob", "OLD0 tim month-old")
        filler(d, "#a", ["OLD1", "OLD2"])
        d.sync()
        check(scn, "event journaled while expiry was off", any(l.startswith("B\t") for l in z.journal_lines()), "")
        z.stop()
        z.load_lines = ["highlightctx before=2 after=2 max_event_age=7d"]
        z.write_config()
        z.start()
        d.say("#b", "x", "unrelated")
        d.sync()
        c = z.client()
        ev = events_of(c)
        note = notes(c)
        c.quit(); finish(z)
        check(scn, "stale event dropped at load", ev == [], repr([e["raw_header"] for e in ev]))
        check(scn, "drop reported on next replay", any("expired" in n for n in note), repr(note))

    def gc_age_command_takes_effect_after_detach():
        """SetMaxEventAge persists and governs the capture that follows.

        While a client is attached the pending queue is always empty, because
        attaching replays and clears it, so the command itself never has
        anything to drop. Expiry happens at load and as traffic arrives.
        """
        scn = "gc_age_command_takes_effect_after_detach"
        z = mk(scn, V090, load="before=2 after=2")
        c = z.client()
        out = [b for (t, b) in c.command("SetMaxEventAge 2d")]
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        d = z.ircd
        d.ts = int(time.time()) - 10 * 86400
        d.say("#a", "bob", "OLD0 tim stale")
        filler(d, "#a", ["OLD1", "OLD2"])
        d.sync()
        d.ts = int(time.time()) - 30
        d.say("#a", "carol", "NEW0 tim fresh")
        filler(d, "#a", ["NEW1", "NEW2"])
        d.sync()
        c = z.client()
        ev = events_of(c)
        note = notes(c)
        c.quit(); finish(z)
        check(scn, "command confirms the value", any("max_event_age set to 2d" in b for b in out), repr(out))
        check(scn, "command makes no claim about dropping now",
              not any("already older than the new limit" in b for b in out), repr(out))
        check(scn, "Status shows 2d", "max_event_age: 2d" in st, repr([b for b in st if b.startswith("max_event_age")]))
        check(scn, "stale event expired, fresh one replayed",
              len(ev) == 1 and "NEW0" in "".join(l["text"] for l in ev[0]["lines"]), repr([e["raw_header"] for e in ev]))
        check(scn, "expiry reported", any("expired after max_event_age=2d" in n for n in note), repr(note))

    def gc_age_validation():
        scn = "gc_age_validation"
        z = mk(scn, V090)
        c = z.client()
        for val, want in (("30d", "max_event_age set to 30d"), ("12h", "set to 12h"), ("90m", "set to 90m"),
                          ("3600s", "set to 1h"), ("2w", "set to 2w"), ("off", "disabled"), ("0", "disabled"),
                          ("banana", "Usage:"), ("", "Usage:"), ("5x", "Usage:"), ("-1d", "Usage:")):
            out = [b for (t, b) in c.command("SetMaxEventAge " + val)]
            check(scn, "SetMaxEventAge %r -> %s" % (val, want), any(want in b for b in out), repr(out))
        c.quit(); finish(z)

    def gc_age_load_arg_rejects_garbage():
        scn = "gc_age_load_arg_rejects_garbage"
        logpath = os.path.join(ns["ROOT"], scn, "znc.out")
        z = None
        loaded = False
        try:
            z = mk(scn, V090, load="max_event_age=banana")
            loaded = True
        except Exception:
            pass
        finally:
            if z:
                finish(z)
        try:
            out = open(logpath, errors="replace").read()
        except FileNotFoundError:
            out = ""
        check(scn, "bad duration fails the module load", not loaded and "Invalid value for max_event_age" in out, out[-200:])

    # ---------------- drop reporting ----------------

    def gc_cap_drop_is_reported():
        """max_events shedding must not be silent (it was, before 0.11.0)."""
        scn = "gc_cap_drop_is_reported"
        z = mk(scn, V090, load="before=1 after=1 max_events=3")
        d = z.ircd
        for k in range(7):                          # 7 events, cap 3 -> 4 dropped
            d.say("#a", "bob", "E%d tim" % k)
            filler(d, "#a", ["F%d" % k])
        d.sync()
        c = z.client()
        ev = events_of(c)
        note = notes(c)
        st = [b for (t, b) in c.command("Status")]
        c.quit(); finish(z)
        check(scn, "only the cap's worth of events replayed", len(ev) == 3, repr([e["raw_header"] for e in ev]))
        check(scn, "newest events kept", [e["id"] for e in ev] == [5, 6, 7], repr([e["id"] for e in ev]))
        check(scn, "drop count reported at replay",
              any("4 older highlight event(s) were dropped" in n and "max_events=3" in n for n in note), repr(note))
        check(scn, "counters cleared after the report",
              any(b == "events dropped since last replay: 0 by max_events, 0 by max_event_age" for b in st), repr(st))

    def gc_drop_counter_survives_restart():
        scn = "gc_drop_counter_survives_restart"
        z = mk(scn, V090, load="before=1 after=1 max_events=2")
        d = z.ircd
        for k in range(5):
            d.say("#a", "bob", "E%d tim" % k)
            filler(d, "#a", ["F%d" % k])
        d.sync()
        z.stop(kill=True)
        z.start()
        c = z.client()
        note = notes(c)
        c.quit(); finish(z)
        check(scn, "drops survive a crash and are still reported", any("were dropped before this replay" in n for n in note), repr(note))

    def gc_no_note_when_nothing_dropped():
        scn = "gc_no_note_when_nothing_dropped"
        z = mk(scn, V090, load="before=2 after=2")
        d = z.ircd
        d.say("#a", "bob", "L0 tim")
        filler(d, "#a", ["L1", "L2"])
        d.sync()
        c = z.client()
        note = notes(c)
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "no drop note in the normal case", note == [], repr(note))
        check(scn, "event still replayed", len(ev) == 1, repr([e["raw_header"] for e in ev]))

    def gc_default_cap_holds_100():
        """The chosen default keeps the 100 most recent events, not fewer."""
        scn = "gc_default_cap_holds_100"
        z = mk(scn, V090, load="before=0 after=0")   # after=0: one event per highlight, no open events
        d = z.ircd
        for k in range(120):
            d.say("#a", "bob", "E%d tim" % k)
        d.sync()
        j = z.journal_lines()
        c = z.client()
        ev = events_of(c)
        note = notes(c)
        c.quit(); finish(z)
        check(scn, "exactly 100 events retained", len(ev) == 100, str(len(ev)))
        check(scn, "the newest 100 are the ones kept", [e["id"] for e in ev] == list(range(21, 121)), repr([e["id"] for e in ev][:3]))
        check(scn, "20 drops reported", any("20 older highlight event(s) were dropped" in n for n in note), repr(note))
        print("    info: 120 highlights at default cap -> %d events retained, journal %d lines" % (len(ev), len(j)))

    def gc_thrash_at_default_settings():
        """Quantify journal rewrite frequency at the 0.11.0 defaults.

        Compaction rewrites the whole journal whenever it exceeds
        kCompactThresholdLines (512). A compacted default-sized event is
        10 journal lines -- the 8 before-lines live inside the B record,
        not as separate lines -- so the threshold is crossed at ~51 pending
        events, and the default max_events=100 allows a steady state well
        above it. This scenario measures; it does not assert a verdict. The
        rewrite-per-line behaviour is pre-existing and out of scope here.
        """
        scn = "gc_thrash_at_default_settings"
        z = mk(scn, V090)                       # stock defaults
        d = z.ircd
        for k in range(70):                     # 70 events x 10 journal lines = 700 > 512
            filler(d, "#a", ["C%d_%d" % (k, i) for i in range(8)])
            d.say("#a", "bob", "H%d tim" % k)
            filler(d, "#a", ["A%d_%d" % (k, i) for i in range(8)])
        d.sync()
        jlines = len(z.journal_lines())
        rewrites = 0
        for k in range(10):
            with open(z.journal_path(), "a") as f:
                f.write("Zmarker\t%d\n" % k)
            d.say("#b", "alice", "unrelated %d" % k)
            d.sync()
            if not any(l.startswith("Zmarker") for l in z.journal_lines()):
                rewrites += 1
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        print("    info: at stock defaults, %d pending events -> %d journal lines; "
              "%d/10 unrelated channel lines triggered a full journal rewrite" % (len(ev), jlines, rewrites))
        check(scn, "all 70 events retained (below max_events=100)", len(ev) == 70, str(len(ev)))
        check(scn, "journal exceeds the 512-line compaction threshold at these settings", jlines > 512, str(jlines))

    ns["GROWTH"] = [gc_thrash_at_default_settings, gc_defaults, gc_explicit_disable,
                    gc_lines_boundary, gc_lines_never_truncates_window, gc_lines_next_event_is_fresh,
                    gc_lines_command_applies_to_next_capture, gc_lines_survives_restart, gc_lines_lowered_between_sessions,
                    gc_lines_validation, gc_lines_bounds_a_flood,
                    gc_age_expires_old_events, gc_age_expiry_at_load, gc_age_command_takes_effect_after_detach,
                    gc_age_validation, gc_age_load_arg_rejects_garbage,
                    gc_cap_drop_is_reported, gc_drop_counter_survives_restart, gc_no_note_when_nothing_dropped,
                    gc_default_cap_holds_100]
