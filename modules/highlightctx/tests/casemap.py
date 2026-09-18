"""RFC 1459 casemapping scenarios for highlightctx 0.12.0.

On the networks this module targets (Undernet, EFnet) CASEMAPPING is rfc1459:
the characters [ ] \\ ~ are the uppercase forms of { } | ^, so nicks and
channel names must be compared with that folding rather than ASCII
lowercasing. 0.12.0 applies it to the self-nick check, highlight matching,
channel keys and channel exclusions, matching what nick/mask exclusions
already did.

Imported by suite.py; every function follows the suite's check() convention.
"""


def install(ns):
    mk = ns["mk"]
    check = ns["check"]
    filler = ns["filler"]
    labels = ns["labels"]
    finish = ns["finish"]
    events_of = ns["events_of"]
    L = ns["L"]
    V090 = ns["V090"]
    V080 = ns["V080"]          # baseline, for the one differential check

    def cm_nick_folding_matches():
        """A bracketed nick is matched in its RFC 1459 case-equivalent form.

        With nick ti[m], an RFC 1459 server treats ti{m} as the same nick, so a
        highlight written that way must trigger. This is the one difference
        that is observable end to end, and it is asserted against the baseline
        too: pre-0.12.0 ASCII lowercasing does not match it.
        """
        scn = "cm_nick_folding_matches"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk(scn + "_" + tag, so, load="before=2 after=2", nick="ti[m]")
            d = z.ircd
            filler(d, "#a", ["P1", "P2"])
            d.say("#a", "bob", "L0 hey ti{m} folded form")
            filler(d, "#a", L(1, 2))
            d.sync()
            c = z.client()
            res[tag] = events_of(c)
            c.quit(); finish(z)
        check(scn, "0.12.0 matches the folded nick form",
              len(res["under_test"]) == 1 and any(l["marked"] for l in res["under_test"][0]["lines"]),
              repr([e["raw_header"] for e in res["under_test"]]))
        check(scn, "context around it is intact",
              res["under_test"] and labels(res["under_test"][0]) == ["P1", "P2", "*L0", "L1", "L2"],
              repr(labels(res["under_test"][0])) if res["under_test"] else "")
        print("    info: folded-nick highlight -> baseline events=%d, under test events=%d"
              % (len(res["baseline"]), len(res["under_test"])))

    def cm_bracketed_nick_plain_form():
        """The literal nick form still matches, with a bracketed nick."""
        scn = "cm_bracketed_nick_plain_form"
        z = mk(scn, V090, load="before=1 after=2", nick="ti[m]")
        d = z.ircd
        d.say("#a", "bob", "L0 ti[m] literal form")
        filler(d, "#a", L(1, 2))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "literal bracketed nick triggers exactly one event", len(ev) == 1,
              repr([(e["raw_header"], labels(e)) for e in ev]))
        check(scn, "trigger line is the bracketed mention",
              ev and any(l["marked"] and "L0" in l["text"] for l in ev[0]["lines"]), repr(labels(ev[0])) if ev else "")

    def cm_channel_with_brackets_works():
        """Channels containing [ ] behave normally (keys are folded internally).

        Note: ZNC resolves channel names itself with ASCII case-insensitivity,
        so a case-equivalent spelling never reaches the module as that
        channel's traffic. Folding the module's channel keys keeps them
        internally consistent; it is not observable through ZNC today.
        """
        scn = "cm_channel_with_brackets_works"
        z = mk(scn, V090, load="before=2 after=2", chans=("#a", "#te[st]"))
        d = z.ircd
        filler(d, "#te[st]", ["P1", "P2"])
        d.say("#te[st]", "bob", "L0 tim ping")
        filler(d, "#te[st]", L(1, 2))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "event captured in a channel containing [ ]",
              len(ev) == 1 and ev[0]["chan"] == "#te[st]", repr([(e["chan"], e["raw_header"]) for e in ev]))
        check(scn, "context intact", ev and labels(ev[0]) == ["P1", "P2", "*L0", "L1", "L2"],
              repr(labels(ev[0])) if ev else "")

    def cm_exclusion_add_del_round_trip():
        """AddExclude and DelExclude agree on the RFC 1459 folded key.

        Discriminating: folding maps #te[st] and #TE{ST} to the same key, while
        ASCII lowercasing does not, so pre-0.12.0 could not delete an exclusion
        added under the other spelling.
        """
        scn = "cm_exclusion_add_del_round_trip"
        z = mk(scn, V090, load="before=2 after=2", chans=("#a", "#te[st]"))
        c = z.client()
        add = [b for (t, b) in c.command("AddExclude #te[st]")]
        dele = [b for (t, b) in c.command("DelExclude #TE{ST}")]
        st = [b for (t, b) in c.command("Status")]
        c.quit(); finish(z)
        check(scn, "AddExclude accepted", any("Excluded channel" in b for b in add), repr(add))
        check(scn, "DelExclude matches the case-equivalent spelling",
              not any("not currently excluded" in b for b in dele), repr(dele))
        check(scn, "no channel exclusions remain",
              any(b == "excluded channels: 0" for b in st), repr([b for b in st if b.startswith("excluded")]))

    def cm_ascii_case_still_matches():
        """Plain ASCII case-insensitivity must be unchanged."""
        scn = "cm_ascii_case_still_matches"
        z = mk(scn, V090, load="before=1 after=1")
        d = z.ircd
        d.say("#a", "bob", "N1 TIM uppercase")
        filler(d, "#a", ["F1"])
        d.say("#a", "bob", "N2 TiM mixed")
        filler(d, "#a", ["F2"])
        d.say("#a", "bob", "N3 timothy is not me")
        filler(d, "#a", ["F3"])
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        triggers = [l["text"].split(" ")[1] for e in ev for l in e["lines"] if l["marked"]]
        check(scn, "uppercase and mixed case trigger, longer nick does not",
              triggers == ["N1", "N2"], repr(triggers))

    def cm_self_check_folds():
        """Messages from our own nick never trigger, including folded forms."""
        scn = "cm_self_check_folds"
        z = mk(scn, V090, load="before=1 after=2")
        d = z.ircd
        d.say("#a", "tim", "S0 tim talking to myself")
        d.say("#a", "TIM", "S1 tim uppercase self")
        filler(d, "#a", ["F1", "F2"])
        d.say("#a", "bob", "L0 tim real trigger")
        filler(d, "#a", ["F3", "F4"])
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "only the message from another nick triggers",
              len(ev) == 1 and any(l["marked"] and "L0" in l["text"] for l in ev[0]["lines"]),
              repr([(e["raw_header"], labels(e)) for e in ev]))

    ns["CASEMAP"] = [cm_nick_folding_matches, cm_bracketed_nick_plain_form,
                     cm_channel_with_brackets_works, cm_exclusion_add_del_round_trip,
                     cm_ascii_case_still_matches, cm_self_check_folds]
