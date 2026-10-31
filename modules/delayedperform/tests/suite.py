"""delayedperform test suite: runs a real ZNC against fake IRC servers.

Assertions are made on the exact lines ZNC sends to the server after it
connects (with arrival times relative to the server's 001), on the module's
replies and timer output as a client receives them, and on the NV store.
Connect-time output is observed by attaching a client first and then having
the fake server drop the connection, so the client is present when ZNC
reconnects. See ../TESTING.md.
"""
import base64
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hx import Network, Znc, modmsgs  # noqa: E402

MOD = "delayedperform"
ZNC_BIN = os.environ.get("ZNC_BIN", "/usr/bin/znc")
NEW = os.environ.get("NEW", "")        # delayedperform.so under test
PROBE = os.environ.get("PROBE", "")    # NEW rebuilt with a different version string
if not NEW:
    raise SystemExit("set NEW (the .so under test); see TESTING.md")
EXTRA_ENV = {}
if os.environ.get("ZNC_PRELOAD"):
    EXTRA_ENV["LD_PRELOAD"] = os.environ["ZNC_PRELOAD"]
    EXTRA_ENV["ASAN_OPTIONS"] = os.environ.get("ASAN_OPTIONS", "")
    EXTRA_ENV["UBSAN_OPTIONS"] = os.environ.get("UBSAN_OPTIONS", "")
ROOT = os.environ.get("HX_ROOT", "/tmp/delayedperform-tests")
ONLY = set(a for a in sys.argv[1:] if not a.startswith("-"))

TOL = 0.6
M = "*" + MOD

results = []
open_zncs = []


def check(scn, name, ok, detail=""):
    results.append((scn, name, bool(ok), detail))
    if not ok:
        print("    FAIL %s :: %s :: %s" % (scn, name, detail))


def b64(s):
    return base64.b64encode(s.encode("utf-8", "surrogateescape")).decode()


def entry(delay, cmd, flags=""):
    return "%s|%s|%s" % (delay, flags, b64(cmd))


def mk(name, so=NEW, entries=None, extra_nv=None, networks=None, load=True, start=True, **ircd_kw):
    """entries: [(delay, command, flags)] seeded as cmd.0.. before start."""
    if networks is None:
        networks = [Network("n1", chans=["#a"], load_lines=[MOD] if load else [], **ircd_kw)]
    nv = {}
    for i, e in enumerate(entries or []):
        nv["cmd.%d" % i] = entry(*e)
    nv.update(extra_nv or {})
    regs = {networks[0].name: {MOD: nv}} if nv else None
    z = Znc(os.path.join(ROOT, name), ZNC_BIN, {MOD: so}, networks, registries=regs, extra_env=EXTRA_ENV)
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
    m = re.search(rb"\(\+slash aliases\)\. v(\d+\.\d+\.\d+)", data)
    return m.group(1).decode() if m else None


NEW_VER = version_of(NEW)


