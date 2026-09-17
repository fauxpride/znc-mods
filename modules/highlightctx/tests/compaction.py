"""Journal compaction scenarios for highlightctx 0.11.1.

The fix makes the compaction trigger point relative to what the last
compaction achieved (max(512, 2 x lines written)) instead of a fixed 512,
so the journal must at least double before being rewritten again.

These scenarios measure rewrite frequency directly by appending a marker line
to the journal and checking whether a subsequent channel line removed it: a
full rewrite drops any line the module did not write itself.

Imported by suite.py; every function follows the suite's check() convention.
"""
import os


def install(ns):
    mk = ns["mk"]
    check = ns["check"]
    filler = ns["filler"]
    labels = ns["labels"]
    finish = ns["finish"]
    events_of = ns["events_of"]
    P = ns["P"]
    L = ns["L"]
    V090 = ns["V090"]          # build under test (0.11.1)
    V080 = ns["V080"]          # baseline (0.11.0)

    def fill_pending(d, chan, count, tag):
        """Generate `count` finalized default-sized events (10 journal lines each)."""
        for k in range(count):
            filler(d, chan, ["%sC%d_%d" % (tag, k, i) for i in range(8)])
            d.say(chan, "bob", "%sH%d tim" % (tag, k))
            filler(d, chan, ["%sA%d_%d" % (tag, k, i) for i in range(8)])

    def rewrites_per_lines(z, d, n):
        """Append a marker, send one unrelated channel line, see if it survived."""
        hits = 0
        for k in range(n):
            with open(z.journal_path(), "a") as f:
                f.write("Zmarker\t%d\n" % k)
            d.say("#b", "alice", "unrelated %d" % k)
            d.sync()
            if not any(l.startswith("Zmarker") for l in z.journal_lines()):
                hits += 1
        return hits

    def cmp_thrash_fixed():
        """The headline fix, measured on both versions with identical traffic."""
        scn = "cmp_thrash_fixed"
        res = {}
        for tag, so in (("v080", V080), ("v090", V090)):
            z = mk(scn + "_" + tag, so)          # stock defaults
            d = z.ircd
            fill_pending(d, "#a", 70, "")
            d.sync()
            jlines = len(z.journal_lines())
            res[tag] = (rewrites_per_lines(z, d, 10), jlines)
            c = z.client()
            res[tag] += (len(events_of(c)),)
            c.quit(); finish(z)
        print("    info: 70 pending events -> baseline %d/10 rewrites (%d journal lines), "
              "fixed %d/10 rewrites (%d journal lines)" % (res["v080"][0], res["v080"][1], res["v090"][0], res["v090"][1]))
        check(scn, "baseline rewrites on every line", res["v080"][0] == 10, repr(res["v080"]))
        check(scn, "fixed version does not rewrite per line", res["v090"][0] == 0, repr(res["v090"]))
        check(scn, "both retain all 70 events", res["v080"][2] == 70 and res["v090"][2] == 70, repr(res))

    def cmp_amortized_growth():
        """Rewrite count must grow logarithmically, not linearly, with appends."""
        scn = "cmp_amortized_growth"
        z = mk(scn, V090, load="max_events=off")
        d = z.ircd
        fill_pending(d, "#a", 60, "")            # ~600 live lines, past the 512 floor
        d.sync()
        start_lines = len(z.journal_lines())
        # Now append a lot: 100 more events (~1000 lines) on a channel with no
        # open event, so every line is journalled into pending state.
        rewrites = 0
        for k in range(100):
            with open(z.journal_path(), "a") as f:
                f.write("Zmarker\t%d\n" % k)
            filler(d, "#a", ["G%d_%d" % (k, i) for i in range(8)])
            d.say("#a", "bob", "G%dH tim" % k)
            filler(d, "#a", ["G%dA_%d" % (k, i) for i in range(8)])
            d.sync()
            if not any(l.startswith("Zmarker") for l in z.journal_lines()):
                rewrites += 1
        end_lines = len(z.journal_lines())
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        print("    info: 160 events appended over %d->%d journal lines -> %d compactions" % (
            start_lines, end_lines, rewrites))
        check(scn, "compactions are rare, not per-event", rewrites <= 5, str(rewrites))
        check(scn, "at least one compaction still happened", rewrites >= 1, str(rewrites))
        check(scn, "all 160 events retained", len(ev) == 160, str(len(ev)))

    def cmp_file_stays_bounded():
        """The trade: the file may sit at up to ~2x live state, but no more."""
        scn = "cmp_file_stays_bounded"
        z = mk(scn, V090, load="max_events=off")
        d = z.ircd
        fill_pending(d, "#a", 120, "")
        d.sync()
        jlines = len(z.journal_lines())
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        live = len(ev) * 10                      # 10 compacted journal lines per default event
        check(scn, "journal stays within ~2x live state", jlines <= live * 2 + 64,
              "journal=%d live~=%d" % (jlines, live))
        check(scn, "all events retained", len(ev) == 120, str(len(ev)))

    def cmp_garbage_still_reclaimed():
        """Compaction must still shrink the file once records become dead."""
        scn = "cmp_garbage_still_reclaimed"
        z = mk(scn, V090, load="before=8 after=8 max_events=40")
        d = z.ircd
        fill_pending(d, "#a", 120, "")           # 80 of these get dropped by the cap
        d.sync()
        before = len(z.journal_lines())
        c = z.client()
        ev = events_of(c)
        out = [b for (t, b) in c.command("Compact")]
        after = len(z.journal_lines())
        c.quit(); finish(z)
        check(scn, "cap honoured", len(ev) == 40, str(len(ev)))
        check(scn, "Compact reclaimed dead records", after < before and after <= 8, "before=%d after=%d" % (before, after))
        check(scn, "Compact reports success", any("Journal compacted" in b for b in out), repr(out))

    def cmp_recovery_from_uncompacted():
        """The main risk of the fix: recovery now reads a less-compacted file."""
        scn = "cmp_recovery_from_uncompacted"
        z = mk(scn, V090, load="before=4 after=6 max_event_lines=20")
        d = z.ircd
        fill_pending(d, "#a", 60, "")            # bulk, so the journal carries slack
        # a capped, extended event that must survive recovery exactly
        filler(d, "#a", P(4))
        d.say("#a", "bob", "L0 tim t1")
        filler(d, "#a", L(1, 5))
        d.say("#a", "carol", "L6 tim t2")        # target 12
        filler(d, "#a", ["L7"])
        d.say("#a", "dave", "L8 tim t3")         # target 14
        filler(d, "#a", ["L9"])
        d.say("#a", "erin", "L10 tim t4")        # suppressed by cap
        d.sync()
        jlines = len(z.journal_lines())
        z.stop(kill=True)
        z.start()
        filler(d, "#a", L(11, 20))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        target = [e for e in ev if any("L0 tim t1" in l["text"] for l in e["lines"])]
        print("    info: recovered from a %d-line journal, %d events" % (jlines, len(ev)))
        check(scn, "all events recovered", len(ev) == 61, str(len(ev)))
        check(scn, "capped extended event recovered exactly",
              len(target) == 1 and target[0]["raw_header"].endswith("(complete, before=4, after=14/14, triggers=4, capped)"),
              repr([e["raw_header"] for e in target]))
        check(scn, "its lines are intact",
              target and labels(target[0]) == P(4) + ["*L0"] + L(1, 5) + ["*L6", "L7", "*L8", "L9", "*L10"] + L(11, 14),
              repr(labels(target[0])) if target else "")

    def cmp_small_journal_unchanged():
        """Below the floor, behaviour is exactly as before the fix."""
        scn = "cmp_small_journal_unchanged"
        res = {}
        for tag, so in (("v080", V080), ("v090", V090)):
            z = mk(scn + "_" + tag, so)
            d = z.ircd
            fill_pending(d, "#a", 10, "")        # ~100 journal lines, well under 512
            d.sync()
            j = z.journal_lines()
            c = z.client()
            res[tag] = (j, events_of(c), len(z.journal_lines()))
            c.quit(); finish(z)
        check(scn, "journal byte-identical below the floor", res["v080"][0] == res["v090"][0],
              "%d vs %d lines" % (len(res["v080"][0]), len(res["v090"][0])))
        check(scn, "replay identical", res["v080"][1] == res["v090"][1], "")
        check(scn, "post-replay compaction identical", res["v080"][2] == res["v090"][2], repr((res["v080"][2], res["v090"][2])))

    def cmp_replay_resets_trigger():
        """After replay clears state, the trigger returns to the floor."""
        scn = "cmp_replay_resets_trigger"
        z = mk(scn, V090, load="max_events=off")
        d = z.ircd
        fill_pending(d, "#a", 80, "")            # pushes the trigger point well up
        d.sync()
        c = z.client()
        n1 = len(events_of(c))
        c.quit()
        after_replay = len(z.journal_lines())
        # A fresh, small journal must compact on the old rules again.
        fill_pending(d, "#a", 60, "R")
        d.sync()
        rewrites = rewrites_per_lines(z, d, 6)
        c = z.client()
        n2 = len(events_of(c))
        c.quit(); finish(z)
        check(scn, "first batch replayed", n1 == 80, str(n1))
        check(scn, "journal emptied after replay", after_replay <= 2, str(after_replay))
        check(scn, "second batch replayed", n2 == 60, str(n2))
        check(scn, "trigger point reset to the floor (no per-line rewrites either)", rewrites <= 1, str(rewrites))

    ns["COMPACTION"] = [cmp_thrash_fixed, cmp_amortized_growth, cmp_file_stays_bounded,
                        cmp_garbage_still_reclaimed, cmp_recovery_from_uncompacted,
                        cmp_small_journal_unchanged, cmp_replay_resets_trigger]
