"""journal=off opt-out scenarios for highlightctx 0.10.0.

Imported by suite.py; every function follows the suite's check() convention.
The central claim under test is that with journal=off the module writes
NOTHING to disk under the moddata directory other than ZNC's own .registry,
while behaving identically in-memory for a single session.
"""
import os
import time


def install(ns):
    """ns is suite.py's module namespace (mk, check, filler, labels, ... )."""
    mk = ns["mk"]
    check = ns["check"]
    filler = ns["filler"]
    labels = ns["labels"]
    finish = ns["finish"]
    events_of = ns["events_of"]
    P = ns["P"]
    L = ns["L"]
    V090 = ns["V090"]          # module under test (0.10.0)
    V080 = ns["V080"]          # baseline (0.9.0)

    def moddir(z):
        return os.path.dirname(z.journal_path())

    def dirstate(z):
        d = moddir(z)
        if not os.path.isdir(d):
            return {}
        out = {}
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            out[name] = os.path.getsize(p) if os.path.isfile(p) else "<dir>"
        return out

    def jo_no_disk_writes():
        """Core guarantee: nothing but .registry appears in moddata."""
        scn = "jo_no_disk_writes"
        z = mk(scn, V090, load="before=8 after=8 journal=off")
        d = z.ircd
        filler(d, "#a", P(8))
        d.say("#a", "bob", "L0 tim first")
        filler(d, "#a", L(1, 3))
        d.say("#a", "carol", "L4 tim second")     # extension path also silent
        filler(d, "#a", L(5, 12))
        d.say("#b", "dave", "M0 tim open event")  # left open on purpose
        filler(d, "#b", ["M1", "M2"])
        d.sync()
        mid = dirstate(z)
        c = z.client()
        ev = events_of(c)
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        d.say("#a", "bob", "post-replay tim")     # new event after replay
        d.sync()
        after = dirstate(z)
        finish(z)
        check(scn, "no journal file while capturing", "highlightctx.journal" not in mid, repr(mid))
        check(scn, "no journal file after replay", "highlightctx.journal" not in after, repr(after))
        check(scn, "no temp file either", not any(k.endswith(".tmp") for k in list(mid) + list(after)), repr(mid) + repr(after))
        check(scn, "only ZNC's own .registry present", set(after) <= {".registry"}, repr(after))
        check(scn, "capture still works in memory: extended + open partial",
              [e["raw_header"] for e in ev] == [
                  "[#a] highlight event #1 (complete, before=8, after=12/12, triggers=2)",
                  "[#b] highlight event #2 (partial, before=0, after=2/8)"],   # #b had no prior lines, so before is empty
              repr([e["raw_header"] for e in ev]))
        check(scn, "extended event content intact",
              ev and labels(ev[0]) == P(8) + ["*L0"] + L(1, 3) + ["*L4"] + L(5, 12), repr(labels(ev[0])) if ev else "")
        check(scn, "Status reports journal disabled", any(b.startswith("journal: disabled") for b in st), repr([b for b in st if b.startswith("journal")]))
        check(scn, "Status still reports the path", any(b.startswith("journal path: ") for b in st), repr(st))
        check(scn, "no leftover note when no file exists", not any("still exists at that path" in b for b in st), repr(st))

    def jo_same_replay_as_journal_on():
        """Within one session, journal=off must replay exactly what journal=on does."""
        scn = "jo_same_replay_as_journal_on"
        res = {}
        for tag, args in (("on", "before=6 after=5"), ("off", "before=6 after=5 journal=off")):
            z = mk(scn + "_" + tag, V090, load=args, chans=("#a", "#b"))
            d = z.ircd
            filler(d, "#a", P(6))
            d.say("#a", "bob", "L0 tim one")
            filler(d, "#a", L(1, 2))
            d.say("#a", "carol", "L3 tim two")
            filler(d, "#a", L(4, 20))
            d.say("#b", "dave", "M0 tim notice", kind="N")
            d.say("#b", "eve", "M1 waves at tim", kind="A")
            filler(d, "#b", ["M%d" % i for i in range(2, 9)])
            d.sync()
            c = z.client()
            res[tag] = events_of(c)
            c.quit(); finish(z)
        check(scn, "replay identical with journaling on and off", res["on"] == res["off"],
              "\non =%r\noff=%r" % (res["on"], res["off"]))
        check(scn, "and it captured something", len(res["off"]) == 2, repr([e["raw_header"] for e in res["off"]]))

    def jo_state_not_durable():
        """The documented trade-off: events do not survive a restart."""
        scn = "jo_state_not_durable"
        z = mk(scn, V090, load="before=4 after=8 journal=off")
        d = z.ircd
        filler(d, "#a", P(4))
        d.say("#a", "bob", "L0 tim open at crash")
        filler(d, "#a", L(1, 2))
        d.sync()
        check(scn, "still nothing on disk before crash", "highlightctx.journal" not in dirstate(z), repr(dirstate(z)))
        z.stop(kill=True)
        z.start()
        filler(d, "#a", L(3, 9))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "event lost across restart (as documented)", ev == [], repr([e["raw_header"] for e in ev]))

    def jo_setting_persists():
        """journal=off must survive a reload with no load args, via NV."""
        scn = "jo_setting_persists"
        z = mk(scn, V090, load="before=3 after=3 journal=off")
        c = z.client()
        st1 = [b for (t, b) in c.command("Status")]
        c.quit()
        z.stop()
        z.load_lines = ["highlightctx"]          # reload with NO arguments
        z.write_config()
        z.start()
        d = z.ircd
        d.say("#a", "bob", "L0 tim after argless reload")
        filler(d, "#a", L(1, 3))
        d.sync()
        c = z.client()
        st2 = [b for (t, b) in c.command("Status")]
        ev = events_of(c)
        c.quit()
        state = dirstate(z)
        finish(z)
        check(scn, "disabled before reload", any(b.startswith("journal: disabled") for b in st1), repr(st1))
        check(scn, "still disabled after argless reload", any(b.startswith("journal: disabled") for b in st2), repr(st2))
        check(scn, "still no journal file", "highlightctx.journal" not in state, repr(state))
        check(scn, "capture still works", len(ev) == 1, repr([e["raw_header"] for e in ev]))

    def jo_reset_does_not_reenable():
        """Reset must never silently resume writing to disk."""
        scn = "jo_reset_does_not_reenable"
        z = mk(scn, V090, load="before=3 after=3 journal=off excludes=#noise")
        c = z.client()
        out = [b for (t, b) in c.command("Reset")]
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        d = z.ircd
        d.say("#a", "bob", "L0 tim after reset")
        filler(d, "#a", L(1, 8))
        d.sync()
        state = dirstate(z)
        finish(z)
        check(scn, "Reset says the journal setting was untouched",
              any("journal setting was left unchanged" in b and "off" in b for b in out), repr(out))
        check(scn, "still disabled after Reset", any(b.startswith("journal: disabled") for b in st), repr(st))
        check(scn, "Reset did restore other defaults", any(b == "before cap: 8" for b in st) and any(b == "excluded channels: 0" for b in st), repr(st))
        check(scn, "no journal file after Reset + traffic", "highlightctx.journal" not in state, repr(state))

    def jo_leftover_file():
        """A journal written while enabled is ignored, reported, and removable."""
        scn = "jo_leftover_file"
        z = mk(scn, V090, load="before=8 after=8")
        d = z.ircd
        filler(d, "#a", P(8))
        d.say("#a", "bob", "L0 tim journaled")
        filler(d, "#a", L(1, 2))
        d.sync()
        jbefore = z.journal_lines()
        check(scn, "baseline: journal written while enabled", len(jbefore) > 0, repr(jbefore[:2]))
        z.stop()
        z.load_lines = ["highlightctx journal=off"]
        z.write_config()
        z.start()
        filler(d, "#a", L(3, 9))
        d.sync()
        check(scn, "leftover file untouched (not read, not written, not deleted)",
              z.journal_lines() == jbefore, "%d vs %d lines" % (len(z.journal_lines()), len(jbefore)))
        c = z.client()
        ev = events_of(c)
        st = [b for (t, b) in c.command("Status")]
        load_note = [l for l in z.output().splitlines() if "journal=off" in l and "still exists" in l]
        check(scn, "journaled event NOT recovered (journal not read)", ev == [], repr([e["raw_header"] for e in ev]))
        check(scn, "Status flags the leftover file", any("still exists at that path" in b for b in st), repr(st))
        out = [b for (t, b) in c.command("Compact")]
        check(scn, "Compact reports removing the leftover", any("Removed the leftover journal file" in b for b in out), repr(out))
        check(scn, "leftover file actually gone", "highlightctx.journal" not in dirstate(z), repr(dirstate(z)))
        st2 = [b for (t, b) in c.command("Status")]
        check(scn, "Status note clears once removed", not any("still exists at that path" in b for b in st2), repr(st2))
        out2 = [b for (t, b) in c.command("Compact")]
        check(scn, "second Compact is a clean no-op", any("no journal file exists" in b for b in out2), repr(out2))
        c.quit(); finish(z)

    def jo_reenable():
        """journal=on after journal=off starts journaling again from scratch."""
        scn = "jo_reenable"
        z = mk(scn, V090, load="before=3 after=4 journal=off")
        d = z.ircd
        d.say("#a", "bob", "L0 tim memory only")
        filler(d, "#a", L(1, 2))
        d.sync()
        check(scn, "nothing on disk while off", "highlightctx.journal" not in dirstate(z), repr(dirstate(z)))
        z.stop()
        z.load_lines = ["highlightctx journal=on"]
        z.write_config()
        z.start()
        d.say("#a", "carol", "R0 tim journaled now")
        filler(d, "#a", ["R1", "R2", "R3", "R4"])
        d.sync()
        j = z.journal_lines()
        c = z.client()
        ev = events_of(c)
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        z.stop(kill=True)
        z.start()
        d.say("#b", "x", "unrelated")
        d.sync()
        c = z.client()
        ev2 = events_of(c)
        c.quit(); finish(z)
        check(scn, "journal file created after re-enable", len(j) > 0 and j[0].startswith("B\t"), repr(j[:2]))
        check(scn, "Status reports enabled", any(b.startswith("journal: enabled") for b in st), repr(st))
        check(scn, "only the post-re-enable event exists", len(ev) == 1 and labels(ev[0]) == ["*R0", "R1", "R2", "R3", "R4"], repr([(e["raw_header"], labels(e)) for e in ev]))
        check(scn, "delivered event stays delivered across crash", ev2 == [], repr([e["raw_header"] for e in ev2]))

    def jo_load_arg_validation():
        """The full on/off vocabulary is accepted; anything else fails the load."""
        scn = "jo_load_arg_validation"
        OFF_WORDS = ("off", "0", "no", "false", "disable", "disabled")
        for val, ok_expected in (("off", True), ("on", True), ("0", True), ("1", True),
                                 ("yes", True), ("no", True), ("true", True), ("false", True),
                                 ("disabled", True), ("enabled", True),
                                 ("maybe", False), ("", False), ("2", False), ("auto", False)):
            name = "%s_%s" % (scn, val or "empty")
            # Read the ZNC log by path: a rejected load makes mk() raise before
            # it can return the object.
            logpath = os.path.join(ns["ROOT"], name, "znc.out")
            z = None
            st = []
            try:
                z = mk(name, V090, load="before=2 after=2 journal=%s" % val)
                c = z.client()
                st = [b for (t, b) in c.command("Status")]
                loaded = any(b.startswith("journal: ") for b in st)
                c.quit()
            except Exception:
                loaded = False
            finally:
                if z:
                    finish(z)
            try:
                out = open(logpath, errors="replace").read()
            except FileNotFoundError:
                out = ""
            if ok_expected:
                want = "disabled" if val in OFF_WORDS else "enabled"
                check(scn, "journal=%s accepted as %s" % (val, want),
                      loaded and any(b.startswith("journal: " + want) for b in st),
                      "loaded=%s st=%r" % (loaded, [b for b in st if b.startswith("journal")]))
            else:
                check(scn, "journal=%s rejected with a clear error" % (val or "<empty>"),
                      not loaded and ("Invalid value for journal" in out or "Invalid load arg syntax" in out),
                      "loaded=%s tail=%r" % (loaded, out[-200:]))

    def jo_clearpending_wording():
        scn = "jo_clearpending_wording"
        z = mk(scn, V090, load="before=2 after=9 journal=off")
        d = z.ircd
        d.say("#a", "bob", "L0 tim open")
        filler(d, "#a", L(1, 2))
        d.sync()
        c = z.client(server_time=True)
        # ClearPending after the attach-time replay: start a fresh open event first
        c.quit()
        d.say("#a", "bob", "K0 tim open again")
        filler(d, "#a", ["K1"])
        d.sync()
        c = z.client()
        out = [b for (t, b) in c.command("ClearPending")]
        st = [b for (t, b) in c.command("Status")]
        c.quit()
        state = dirstate(z)
        finish(z)
        check(scn, "ClearPending says nothing was written to disk",
              any("Journaling is disabled, so nothing was written to disk" in b for b in out), repr(out))
        check(scn, "state cleared", any(b == "open events: 0" for b in st) and any(b == "pending finalized events: 0" for b in st), repr(st))
        check(scn, "still no journal file", "highlightctx.journal" not in state, repr(state))

    def jo_max_events_and_excludes():
        """Other features keep working with journaling off."""
        scn = "jo_max_events_and_excludes"
        z = mk(scn, V090, load="before=1 after=2 max_events=2 journal=off excludes=#b,spammer")
        d = z.ircd
        d.say("#b", "bob", "B0 tim excluded channel")
        filler(d, "#b", ["B1", "B2"])
        d.say("#a", "spammer", "S0 tim excluded nick")
        filler(d, "#a", ["S1", "S2"])
        for k in range(3):
            d.say("#a", "bob", "E%d tim" % k)
            filler(d, "#a", ["E%d_1" % k, "E%d_2" % k])
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit()
        state = dirstate(z)
        finish(z)
        check(scn, "exclusions honored and max_events enforced",
              [e["chan"] for e in ev] == ["#a", "#a"] and len(ev) == 2, repr([(e["chan"], e["raw_header"]) for e in ev]))
        check(scn, "no journal file", "highlightctx.journal" not in state, repr(state))

    def jo_no_records_while_open():
        """With an event open, journal=on writes a record per line; off writes none.

        Wall-clock time is NOT compared: the fake IRC server feeds lines
        serially, so the harness dominates and any timing here would be
        meaningless."""
        scn = "jo_no_records_while_open"
        timings = {}
        for tag, args in (("on", "before=8 after=8"), ("off", "before=8 after=8 journal=off")):
            z = mk(scn + "_" + tag, V090, load=args)
            d = z.ircd
            d.say("#a", "bob", "T0 tim trigger")   # one open event -> every line journaled
            d.sync()
            for i in range(200):
                d.say("#a", "alice", "line %d" % i)
            d.sync()
            j = len(z.journal_lines())
            finish(z)
            timings[tag + "_journal_lines"] = j
        print("    info: 200 channel lines with one open event -> journal records written: on=%d off=%d" % (
            timings["on_journal_lines"], timings["off_journal_lines"]))
        check(scn, "journal=on wrote records, journal=off wrote none",
              timings["on_journal_lines"] > 0 and timings["off_journal_lines"] == 0, repr(timings))

    ns["JOURNAL_OFF"] = [jo_no_disk_writes, jo_same_replay_as_journal_on, jo_state_not_durable,
                         jo_setting_persists, jo_reset_does_not_reenable, jo_leftover_file, jo_reenable,
                         jo_load_arg_validation, jo_clearpending_wording, jo_max_events_and_excludes,
                         jo_no_records_while_open]