def unique_syms(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return set(l.split()[-1] for l in out.splitlines() if len(l.split()) >= 3 and l.split()[1] == "u")


def pinned(so):
    return bool(unique_syms(so) - unique_syms(ZNC_BIN))


def store(z, net="n1"):
    return z.registry(net, MOD) or {}


# --------------------------------------------------------------------------
# Observation helpers
# --------------------------------------------------------------------------

# Lines ZNC sends on every connect regardless of the module.
ZNC_OWN = re.compile(r"^(CAP |NICK |USER |WHO tim|JOIN #a$|MODE #\S+$|PONG |PING )")


def sent_after_connect(d, since_welcome=-1):
    """[(seconds after that 001, line)] ZNC sent after the given 001 (the
    last one by default), leaving out ZNC's own connect traffic."""
    t0 = d.welcomes[since_welcome]
    with d.lock:
        rx = list(d.rx)
    return [(t - t0, l) for t, l in rx if t >= t0 and not ZNC_OWN.match(l)]


def reconnect(z, c, settle):
    """Drop ZNC's server connection with the client attached, wait for the
    reconnect, then collect `settle` seconds of client output. Returns the
    module's messages."""
    d = z.net().ircd
    n = len(d.welcomes)
    d.drop()
    end = time.time() + 15
    while len(d.welcomes) <= n and time.time() < end:
        time.sleep(0.02)
    if len(d.welcomes) <= n:
        raise RuntimeError("ZNC did not reconnect")
    got = c.drain(settle)
    return modmsgs(got, M)


# --------------------------------------------------------------------------
# fire_* - scheduling and sending
# --------------------------------------------------------------------------

def fire_order():
    """Entries added by command fire on the next connect, each once, at its
    own delay measured from 001; entries with equal delays fire together.
    The client sees Scheduled and one Ran: per entry, nothing else, and
    nothing repeats later."""
    scn = "fire_order"
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    for cmd in ("Add 0 JOIN #zero", "Add 3 PRIVMSG #a :three", "Add 1 /msg bob one", "Add 3 PRIVMSG #a :three-b"):
        c.cmd(M, cmd)
    msgs = reconnect(z, c, 4.5)
    msgs += modmsgs(c.drain(3), M)
    sent = sent_after_connect(d)
    finish(z)
    lines = [l for _, l in sent]
    want = {"JOIN #zero": 0, "PRIVMSG bob :one": 1, "PRIVMSG #a :three": 3, "PRIVMSG #a :three-b": 3}
    check(scn, "exactly the four commands, once each", sorted(lines) == sorted(want), repr(sent))
    for t, l in sent:
        if l in want:
            check(scn, "%s at %ds" % (l, want[l]), want[l] - 0.05 <= t <= want[l] + TOL, "%.2f" % t)
    check(scn, "client output", msgs == ["Scheduled 4 delayed command(s).", "Ran: JOIN #zero", "Ran: PRIVMSG bob :one",
                                          "Ran: PRIVMSG #a :three", "Ran: PRIVMSG #a :three-b"], repr(msgs))


def fire_global_delay():
    """SetDelay sets the delay for later Adds without one; entries keep the
    delay they were added with when SetDelay changes again."""
    scn = "fire_global_delay"
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    c.cmd(M, "SetDelay 2")
    c.cmd(M, "Add PRIVMSG #a :two")
    c.cmd(M, "SetDelay 4")
    c.cmd(M, "Add PRIVMSG #a :four")
    c.cmd(M, "SetDelay 0")
    reconnect(z, c, 5)
    sent = sent_after_connect(d)
    finish(z)
    got = {l: t for t, l in sent}
    check(scn, "two at 2s", "PRIVMSG #a :two" in got and 2 - 0.05 <= got["PRIVMSG #a :two"] <= 2 + TOL, repr(sent))
    check(scn, "four at 4s", "PRIVMSG #a :four" in got and 4 - 0.05 <= got["PRIVMSG #a :four"] <= 4 + TOL, repr(sent))


def fire_reconnect_cancels():
    """A drop before the timers fire cancels them; after the reconnect each
    entry fires exactly once, timed from the new 001."""
    scn = "fire_reconnect_cancels"
    z = mk(scn, entries=[(3, "PRIVMSG #a :late", ""), (2, "PRIVMSG #a :mid", "s")])
    d = z.net().ircd
    c = z.client()
    time.sleep(1)
    msgs = reconnect(z, c, 5)
    total = [l for _, l in sent_after_connect(d, since_welcome=0)]
    sent = sent_after_connect(d)
    finish(z)
    check(scn, "each fired once in total", sorted(total) == ["PRIVMSG #a :late", "PRIVMSG #a :mid"], repr(total))
    got = {l: t for t, l in sent}
    check(scn, "timed from the second 001", abs(got.get("PRIVMSG #a :late", -9) - 3) <= TOL and abs(got.get("PRIVMSG #a :mid", -9) - 2) <= TOL, repr(sent))
    check(scn, "client output", msgs == ["Scheduled 2 delayed command(s).", "Ran: [hidden]", "Ran: PRIVMSG #a :late"], repr(msgs))


def fire_unload_pending():
    """UnloadMod while timers are pending: nothing fires, ZNC keeps running
    (the sanitizer run checks the timer cleanup)."""
    scn = "fire_unload_pending"
    z = mk(scn, entries=[(2, "PRIVMSG #a :never", "")])
    d = z.net().ircd
    c = z.client()
    reconnect(z, c, 0.3)
    c.cmd("*status", "UnloadMod " + MOD)
    time.sleep(3)
    sent = sent_after_connect(d)
    alive = z.alive()
    c.quit(); finish(z)
    check(scn, "nothing sent", sent == [], repr(sent))
    check(scn, "ZNC alive", alive, "")


def fire_hot_load():
    """Characterised: LoadMod while connected schedules nothing (the module
    schedules only in OnIRCConnected); the entries run on the next connect."""
    scn = "fire_hot_load"
    z = mk(scn, load=False, extra_nv=None)
    z.write_registry("n1", MOD, {"cmd.0": entry(0, "PRIVMSG #a :hot")})
    d = z.net().ircd
    c = z.client()
    out = c.cmd("*status", "LoadMod " + MOD)
    time.sleep(2)
    before = sent_after_connect(d)
    msgs = reconnect(z, c, 1)
    after = sent_after_connect(d)
    finish(z)
    check(scn, "nothing on LoadMod", before == [] and not any("Scheduled" in x for x in out), repr((out, before)))
    check(scn, "runs on the next connect", [l for _, l in after] == ["PRIVMSG #a :hot"] and "Ran: PRIVMSG #a :hot" in msgs, repr((after, msgs)))


def fire_restart():
    """Entries added by command survive a ZNC restart and fire after it."""
    scn = "fire_restart"
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    c.cmd(M, "Add 1 PRIVMSG #a :after-restart")
    c.quit()
    z.stop()
    z.start()
    time.sleep(2)
    sent = sent_after_connect(d)
    finish(z)
    check(scn, "fired after restart", [l for _, l in sent] == ["PRIVMSG #a :after-restart"], repr(sent))


def fire_isolation():
    """Two networks keep separate entries and fire only their own."""
    scn = "fire_isolation"
    nets = [Network("n1", chans=["#a"], load_lines=[MOD]), Network("n2", chans=["#a"], load_lines=[MOD])]
    z = mk(scn, networks=nets)
    for net in ("n1", "n2"):
        c = z.client(net)
        c.cmd(M, "Add 0 PRIVMSG #a :from-%s" % net)
        c.quit()
    z.stop()
    z.start()
    time.sleep(1.5)
    res = {net: [l for _, l in sent_after_connect(z.net(net).ircd)] for net in ("n1", "n2")}
    finish(z)
    check(scn, "n1 sends only its own", res["n1"] == ["PRIVMSG #a :from-n1"], repr(res))
    check(scn, "n2 sends only its own", res["n2"] == ["PRIVMSG #a :from-n2"], repr(res))


# --------------------------------------------------------------------------
# sh_* - slash shorthands
# --------------------------------------------------------------------------

SHORTHANDS = [
    ("/msg NickServ hello there", "PRIVMSG NickServ :hello there"),
    ("/MSG bob :already", "PRIVMSG bob :already"),
    ("/notice bob hi", "NOTICE bob :hi"),
    ("/ctcp bob VERSION", "PRIVMSG bob :\x01VERSION\x01"),
    ("/me #a waves", "PRIVMSG #a :\x01ACTION waves\x01"),
    ("/join #c", "JOIN #c"),
    ("/join #c key", "JOIN #c key"),
    ("/part #c", "PART #c"),
    ("/part #c bye now", "PART #c :bye now"),
    ("/quit", "QUIT"),
    ("/quit gone", "QUIT :gone"),
    ("/nick other", "NICK other"),
    ("/topic #c", "TOPIC #c"),
    ("/topic #c new topic", "TOPIC #c :new topic"),
    ("/mode #c +nt", "MODE #c +nt"),
    ("/mode tim", "MODE tim"),
    ("/kick #c bob", "KICK #c bob"),
    ("/kick #c bob go away", "KICK #c bob :go away"),
    ("/invite bob #c", "INVITE bob #c"),
    ("/whois bob", "WHOIS bob"),
    ("/whois bob carol", "WHOIS bob carol"),
    ("/whois irc.server.net bob", "WHOIS irc.server.net bob"),
    ("/whois bob irc.server.net", "WHOIS irc.server.net bob"),
    ("/away", "AWAY"),
    ("/away brb", "AWAY :brb"),
    ("/oper name pw", "OPER name pw"),
    ("/raw PRIVMSG #a :raw", "PRIVMSG #a :raw"),
    ("/quote MODE #a", "MODE #a"),
    ("/ns IDENTIFY x", "PRIVMSG NickServ :IDENTIFY x"),
    ("/cs OP #a", "PRIVMSG ChanServ :OP #a"),
    ("/hs ON", "PRIVMSG HostServ :ON"),
    ("/ms READ 1", "PRIVMSG MemoServ :READ 1"),
    ("/os STATS", "PRIVMSG OperServ :STATS"),
    ("/bs INFO", "PRIVMSG BotServ :INFO"),
    ("/cap ls", "CAP ls"),
    ("/Who #a", "WHO #a"),
    ("PRIVMSG #a :plain raw", "PRIVMSG #a :plain raw"),
    # Characterised: a shorthand missing a required argument is not rejected;
    # the text is stored and later sent exactly as typed, slash included.
    ("/msg bob", "/msg bob"),
    ("/me waves", "/me waves"),
    ("/ns", "/ns"),
    ("/join", "/join"),
    ("/kick #c", "/kick #c"),
    ("/oper name", "/oper name"),
]


def sh_normalise():
    """Each shorthand as stored (the Added echo shows the stored form), and
    a subset fired to confirm the stored line is what reaches the server."""
    scn = "sh_normalise"
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    for i, (typed, want) in enumerate(SHORTHANDS):
        got = c.cmd(M, "Add 0 " + typed)
        check(scn, "%02d %s" % (i, typed), got == ["Added [%d]: delay=0s, cmd=%s" % (i, want)], repr(got))
    st = store(z)
    # Keep only entries that are harmless to send, then fire them.
    # Lines that look like ZNC's own connect traffic (MODE #a) are left out,
    # since sent_after_connect() cannot tell them apart.
    fire = [w for t, w in SHORTHANDS if not w.startswith(("QUIT", "NICK", "PART", "KICK", "JOIN", "OPER", "CAP"))
            and not ZNC_OWN.match(w)]
    c.cmd(M, "Clear")
    for w in fire:
        c.cmd(M, "Add 0 " + w)
    reconnect(z, c, 1.5)
    sent = [l for _, l in sent_after_connect(d)]
    finish(z)
    stored = sorted(base64.b64decode(v.split("|")[-1]).decode() for k, v in st.items() if k.startswith("cmd."))
    check(scn, "NV holds the normalised forms", stored == sorted(w for _, w in SHORTHANDS), repr(stored[:5]))
    check(scn, "fired lines equal the stored lines", sorted(sent) == sorted(fire), repr(sorted(set(sent) ^ set(fire))))


# --------------------------------------------------------------------------
# var_* - %nick% expansion
# --------------------------------------------------------------------------

def var_nick():
    """%nick% expands to the current nick at send time, anywhere in the
    line, as often as it appears; other casings stay literal."""
    scn = "var_nick"
    z = mk(scn, entries=[(0, "PRIVMSG #a :I am %nick% *%nick%*", ""), (0, "PRIVMSG #a :%NICK% %Nick%", "")])
    d = z.net().ircd
    time.sleep(1.5)
    sent = sorted(l for _, l in sent_after_connect(d))
    finish(z)
    check(scn, "expanded and literal", sent == ["PRIVMSG #a :%NICK% %Nick%", "PRIVMSG #a :I am tim *tim*"], repr(sent))


def var_nick_alt():
    """With tim held, ZNC registers as tim_, and %nick% follows it."""
    scn = "var_nick_alt"
    n = Network("n1", chans=["#a"], load_lines=[MOD])
    n.ircd.take("tim")
    z = mk(scn, networks=[n], entries=[(0, "PRIVMSG #a :I am %nick%", "")])
    time.sleep(1.5)
    sent = [l for _, l in sent_after_connect(z.net().ircd)]
    finish(z)
    check(scn, "uses the alternate nick", sent == ["PRIVMSG #a :I am tim_"], repr(sent))


def var_nick_invalid():
    """The server assigns a nick outside IRC nick grammar (tim.x). Entries
    with %nick% are skipped with a message (masked for secret entries);
    entries without it are sent."""
    scn = "var_nick_invalid"
    z = mk(scn, welcome_nick="tim.x",
           entries=[(1, "PRIVMSG #a :hi %nick%", ""), (1, "PRIVMSG #a :secret %nick%", "s"), (1, "PRIVMSG #a :plain", "")])
    d = z.net().ircd
    c = z.client()
    msgs = reconnect(z, c, 2)
    sent = [l for _, l in sent_after_connect(d)]
    finish(z)
    check(scn, "only the plain entry is sent", sent == ["PRIVMSG #a :plain"], repr(sent))
    check(scn, "skip messages", msgs == ["Scheduled 3 delayed command(s).",
                                         "Skipped (invalid nick for expansion): PRIVMSG #a :hi %nick%",
                                         "Skipped (invalid nick for expansion): [hidden]",
                                         "Ran: PRIVMSG #a :plain"], repr(msgs))


# --------------------------------------------------------------------------
# sec_* - control characters and the secret flag
# --------------------------------------------------------------------------

def sec_control_chars():
    """Stored commands with CR, LF or NUL (only possible by editing the
    registry) are never sent. The skip message masks secret entries.
    Characterised: for a non-secret entry the message carries the stored
    line itself, so a lone CR reaches the client inside a module message
    line, and an LF splits it into two module lines."""
    scn = "sec_control_chars"
    z = mk(scn, entries=[(0, "PRIVMSG #a :x\rQUIT :cr", ""), (0, "PRIVMSG #a :y\nQUIT :lf", ""),
                         (0, "PRIVMSG #a :z\x00nul", ""), (0, "PRIVMSG #a :s\r\nQUIT :secret", "s"),
                         (0, "PRIVMSG #a :ok", "")])
    d = z.net().ircd
    c = z.client()
    msgs = reconnect(z, c, 1.5)
    sent = [l for _, l in sent_after_connect(d)]
    finish(z)
    check(scn, "only the clean entry is sent", sent == ["PRIVMSG #a :ok"], repr(sent))
    check(scn, "secret entry masked", "Skipped (contains control chars): [hidden]" in msgs and not any("secret" in m for m in msgs), repr(msgs))
    check(scn, "lone CR reaches the client inside the message (characterised)",
          "Skipped (contains control chars): PRIVMSG #a :x\rQUIT :cr" in msgs, repr(msgs))
    check(scn, "LF splits the message into two module lines (characterised)",
          "Skipped (contains control chars): PRIVMSG #a :y" in msgs and "QUIT :lf" in msgs, repr(msgs))


def sec_secret():
    """AddSecret masks the command in Added, List and Ran: output, and in
    no module message does the text appear; the server receives it in full,
    and the NV value is plain base64 of it with the s flag (output masking,
    not encryption, as the README says)."""
    scn = "sec_secret"
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    out = c.cmd(M, "AddSecret 0 /ns IDENTIFY hunter2")
    lst = c.cmd(M, "List")
    msgs = reconnect(z, c, 1)
    sent = [l for _, l in sent_after_connect(d)]
    st = store(z)
    finish(z)
    alltext = out + lst + msgs
    check(scn, "Added masked", out == ["Added [0]: delay=0s, cmd=[hidden]"], repr(out))
    check(scn, "List masked", lst[-1] == "0     | 0        | [hidden]", repr(lst))
    check(scn, "Ran: masked", msgs == ["Scheduled 1 delayed command(s).", "Ran: [hidden]"], repr(msgs))
    check(scn, "text appears in no module output", not any("hunter2" in x for x in alltext), repr(alltext))
    check(scn, "server receives the full command", sent == ["PRIVMSG NickServ :IDENTIFY hunter2"], repr(sent))
    check(scn, "NV: s flag and plain base64", st == {"cmd.0": "0|s|" + b64("PRIVMSG NickServ :IDENTIFY hunter2")}, repr(st))


# --------------------------------------------------------------------------
# cmd_* - command surface
# --------------------------------------------------------------------------

HELP = [
    "delayedperform %s — run post-connect IRC commands with delays." % NEW_VER,
    "Usage:",
    "  Help [filter]                — Show this help, or commands matching a filter.",
    "  Version                      — Show the module version.",
    "  SetDelay <seconds>           — Set global default delay.",
    "  Add [seconds] <command>      — Raw IRC or shorthands like /msg, /join.",
    "  AddSecret [seconds] <cmd>    — Like Add, but command text is masked in module output.",
    "  List                         — Show index, per-cmd delay, and command.",
    "  Del <index|all>              — Delete by index or everything.",
    "  Clear                        — Same as Del all.",
    "Shorthands: /msg /notice /join /part /quit /nick /topic /mode /kick /invite",
    "            /ctcp /me (/me needs a target) /whois /away /oper /raw /quote",
    "            /ns /cs /hs /ms /os /bs (service aliases).",
    "Vars: %nick% expands to your current IRC nick at send time (e.g., '*%nick%*').",
    "AddSecret masks output only; it is not encryption. Do not use this module for passwords or other credentials.",
]


def cmd_surface():
    """Every command and its error paths, reply by reply, with the NV store
    checked at the end. Characterised: CString::ToUInt truncates to 32 bits,
    so Del 4294967296 deletes entry 0, Add 4294967296 stores delay 0, and
    SetDelay 4294967297 sets 1."""
    scn = "cmd_surface"
    z = mk(scn)
    c = z.client()
    cases = [
        ("Version", ["delayedperform version %s" % NEW_VER]),
        ("Help", HELP),
        ("List", ["No commands configured. Set a global delay with 'SetDelay <s>' and add with 'Add'.", "Current global delay: 0s"]),
        ("SetDelay", ["Usage: SetDelay <seconds>"]),
        ("SetDelay x", ["Usage: SetDelay <seconds>"]),
        ("SetDelay -1", ["Usage: SetDelay <seconds>"]),
        ("SetDelay 4294967297", ["Global delay set to 1s."]),
        ("SetDelay 5", ["Global delay set to 5s."]),
        ("Add", ["Usage: Add [seconds] <irc-or-slash-command>"]),
        ("Add 7", ["Usage: Add [seconds] <irc-or-slash-command>"]),
        ("AddSecret", ["Usage: AddSecret [seconds] <irc-or-slash-command>"]),
        ("Add JOIN #x", ["Added [0]: delay=5s, cmd=JOIN #x"]),
        ("Add 2 JOIN #y", ["Added [1]: delay=2s, cmd=JOIN #y"]),
        ("AddSecret 7 PRIVMSG x :s", ["Added [2]: delay=7s, cmd=[hidden]"]),
        ("Add -3 JOIN #neg", ["Added [3]: delay=5s, cmd=-3 JOIN #neg"]),
        ("SetDelay 9", ["Global delay set to 9s."]),
        ("List", ["Global delay (default): 9s", "Index | Delay(s) | Command", "------+----------+-----------------------------",
                  "0     | 5        | JOIN #x", "1     | 2        | JOIN #y", "2     | 7        | [hidden]", "3     | 5        | -3 JOIN #neg"]),
        ("Del", ["Usage: Del <index|all>"]),
        ("Del x", ["Del: index must be a non-negative integer."]),
        ("Del -1", ["Del: index must be a non-negative integer."]),
        ("Del 4", ["Del: index out of range."]),
        ("Del 1", ["Deleted entry 1."]),
        ("List", ["Global delay (default): 9s", "Index | Delay(s) | Command", "------+----------+-----------------------------",
                  "0     | 5        | JOIN #x", "1     | 7        | [hidden]", "2     | 5        | -3 JOIN #neg"]),
        ("Del 4294967296", ["Deleted entry 0."]),
        ("Add 4294967296 PRIVMSG #a :wrapped", ["Added [2]: delay=0s, cmd=PRIVMSG #a :wrapped"]),
        ("help add", None),
        ("Frobnicate", ["Unknown command!"]),
    ]
    snap = None
    for i, (cmd, exp) in enumerate(cases):
        got = c.cmd(M, cmd)
        if cmd == "Add 4294967296 PRIVMSG #a :wrapped":
            snap = store(z)
        if exp is None:
            text = " ".join(got)
            check(scn, "%02d %s" % (i, cmd), "Add" in text and "AddSecret" in text and "Version" not in text, repr(got))
        else:
            check(scn, "%02d %s" % (i, cmd), got == exp, "got %r want %r" % (got, exp))
    clr = c.cmd(M, "Clear")
    after_clear = store(z)
    c.cmd(M, "Add JOIN #z")
    dall = c.cmd(M, "Del ALL")
    c.quit()
    finish(z)
    check(scn, "NV before Clear", snap == {"global_delay": "9", "cmd.0": entry(7, "PRIVMSG x :s", "s"),
                                           "cmd.1": entry(5, "-3 JOIN #neg"), "cmd.2": entry(0, "PRIVMSG #a :wrapped")}, repr(snap))
    check(scn, "Clear", clr == ["All commands deleted."] and after_clear == {"global_delay": "9"}, repr((clr, after_clear)))
    check(scn, "Del ALL (case-insensitive)", dall == ["All commands deleted."], repr(dall))


def cmd_legacy_store():
    """A hand-edited or v1.0 registry. Legacy delay|b64 entries load, unknown
    flags are kept through a rewrite, an entry with an empty payload shows as
    [corrupt entry] and is not scheduled, an entry with a non-numeric delay
    is not listed, keys that are not cmd.<n> are ignored, and a non-numeric
    global_delay reads as 0. Characterised: with gaps in the keys, Add's
    reported index follows the highest numeric key (here 9), while List and
    Del count positions; and Del's rewrite drops the unparsable entry and the
    non-numeric cmd.foo key from the store."""
    scn = "cmd_legacy_store"
    seed = {
        "global_delay": "abc",
        "cmd.0": "1|" + b64("PRIVMSG #a :legacy"),
        "cmd.2": entry(1, "PRIVMSG #a :secret", "s"),
        "cmd.5": entry(1, "PRIVMSG #a :flags", "zq"),
        "cmd.7": "x|" + b64("PRIVMSG #a :baddelay"),
        "cmd.8": "1||",
        "cmd.foo": entry(1, "PRIVMSG #a :foo"),
    }
    z = mk(scn, extra_nv=seed)
    d = z.net().ircd
    c = z.client()
    lst = c.cmd(M, "List")
    msgs = reconnect(z, c, 2)
    sent = sorted(l for _, l in sent_after_connect(d))
    add = c.cmd(M, "Add 1 PRIVMSG #a :new")
    dl = c.cmd(M, "Del 0")
    st = store(z)
    c.quit()
    finish(z)
    check(scn, "List", lst == ["Global delay (default): 0s", "Index | Delay(s) | Command", "------+----------+-----------------------------",
                               "0     | 1        | PRIVMSG #a :legacy", "1     | 1        | [hidden]", "2     | 1        | PRIVMSG #a :flags",
                               "3     | 1        | [corrupt entry]"], repr(lst))
    check(scn, "scheduled and sent: the three readable entries", msgs[:1] == ["Scheduled 3 delayed command(s)."] and
          sent == ["PRIVMSG #a :flags", "PRIVMSG #a :legacy", "PRIVMSG #a :secret"], repr((msgs, sent)))
    check(scn, "Add index follows the highest key (characterised)", add == ["Added [9]: delay=1s, cmd=PRIVMSG #a :new"], repr(add))
    check(scn, "Del rewrites contiguously, keeps flags, drops unparsable and non-numeric keys (characterised)",
          dl == ["Deleted entry 0."] and st == {"global_delay": "abc",
                                                "cmd.0": entry(1, "PRIVMSG #a :secret", "s"), "cmd.1": entry(1, "PRIVMSG #a :flags", "zq"),
                                                "cmd.2": "1||", "cmd.3": entry(1, "PRIVMSG #a :new")}, repr(st))


# --------------------------------------------------------------------------
# ty_*, up_*, sym_*
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
    """Network-only: --type=user and --type=global are refused."""
    scn = "ty_runtime_load"
    for i, cmd in enumerate(["LoadMod %s" % MOD, "LoadMod --type=user %s" % MOD, "LoadMod --type=global %s" % MOD]):
        z = mk("%s_%d" % (scn, i), load=False)
        c = z.client()
        reply = c.cmd("*status", cmd)
        sections = listmods_sections(c)
        c.quit(); finish(z)
        if i == 0:
            check(scn, cmd, sections == ["Network"], repr((reply, sections)))
        else:
            check(scn, cmd + " refused", sections == [] and any("does not support" in x for x in reply), repr((reply, sections)))


def up_inplace():
    """UpdateMod to the PROBE build: Version reports the probe's version (or
    the old one if glibc cannot unload the build, decided with nm); stored
    entries carry over and fire on the next connect."""
    scn = "up_inplace"
    if not PROBE:
        check(scn, "PROBE build provided", False, "set PROBE; see TESTING.md")
        return
    z = mk(scn)
    d = z.net().ircd
    c = z.client()
    c.cmd(M, "Add 0 PRIVMSG #a :kept")
    z.install_modules({MOD: PROBE})
    upd = c.cmd("*status", "UpdateMod " + MOD)
    v1 = c.cmd(M, "Version")
    reconnect(z, c, 1)
    sent = [l for _, l in sent_after_connect(d)]
    finish(z)
    stuck = pinned(NEW)
    print("    info: build under test unloadable by glibc in this znc: %s" % ("no (pinned)" if stuck else "yes"))
    exp = NEW_VER if stuck else version_of(PROBE)
    check(scn, "UpdateMod reports Done", upd[-1:] == ["Done"], repr(upd))
    check(scn, "version after UpdateMod is %s" % exp, v1 == ["delayedperform version %s" % exp], repr(v1))
    check(scn, "entries carried over", sent == ["PRIVMSG #a :kept"], repr(sent))


def sym_unique():
    scn = "sym_unique"
    extra = sorted(unique_syms(NEW) - unique_syms(ZNC_BIN))
    check(scn, "no unique symbols beyond znc's", not extra, repr(extra))


SCENARIOS = [fire_order, fire_global_delay, fire_reconnect_cancels, fire_unload_pending, fire_hot_load, fire_restart,
             fire_isolation, sh_normalise, var_nick, var_nick_alt, var_nick_invalid, sec_control_chars, sec_secret,
             cmd_surface, cmd_legacy_store, ty_runtime_load, up_inplace, sym_unique]


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
