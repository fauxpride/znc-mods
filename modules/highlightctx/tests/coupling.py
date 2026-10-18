"""ignore_drop coupling scenarios for highlightctx 0.14.0.

ZNC dispatches a network's channel messages to that network's modules in list
order, so ignore_drop protects highlightctx only while it sits ahead of it.
ZNC re-appends a module at the end of the list on every load, including
ReloadMod and UpdateMod, so reloading ignore_drop reverses a correct order.
Up to 0.13.0 the live gate only checked that ignore_drop was loaded: after
such a reload, armed auto mode kept capturing, ignored senders started
events, and replay delivered their lines. 0.14.0 checks the position live,
pauses capture when the requirement is in force but not met, counts the
highlights it misses, and reports them at the next replay.

Also covered: require_ignore_drop=on no longer aborts ZNC startup when
znc.conf lists highlightctx first; the armed state persists across a restart,
so a reversed order saved to znc.conf cannot silently disarm; and an
ignore_drop loaded at user scope counts as ahead.

Imported by suite.py; every function follows the suite's check() convention.
Needs IGNORE_DROP. idc_user_scope_ignore_drop also needs IGNORE_DROP_OLD, an
ignore_drop build that still accepts user scope (1.1.0 or earlier).
"""
import os
import re

from hx import Znc


