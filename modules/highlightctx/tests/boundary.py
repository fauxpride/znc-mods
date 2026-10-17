"""Nick-boundary scenarios for highlightctx 0.13.0.

Up to 0.12.0 a mention was rejected whenever a nick character touched the
nick on either side. The nick-character set includes - _ [ ] \\ ` ^ { } |,
so ordinary punctuation-like decoration such as "tim- hi", "tim_" or "[tim]"
was treated as a longer, different nick and never triggered.

0.13.0 looks at the whole run of nick characters touching the nick:
  - a run containing a letter or digit means the nick is embedded in another
    word or nick (timothy, xtim, tim_2, tim-tam, tim|away): no match;
  - a symbol-only run is decoration: match, unless the decorated token is the
    nick of someone else currently in that channel.

0.13.0 also matches against the text with IRC formatting removed. Before, a
coloured mention such as \x0312tim\x03 was missed, because the colour
number's digits touch the nick and read as part of a word.

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
    V080 = ns["V080"]          # baseline, for the differential checks

    def triggers_of(ev):
        """Labels of every marked (trigger) line, in replay order."""
        out = []
        for e in ev:
            for lab in labels(e):
                if lab.startswith("*"):
                    out.append(lab[1:])
        return out

    def per_channel(so, scn, probes, nick="tim", setup=None):
        """Send each probe into its own channel so each is judged alone.

        Returns the set of probe labels whose channel produced an event.
        `setup(ircd, chans)` may inject JOIN/PART/NICK lines first.
        """
        chans = tuple("#p%d" % i for i in range(len(probes)))
        z = mk(scn, so, load="before=0 after=0", chans=chans, nick=nick)
        d = z.ircd
        if setup:
            setup(d, chans)
        for ch, (lab, text) in zip(chans, probes):
            d.say(ch, "bob", "%s %s" % (lab, text))
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        hit = {e["chan"] for e in ev}
        return {lab for ch, (lab, _) in zip(chans, probes) if ch in hit}

    def nb_reported_line():
        """The reported line triggers on 0.13.0 and was missed by the baseline.

        Uses the nick from the bug report, in the exact shape reported: nick,
        a dash, a space, one more word.
        """
        scn = "nb_reported_line"
        res = {}
        for tag, so in (("baseline", V080), ("under_test", V090)):
            z = mk(scn + "_" + tag, so, load="before=2 after=2", nick="fauxpride")
            d = z.ircd
            filler(d, "#a", ["P1", "P2"])
            d.say("#a", "bob", "fauxpride- i")
            filler(d, "#a", L(1, 2))
            d.sync()
            c = z.client()
            res[tag] = events_of(c)
            c.quit(); finish(z)
        ut = res["under_test"]
        check(scn, "0.13.0 captures the reported line", len(ut) == 1, repr([e["raw_header"] for e in ut]))
        check(scn, "trigger and context are intact",
              ut and [("*" if l["marked"] else "") + l["text"] for l in ut[0]["lines"]] ==
              ["<alice> P1 chatter", "<alice> P2 chatter", "*<bob> fauxpride- i",
               "<alice> L1 chatter", "<alice> L2 chatter"],
              repr(ut[0]["lines"]) if ut else "")
        print("    info: reported line -> baseline events=%d, under test events=%d"
              % (len(res["baseline"]), len(ut)))

    DECORATED = [
        ("D01", "tim- hi"),
        ("D02", "tim-"),
        ("D03", "tim-- ok"),
        ("D04", "tim_ ping"),
        ("D05", "tim__"),
        ("D06", "tim^^ hey"),
        ("D07", "tim` there"),
        ("D08", "tim| you around"),
        ("D09", "tim\\ odd"),
        ("D10", "[tim] bracketed"),
        ("D11", "{tim} braced"),
        ("D12", "_tim_ underscores"),
        ("D13", "-tim- dashes"),
        ("D14", "^tim hat"),
        ("D15", "hey tim-, there"),
        ("D16", "end with tim-"),
        ("D17", "TIM- uppercase"),
        ("D18", "\x02tim-\x02 bold"),
        ("D19", "\x0304tim_\x03 coloured"),
        ("D20", "tim-ș non-ascii after"),
    ]
    EMBEDDED = [
        ("E01", "timothy"),
        ("E02", "xtim"),
        ("E03", "tim2"),
        ("E04", "tim_2"),
        ("E05", "tim-tam"),
        ("E06", "tim|away"),
        ("E07", "tim^x"),
        ("E08", "[tim]x"),
        ("E09", "x[tim]"),
        ("E10", "bob_tim"),
        ("E11", "tim_-_x"),
        ("E12", "tim-tim"),
    ]
    PLAIN = [
        ("C01", "tim: plain"),
        ("C02", "tim, plain"),
        ("C03", "tim plain"),
        ("C04", "tim's plain"),
        ("C05", "<tim> plain"),
        ("C06", "@tim plain"),
        ("C07", "ping tim"),
    ]

    def nb_decorated_mentions_match():
        """Symbol-only decoration on either side counts as a mention.

        Every probe here is missed by the baseline (asserted, so the test
        discriminates) and caught by 0.13.0.
        """
        scn = "nb_decorated_mentions_match"
        want = {lab for lab, _ in DECORATED}
        base = per_channel(V080, scn + "_baseline", DECORATED)
        got = per_channel(V090, scn + "_under_test", DECORATED)
        check(scn, "all decorated forms trigger", got == want, "missing=%r" % sorted(want - got))
        check(scn, "baseline missed all of them", base == set(), repr(sorted(base)))

    def nb_embedded_still_rejected():
        """A letter or digit in the touching run still means a different word."""
        scn = "nb_embedded_still_rejected"
        got = per_channel(V090, scn, EMBEDDED)
        check(scn, "no embedded form triggers", got == set(), repr(sorted(got)))

    def nb_plain_mentions_unchanged():
        """Undecorated mentions behave identically to the baseline."""
        scn = "nb_plain_mentions_unchanged"
        want = {lab for lab, _ in PLAIN}
        base = per_channel(V080, scn + "_baseline", PLAIN)
        got = per_channel(V090, scn + "_under_test", PLAIN)
        check(scn, "all plain forms trigger on 0.13.0", got == want, repr(sorted(want - got)))
        check(scn, "same result as baseline", got == base, "base=%r new=%r" % (sorted(base), sorted(got)))

    FORMATTED = [
        ("F1", "\x0312tim\x03: coloured plain"),
        ("F2", "\x0304,12tim\x03 foreground and background"),
        ("F3", "\x034tim\x03 one-digit colour"),
        ("F4", "\x04FF0000tim\x04 hex colour"),
        ("F5", "\x04ff0000,00ff00tim\x04 hex foreground and background"),
        ("F6", "\x0304,tim literal comma after colour"),
        ("F7", "\x1dtim\x1d italic, \x1ftim\x1f underline, \x16tim\x16 reverse"),
        ("F8", "\x0312tim-\x03 coloured and decorated"),
    ]
    FORMATTED_NOT = [
        ("G1", "\x0312tim\x03othy colour split inside a word"),
        ("G2", "x\x02tim\x02 bold split inside a word"),
        ("G3", "\x03,05tim no foreground, so ,05 is literal text"),
    ]

    def nb_formatting_is_ignored():
        """Colour and formatting codes neither hide nor fake a mention.

        The baseline misses every probe where a colour number's digits touch
        the nick (asserted for F1-F5 and F8, the cases that discriminate);
        0.13.0 catches all of FORMATTED. FORMATTED_NOT probes display as a
        longer word and must not trigger.
        """
        scn = "nb_formatting_is_ignored"
        want = {lab for lab, _ in FORMATTED}
        base = per_channel(V080, scn + "_baseline", FORMATTED)
        got = per_channel(V090, scn + "_under_test", FORMATTED)
        check(scn, "all formatted mentions trigger", got == want, "missing=%r" % sorted(want - got))
        check(scn, "baseline missed the colour-digit cases",
              not (base & {"F1", "F2", "F3", "F4", "F5", "F8"}), repr(sorted(base)))
        neg = per_channel(V090, scn + "_negative", FORMATTED_NOT)
        check(scn, "formatting inside a word does not trigger", neg == set(), repr(sorted(neg)))

    def nb_other_nick_present():
        """A decorated token that is someone else's nick in the channel is theirs.

        tim_, [tim] and TIM^ join every probe channel. Mentions spelled as those
        nicks (including their RFC 1459 and ASCII case-equivalent spellings) are
        addressed to them and must not trigger; other decorations still do.
        """
        scn = "nb_other_nick_present"

        def setup(d, chans):
            for ch in chans:
                for who in ("tim_", "[tim]", "TIM^"):
                    d.raw(":%s!u@other.host JOIN %s" % (who, ch))

        probes = [
            ("O1", "tim_: hi"),
            ("O2", "TIM_ hi"),
            ("O3", "[tim] hi"),
            ("O4", "{tim} folded form of [tim]"),
            ("O5", "tim^ hi"),
            ("O6", "tim~ folded form of TIM^"),
            ("O7", "tim- not a present nick"),
            ("O8", "tim__ not a present nick"),
            ("O9", "tim_ first is theirs, tim- second is mine"),
        ]
        got = per_channel(V090, scn, probes, setup=setup)
        check(scn, "only decorations that are not present nicks trigger",
              got == {"O7", "O8", "O9"}, repr(sorted(got)))

    def nb_other_nick_tracks_membership():
        """The guard follows the live nicklist: PART, QUIT and NICK release it."""
        scn = "nb_other_nick_tracks_membership"
        z = mk(scn, V090, load="before=0 after=0", chans=("#a", "#b"))
        d = z.ircd
        d.raw(":tim_!u@other.host JOIN #a")
        d.raw(":tim^!u@other.host JOIN #a")
        d.raw(":tim`!u@other.host JOIN #a")
        d.say("#a", "bob", "M1 tim_ present, no trigger")
        d.say("#b", "bob", "M2 tim_ only present in #a, triggers in #b")
        d.raw(":tim_!u@other.host PART #a")
        d.say("#a", "bob", "M3 tim_ after part, triggers")
        d.raw(":tim^!u@other.host QUIT :bye")
        d.say("#a", "bob", "M4 tim^ after quit, triggers")
        d.raw(":tim`!u@other.host NICK :carol")
        d.say("#a", "bob", "M5 tim` after nick change, triggers")
        d.raw(":carol!u@other.host NICK :tim|")
        d.say("#a", "bob", "M6 tim| after nick change into it, no trigger")
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "membership changes are honoured",
              sorted(triggers_of(ev)) == ["M2", "M3", "M4", "M5"], repr(triggers_of(ev)))

    def nb_kinds_and_exclusions():
        """Decorated mentions work for notices and actions; exclusions still apply."""
        scn = "nb_kinds_and_exclusions"
        z = mk(scn, V090, load="before=0 after=0 excludes=spammer")
        d = z.ircd
        d.say("#a", "bob", "K1 tim- notice", kind="N")
        d.say("#b", "bob", "K2 waves at [tim]", kind="A")
        d.say("#a", "spammer", "K3 tim- from an excluded sender")
        d.say("#a", "tim", "K4 tim- from myself")
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "notice and action trigger; excluded and self do not",
              sorted(triggers_of(ev)) == ["K1", "K2"], repr(triggers_of(ev)))

    def nb_nick_with_symbols():
        """Own nicks that contain or end in symbols still match correctly."""
        scn = "nb_nick_with_symbols"
        res = {}
        for i, (nick, probes) in enumerate((
            ("ti-m", [("S1", "ti-m- hi"), ("S2", "ti-m"), ("S3", "ti-mx no")]),
            ("tim_", [("S4", "tim_ hi"), ("S5", "tim__ hi"), ("S6", "tim_x no"), ("S7", "tim no")]),
            ("ti[m]", [("S8", "ti{m}- folded"), ("S9", "ti[m]_ hi"), ("S10", "ti[m]2 no")]),
        )):
            res[nick] = per_channel(V090, "%s_%d" % (scn, i), probes, nick=nick)
        check(scn, "ti-m", res["ti-m"] == {"S1", "S2"}, repr(sorted(res["ti-m"])))
        check(scn, "tim_", res["tim_"] == {"S4", "S5"}, repr(sorted(res["tim_"])))
        check(scn, "ti[m]", res["ti[m]"] == {"S8", "S9"}, repr(sorted(res["ti[m]"])))

    def nb_alt_nick_after_rename():
        """Matching follows the nick actually in use after a server-side rename.

        Registers as tim, then the server renames us to tim- (as on a nick
        collision). Mentions of tim- in every form that matched before 0.13.0
        still match, decorated forms now match too, and the old nick no longer
        does. Repeated for tim_. Compared against the baseline so a regression
        on alternate nicks would show as a lost hit.
        """
        scn = "nb_alt_nick_after_rename"
        for i, alt in enumerate(("tim-", "tim_")):
            probes = [
                ("A1", "%s: hi" % alt), ("A2", "%s, hi" % alt), ("A3", "hey %s" % alt),
                ("A4", "%s's" % alt), ("A5", "%s- i" % alt), ("A6", "[%s] hi" % alt),
                ("A7", "%s2 no" % alt), ("A8", "tim: old nick"),
            ]

            def setup(d, chans, alt=alt):
                d.raw(":tim!tim@me.test NICK :%s" % alt)
                d.sync()

            base = per_channel(V080, "%s_%d_baseline" % (scn, i), probes, setup=setup)
            got = per_channel(V090, "%s_%d_under_test" % (scn, i), probes, setup=setup)
            check(scn, "%s: every baseline hit is kept" % alt, base <= got, "lost=%r" % sorted(base - got))
            check(scn, "%s: expected matches" % alt, got == {"A1", "A2", "A3", "A4", "A5", "A6"}, repr(sorted(got)))

    def nb_extension_with_decoration():
        """A decorated mention inside an open window extends it like any other."""
        scn = "nb_extension_with_decoration"
        z = mk(scn, V090, load="before=1 after=2")
        d = z.ircd
        filler(d, "#a", ["P1"])
        d.say("#a", "bob", "L0 tim: first")
        filler(d, "#a", ["L1"])
        d.say("#a", "carol", "L2 tim- second")
        filler(d, "#a", ["L3", "L4", "L5"])
        d.sync()
        c = z.client()
        ev = events_of(c)
        c.quit(); finish(z)
        check(scn, "one event, two triggers",
              len(ev) == 1 and ev[0]["triggers"] == 2 and labels(ev[0]) == ["P1", "*L0", "L1", "*L2", "L3", "L4"],
              repr([(e["raw_header"], labels(e)) for e in ev]))

    def nb_version_marker():
        """The module reports 0.13.0."""
        scn = "nb_version_marker"
        z = mk(scn, V090, load="before=1 after=1")
        c = z.client()
        out = [b for (t, b) in c.command("Version")]
        c.quit(); finish(z)
        check(scn, "Version reports 0.13.0", any("highlightctx 0.13.0" in b for b in out), repr(out))

    ns["BOUNDARY"] = [nb_reported_line, nb_decorated_mentions_match, nb_embedded_still_rejected,
                      nb_plain_mentions_unchanged, nb_formatting_is_ignored, nb_other_nick_present, nb_other_nick_tracks_membership,
                      nb_kinds_and_exclusions, nb_nick_with_symbols, nb_alt_nick_after_rename,
                      nb_extension_with_decoration,
                      nb_version_marker]