def install(ns):
    check = ns["check"]
    filler = ns["filler"]
    labels = ns["labels"]
    finish = ns["finish"]
    events_of = ns["events_of"]
    open_zncs = ns["open_zncs"]
    V090 = ns["V090"]          # build under test
    V080 = ns["V080"]          # baseline
    ZNC_BIN = ns["ZNC_BIN"]
    ROOT = ns["ROOT"]
    EXTRA_ENV = ns["EXTRA_ENV"]
    IGNORE_DROP = ns["IGNORE_DROP"]
    IGNORE_DROP_OLD = os.environ.get("IGNORE_DROP_OLD", "")
    baseline_version = ns["baseline_version"]

    def old_baseline():
        """True for a baseline older than 0.14.0, which has the bugs these
        scenarios guard against. With a 0.14.0 or later baseline, the
        contrast checks become no-regression checks instead."""
        try:
            return tuple(int(x) for x in baseline_version().split(".")) < (0, 14, 0)
        except ValueError:
            return True

    PAUSE_NOTE = "highlight(s) arrived while capture was paused and were not captured"

    class UserLevelZnc(Znc):
        """Adds user-level LoadModule lines to the generated znc.conf."""
        user_lines = []

        def write_config(self):
            super().write_config()
            p = os.path.join(self.workdir, "configs", "znc.conf")
            cfg = open(p).read()
            extra = "".join("    LoadModule = %s\n" % l for l in self.user_lines)
            cfg = cfg.replace("    MaxNetworks = 5\n", "    MaxNetworks = 5\n" + extra)
            open(p, "w").write(cfg)

    def mkz(name, so, lines, idrop=None, user_lines=(), start=True):
        """ZNC with highlightctx (`so`) and ignore_drop installed, network
        LoadModule lines in the given order."""
        mods = {"highlightctx": so, "ignore_drop": idrop or IGNORE_DROP}
        cls = Znc
        if user_lines:
            cls = type("UZ", (UserLevelZnc,), {"user_lines": list(user_lines)})
        z = cls(os.path.join(ROOT, name), ZNC_BIN, mods, list(lines), extra_env=EXTRA_ENV)
        open_zncs.append(z)
        if start:
            z.start()
        return z

    def status(c):
        return [b for (t, b) in c.command("Status")]

    def field(st, prefix):
        for x in st:
            if x.startswith(prefix):
                return x[len(prefix):]
        return None

    def paused_note(st):
        return any(x.startswith("note: capture is PAUSED") for x in st)

    def mallory_and_bob(d):
        """Traffic with one highlight from an ignored sender and one real one."""
        filler(d, "#a", ["P1", "P2"])
        d.say("#a", "mallory", "M1 tim ignored spam")
        filler(d, "#a", ["F1"])
        d.say("#a", "bob", "B1 tim real")
        filler(d, "#a", ["F2", "F3", "F4"])
        d.sync()

    def summary(c):
        """(events as label lists, pause notes) from an attach replay."""
        evs = events_of(c)
        notes = [b for (t, b) in c.attach_lines if PAUSE_NOTE in b]
        return [labels(e) for e in evs], notes

    def has(evs, lab):
        return any(any(l.lstrip("*") == lab for l in e) for e in evs)

    # ------------------------------------------------------------------

    def reload_case(scn, status_cmd):
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mkz("%s_%s" % (scn, tag), so, ["ignore_drop", "highlightctx"])
            c = z.client()
            before = status(c)
            c.status_cmd("*ignore_drop", "Add mallory always")
            reply = [b for (t, b) in c.status_cmd("*status", status_cmd)]
            after = status(c)
            c.quit()
            mallory_and_bob(z.ircd)
            c = z.client()
            evs, notes = summary(c)
            after_replay = status(c)
            c.quit(); finish(z)
            res[tag] = dict(before=before, reply=reply, after=after, evs=evs, notes=notes, after_replay=after_replay)
        b, n = res["baseline"], res["under_test"]
        if old_baseline():
            check(scn, "baseline reproduces the bug: ignored sender captured and replayed", has(b["evs"], "M1"), repr(b["evs"]))
        else:
            check(scn, "baseline %s pauses the same way" % baseline_version(),
                  (b["evs"], b["notes"]) == (n["evs"], n["notes"]), repr((b["evs"], b["notes"])))
        check(scn, "armed and ahead before the reload",
              field(n["before"], "auto mode armed: ") == "yes" and field(n["before"], "ignore_drop ahead of highlightctx in hook order: ") == "yes",
              repr(n["before"]))
        check(scn, "after the reload Status shows the live position and the pause",
              field(n["after"], "ignore_drop ahead of highlightctx in hook order: ") == "no"
              and field(n["after"], "auto mode armed: ") == "yes" and paused_note(n["after"])
              and field(n["after"], "Detached-only capture active now: ") == "no", repr(n["after"]))
        check(scn, "nothing captured while paused", n["evs"] == [], repr(n["evs"]))
        check(scn, "replay reports both missed highlights", len(n["notes"]) == 1 and n["notes"][0].startswith("note: 2 " + PAUSE_NOTE),
              repr(n["notes"]))
        check(scn, "counter cleared by the replay",
              field(n["after_replay"], "highlights missed while capture was paused for ignore_drop, since last replay: ") == "0",
              repr(n["after_replay"]))

    def idc_reloadmod_pauses():
        """ReloadMod of ignore_drop moves it behind highlightctx. The baseline
        then captures and replays the ignored sender; 0.14.0 pauses."""
        reload_case("idc_reloadmod_pauses", "ReloadMod --type=network ignore_drop")

    def idc_updatemod_pauses():
        """The same through UpdateMod, which is how a new ignore_drop build is
        deployed in place."""
        reload_case("idc_updatemod_pauses", "UpdateMod ignore_drop")

    def idc_restore_with_updatemod():
        """After the reversal, UpdateMod highlightctx re-appends highlightctx
        behind ignore_drop: protected capture resumes with no Rearm."""
        scn = "idc_restore_with_updatemod"
        z = mkz(scn, V090, ["ignore_drop", "highlightctx"])
        c = z.client()
        c.status_cmd("*ignore_drop", "Add mallory always")
        c.status_cmd("*status", "UpdateMod ignore_drop")
        paused = status(c)
        c.status_cmd("*status", "UpdateMod highlightctx")
        c.quit()
        c = z.client()   # reconnect: the old client's *highlightctx query was reset
        st = status(c)
        c.quit()
        mallory_and_bob(z.ircd)
        c = z.client()
        evs, notes = summary(c)
        c.quit(); finish(z)
        check(scn, "paused after UpdateMod ignore_drop", paused_note(paused), repr(paused))
        check(scn, "ahead, armed and capturing after UpdateMod highlightctx",
              field(st, "ignore_drop ahead of highlightctx in hook order: ") == "yes"
              and field(st, "auto mode armed: ") == "yes" and not paused_note(st), repr(st))
        check(scn, "bob captured, mallory dropped, no pause note",
              len(evs) == 1 and has(evs, "B1") and not has(evs, "M1") and notes == [], repr((evs, notes)))

    def idc_rearm_gives_up_protection():
        """Rearm after the reversal disarms auto mode, which explicitly gives
        up the protection: capture resumes and the ignored sender is captured.
        The warning says so."""
        scn = "idc_rearm_gives_up_protection"
        z = mkz(scn, V090, ["ignore_drop", "highlightctx"])
        c = z.client()
        c.status_cmd("*ignore_drop", "Add mallory always")
        c.status_cmd("*status", "ReloadMod --type=network ignore_drop")
        out = [b for (t, b) in c.command("Rearm")]
        st = status(c)
        c.quit()
        mallory_and_bob(z.ircd)
        c = z.client()
        evs, notes = summary(c)
        c.quit(); finish(z)
        check(scn, "Rearm warns that capture is now unprotected",
              any(x.startswith("Warning: auto mode transitioned from armed to unarmed") and "without ignore_drop protection" in x for x in out),
              repr(out))
        check(scn, "Rearm points at UpdateMod highlightctx", any("UpdateMod highlightctx" in x for x in out), repr(out))
        check(scn, "unarmed, not paused", field(st, "auto mode armed: ") == "no" and not paused_note(st), repr(st))
        check(scn, "capture unprotected as documented", has(evs, "M1") and has(evs, "B1") and notes == [], repr((evs, notes)))

    def idc_mode_change_rechecks():
        """SetRequireIgnoreDrop and Reset re-check from scratch, so they can
        disarm a sticky armed state, unlike OnLoad and OnBoot."""
        scn = "idc_mode_change_rechecks"
        z = mkz(scn, V090, ["ignore_drop", "highlightctx"])
        c = z.client()
        c.status_cmd("*status", "UpdateMod ignore_drop")
        st0 = status(c)
        r1 = [b for (t, b) in c.command("SetRequireIgnoreDrop auto")]
        st1 = status(c)
        c.status_cmd("*status", "UpdateMod highlightctx")
        c.quit()
        c = z.client()
        st2 = status(c)
        c.status_cmd("*status", "UpdateMod ignore_drop")
        c.command("Reset")
        st3 = status(c)
        c.quit(); finish(z)
        check(scn, "sticky armed before", field(st0, "auto mode armed: ") == "yes" and paused_note(st0), repr(st0))
        check(scn, "SetRequireIgnoreDrop auto disarms", field(st1, "auto mode armed: ") == "no" and not paused_note(st1)
              and r1[:1] == ["require_ignore_drop mode set to auto (armed=no) | note: ignore_drop is loaded now but auto mode is unarmed because ignore_drop is positioned at or after highlightctx in the module list; try Rearm to re-check, or unload and reload highlightctx to move it after ignore_drop."],
              repr((r1, st1)))
        check(scn, "reload of highlightctx arms again", field(st2, "auto mode armed: ") == "yes" and not paused_note(st2), repr(st2))
        check(scn, "Reset disarms", field(st3, "auto mode armed: ") == "no" and not paused_note(st3), repr(st3))

    def idc_on_mode_conf_order():
        """require_ignore_drop=on with highlightctx listed before ignore_drop
        in znc.conf. Up to 0.13.0 the module refused to load, which ZNC treats
        as fatal: startup aborted with "Unrecoverable config error". 0.14.0
        loads, pauses, and resumes once the order is fixed."""
        scn = "idc_on_mode_conf_order"
        lines = ["highlightctx require_ignore_drop=on", "ignore_drop"]
        zb = mkz(scn + "_baseline", V080, lines, start=False)
        try:
            zb.start()
            base_started = True
        except RuntimeError:
            base_started = False
        base_exit = zb.proc.wait(timeout=15) if not base_started else None
        base_out = zb.output()
        finish(zb)
        if old_baseline():
            check(scn, "baseline: ZNC refuses to start",
                  not base_started and base_exit not in (None, 0) and "Unrecoverable config error" in base_out, repr((base_started, base_exit)))
        else:
            check(scn, "baseline %s starts" % baseline_version(), base_started, repr((base_started, base_exit)))

        z = mkz(scn + "_under_test", V090, lines)
        load_line = [l for l in z.output().splitlines() if "Loaded highlightctx" in l]
        c = z.client()
        c.status_cmd("*ignore_drop", "Add mallory always")
        st = status(c)
        c.quit()
        mallory_and_bob(z.ircd)
        c = z.client()
        evs1, notes1 = summary(c)
        c.status_cmd("*status", "UpdateMod highlightctx")
        c.quit()
        c = z.client()
        st2 = status(c)
        c.quit()
        mallory_and_bob(z.ircd)
        c = z.client()
        evs2, notes2 = summary(c)
        c.quit(); finish(z)
        check(scn, "starts, and the load message notes the pause",
              load_line and "note: capture is paused until ignore_drop is loaded ahead of highlightctx (ignore_drop is currently not loaded)" in load_line[0],
              repr(load_line))
        check(scn, "paused with ignore_drop behind",
              field(st, "require_ignore_drop mode: ") == "on" and field(st, "ignore_drop ahead of highlightctx in hook order: ") == "no"
              and paused_note(st), repr(st))
        check(scn, "nothing captured, both highlights reported", evs1 == [] and len(notes1) == 1 and notes1[0].startswith("note: 2 "),
              repr((evs1, notes1)))
        check(scn, "after UpdateMod highlightctx: protected capture",
              not paused_note(st2) and len(evs2) == 1 and has(evs2, "B1") and not has(evs2, "M1") and notes2 == [],
              repr((st2, evs2, notes2)))

    def idc_on_mode_without_ignore_drop():
        """SetRequireIgnoreDrop on is accepted with ignore_drop absent (it was
        refused before) and pauses capture. A runtime LoadMod of ignore_drop
        appends it behind highlightctx, so capture stays paused until
        highlightctx is reloaded."""
        scn = "idc_on_mode_without_ignore_drop"
        z = mkz(scn, V090, ["highlightctx"])
        c = z.client()
        r = [b for (t, b) in c.command("SetRequireIgnoreDrop on") if not b.startswith("Version: highlightctx")]
        st0 = status(c)
        c.quit()
        d = z.ircd
        d.say("#a", "bob", "B1 tim one")
        d.sync()
        c = z.client()
        _, notes0 = summary(c)
        c.status_cmd("*status", "LoadMod --type=network ignore_drop")
        st1 = status(c)
        c.status_cmd("*status", "UpdateMod highlightctx")
        c.quit()
        c = z.client()
        st2 = status(c)
        c.quit(); finish(z)
        check(scn, "accepted, reply notes the pause",
              r == ["require_ignore_drop mode set to on | note: capture is paused until ignore_drop is loaded ahead of highlightctx (ignore_drop is currently not loaded); see Status"],
              repr(r))
        check(scn, "Status: not loaded, paused", field(st0, "ignore_drop currently loaded: ") == "no" and paused_note(st0)
              and any("Load it with LoadMod --type=network ignore_drop" in x for x in st0), repr(st0))
        check(scn, "missed highlight reported", len(notes0) == 1 and notes0[0].startswith("note: 1 "), repr(notes0))
        check(scn, "runtime LoadMod lands behind: still paused",
              field(st1, "ignore_drop currently loaded: ") == "yes" and field(st1, "ignore_drop ahead of highlightctx in hook order: ") == "no"
              and paused_note(st1), repr(st1))
        check(scn, "after UpdateMod highlightctx: capturing", not paused_note(st2)
              and field(st2, "ignore_drop ahead of highlightctx in hook order: ") == "yes", repr(st2))

    def idc_sticky_across_restart():
        """After UpdateMod ignore_drop, any config write saves highlightctx
        first. On the next boot the baseline comes up unarmed and captures the
        ignored sender. 0.14.0 restores the armed state from NV and pauses."""
        scn = "idc_sticky_across_restart"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mkz("%s_%s" % (scn, tag), so, ["ignore_drop", "highlightctx"])
            c = z.client()
            c.status_cmd("*ignore_drop", "Add mallory always")
            c.status_cmd("*status", "UpdateMod ignore_drop")
            c.status_cmd("*status", "SaveConfig")
            c.quit()
            cfg = open(os.path.join(z.workdir, "configs", "znc.conf")).read()
            order = [l.strip() for l in cfg.splitlines() if l.strip().startswith("LoadModule")]
            z.stop()
            z.start()
            c = z.client()
            st = status(c)
            c.quit()
            mallory_and_bob(z.ircd)
            c = z.client()
            evs, notes = summary(c)
            c.quit(); finish(z)
            res[tag] = dict(order=order, st=st, evs=evs, notes=notes)
        b, n = res["baseline"], res["under_test"]
        check(scn, "config written with highlightctx first",
              n["order"] == ["LoadModule = highlightctx", "LoadModule = ignore_drop"], repr(n["order"]))
        if old_baseline():
            check(scn, "baseline boots unarmed and captures the ignored sender",
                  field(b["st"], "auto mode armed: ") == "no" and has(b["evs"], "M1"), repr(b))
        else:
            check(scn, "baseline %s boots armed and paused as well" % baseline_version(),
                  (b["evs"], b["notes"]) == (n["evs"], n["notes"]) and paused_note(b["st"]), repr(b))
        check(scn, "0.14.0 boots armed and paused", field(n["st"], "auto mode armed: ") == "yes" and paused_note(n["st"]), repr(n["st"]))
        check(scn, "0.14.0 captures nothing and reports 2 missed",
              n["evs"] == [] and len(n["notes"]) == 1 and n["notes"][0].startswith("note: 2 "), repr((n["evs"], n["notes"])))

    def idc_unload_pauses_and_reports():
        """Unloading ignore_drop while armed paused capture before 0.14.0 as
        well; the difference is that the missed highlights are now reported."""
        scn = "idc_unload_pauses_and_reports"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mkz("%s_%s" % (scn, tag), so, ["ignore_drop", "highlightctx"])
            c = z.client()
            c.status_cmd("*status", "UnloadMod ignore_drop")
            st = status(c)
            c.quit()
            mallory_and_bob(z.ircd)
            c = z.client()
            evs, notes = summary(c)
            c.quit(); finish(z)
            res[tag] = dict(st=st, evs=evs, notes=notes)
        b, n = res["baseline"], res["under_test"]
        check(scn, "both pause", b["evs"] == [] and n["evs"] == [], repr((b["evs"], n["evs"])))
        check(scn, "build under test reports the missed highlights", len(n["notes"]) == 1
              and n["notes"][0].startswith("note: 2 ") and "ignore_drop is currently not loaded" in n["notes"][0], repr(n["notes"]))
        if old_baseline():
            check(scn, "baseline does not report them", b["notes"] == [], repr(b["notes"]))
        else:
            check(scn, "baseline %s reports them the same way" % baseline_version(), b["notes"] == n["notes"], repr(b["notes"]))
        check(scn, "Status names the cause", paused_note(n["st"]) and any("ignore_drop is not loaded" in x for x in n["st"]), repr(n["st"]))

    def idc_missed_counting():
        """Only lines that would have been triggers are counted: not self
        lines, excluded senders, excluded channels, or ordinary chatter. The
        count persists across SIGKILL and is cleared by the replay."""
        scn = "idc_missed_counting"
        z = mkz(scn, V090, ["highlightctx"])
        c = z.client()
        c.command("AddExclude BadNick")
        c.command("AddExclude #b")
        c.command("SetRequireIgnoreDrop on")
        c.quit()
        d = z.ircd
        d.say("#a", "tim", "S1 tim: talking to myself")
        d.say("#a", "BadNick", "X1 tim excluded sender")
        d.say("#b", "bob", "X2 tim excluded channel")
        d.say("#a", "bob", "C1 no mention")
        d.say("#a", "bob", "T1 tim text", kind="T")
        d.say("#a", "bob", "N1 tim notice", kind="N")
        d.say("#a", "bob", "A1 tim action", kind="A")
        d.say("#a", "carol", "D1 tim- decorated")
        d.sync()
        z.stop(kill=True)
        z.start()
        c = z.client()
        evs, notes = summary(c)
        st = status(c)
        c.quit(); finish(z)
        check(scn, "4 counted (text, notice, action, decorated), survived SIGKILL",
              evs == [] and len(notes) == 1 and notes[0].startswith("note: 4 "), repr((evs, notes)))
        check(scn, "cleared after replay",
              field(st, "highlights missed while capture was paused for ignore_drop, since last replay: ") == "0", repr(st))

    def idc_user_scope_ignore_drop():
        """An ignore_drop loaded at user scope (possible up to ignore_drop
        1.1.0) runs before every network module. The baseline reports it as
        not loaded, and with require_ignore_drop=on aborts ZNC startup.
        0.14.0 counts it as ahead."""
        scn = "idc_user_scope_ignore_drop"
        if not IGNORE_DROP_OLD:
            check(scn, "IGNORE_DROP_OLD provided", False, "set IGNORE_DROP_OLD to an ignore_drop 1.1.0 build")
            return
        res = {}
        for mode in ("auto", "on"):
            for tag, so in (("baseline", V080), ("under_test", V090)):
                z = mkz("%s_%s_%s" % (scn, mode, tag), so, ["highlightctx require_ignore_drop=" + mode],
                        idrop=IGNORE_DROP_OLD, user_lines=["ignore_drop"], start=False)
                try:
                    z.start()
                except RuntimeError:
                    res[(mode, tag)] = dict(started=False)
                    finish(z)
                    continue
                c = z.client()
                c.status_cmd("*ignore_drop", "Add mallory always")
                st = status(c)
                c.quit()
                mallory_and_bob(z.ircd)
                c = z.client()
                evs, notes = summary(c)
                c.quit(); finish(z)
                res[(mode, tag)] = dict(started=True, st=st, evs=evs, notes=notes)
        b, n = res[("auto", "baseline")], res[("auto", "under_test")]
        if old_baseline():
            check(scn, "auto, baseline: reported as not loaded, unarmed", b["started"] and field(b["st"], "ignore_drop currently loaded: ") == "no"
                  and field(b["st"], "auto mode armed: ") == "no", repr(b))
        else:
            check(scn, "auto, baseline %s: recognised the same way" % baseline_version(), b["started"]
                  and field(b["st"], "ignore_drop currently loaded: ") == "yes (user scope)"
                  and field(b["st"], "auto mode armed: ") == "yes", repr(b))
        check(scn, "auto, 0.14.0: user-scope instance counted as ahead, armed",
              field(n["st"], "ignore_drop currently loaded: ") == "yes (user scope)"
              and field(n["st"], "ignore_drop ahead of highlightctx in hook order: ") == "yes (user-scope instance)"
              and field(n["st"], "auto mode armed: ") == "yes" and not paused_note(n["st"]), repr(n["st"]))
        check(scn, "auto, both: mallory dropped by the user-scope instance, bob captured",
              all(has(r["evs"], "B1") and not has(r["evs"], "M1") for r in (b, n)), repr((b["evs"], n["evs"])))
        bo, no = res[("on", "baseline")], res[("on", "under_test")]
        if old_baseline():
            check(scn, "on, baseline: ZNC does not start", not bo["started"], repr(bo))
        else:
            check(scn, "on, baseline %s: starts and captures with protection" % baseline_version(),
                  bo["started"] and has(bo["evs"], "B1") and not has(bo["evs"], "M1"), repr(bo))
        check(scn, "on, 0.14.0: starts, not paused, protected capture",
              no["started"] and not paused_note(no["st"]) and has(no["evs"], "B1") and not has(no["evs"], "M1") and no["notes"] == [],
              repr(no))

    def idc_version_marker():
        """The module reports 0.14.0 or later, the release that introduced
        this behaviour. (num_version_marker pins the current release.)"""
        scn = "idc_version_marker"
        z = mkz(scn, V090, ["highlightctx"])
        c = z.client()
        out = [b for (t, b) in c.command("Version")]
        c.quit(); finish(z)
        vers = [tuple(int(x) for x in m.group(1).split(".")) for b in out
                for m in [re.search(r"highlightctx (\d+\.\d+\.\d+)", b)] if m]
        check(scn, "Version reports 0.14.0 or later", vers and vers[0] >= (0, 14, 0), repr(out))

    ns["COUPLING"] = [idc_reloadmod_pauses, idc_updatemod_pauses, idc_restore_with_updatemod,
                      idc_rearm_gives_up_protection, idc_mode_change_rechecks, idc_on_mode_conf_order,
                      idc_on_mode_without_ignore_drop, idc_sticky_across_restart, idc_unload_pauses_and_reports,
                      idc_missed_counting, idc_user_scope_ignore_drop, idc_version_marker]
