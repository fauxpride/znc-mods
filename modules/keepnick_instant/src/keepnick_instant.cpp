// keepnick_instant.cpp — auto-backend, idle-aware, join-safe nick reclaim for ZNC
// Version: 1.6.8
// Behavior:
//   • Backend mode: Auto (default) uses MONITOR when advertised via 005, and can also live-probe MONITOR on hot-swapped already-connected sessions; otherwise falls back to ISON
//   • Polls with ISON every Interval seconds (default 5s) ONLY when backend resolves to ISON and you don't own the Primary nick
//   • MONITOR mode subscribes after StartDelay, reacts to 731/730 events when the server supports it, and uses remembered offline state for local retry cadence (no periodic MONITOR S polling)
//   • Idle-aware: skips a backend check if you sent any command in the last IdleGap seconds (default 2s)
//   • Join-safe: first backend action is delayed StartDelay seconds after connect (default 90s)
//   • Dedupes: won't send repeated NICK within MinGap seconds (default 3s)
//   • Jitter: adds 0..Jitter seconds (default 0) to each scheduled backend tick so it doesn't align with other timers
//
// Build: znc-buildmod keepnick_instant.cpp
#include <znc/Modules.h>
#include <znc/IRCNetwork.h>
#include <znc/User.h>
#include <ctime>
#include <vector>
#include <cstdlib>

static unsigned Rand0To(unsigned n) { return n ? (std::rand() % (n + 1)) : 0; }

class CKeepNickInstant;

class CISONOnceTimer : public CTimer {
 public:
  CISONOnceTimer(CModule* pMod, unsigned int secs, const CString& sLabel)
    : CTimer(pMod, secs, 1, sLabel, "Backend tick (single)") {}
  void RunJob() override;
};

class CRearmTimer : public CTimer {
 public:
  CRearmTimer(CModule* pMod, unsigned int secs)
    : CTimer(pMod, secs, 1, "", "Reclaim dedupe") {}
  void RunJob() override;
};

class CKeepNickInstant : public CModule {
  friend class CISONOnceTimer;
  friend class CRearmTimer;

 private:
  enum EBackendMode {
    BACKEND_AUTO = 0,
    BACKEND_ISON,
    BACKEND_MONITOR,
  };

  // ---- Config (persisted) ----
  CString      Primary;
  bool         Enabled           = true;           // Enable/disable module logic
  EBackendMode BackendMode       = BACKEND_AUTO;   // Auto / Ison / Monitor
  unsigned     IntervalSec       = 5;              // ISON interval (s) when backend resolves to ISON and you DON'T own the nick
  unsigned     IdleGapSec        = 2;              // Skip backend tick if you were active in last N seconds
  unsigned     StartDelaySec     = 90;             // First backend action after connect (join-safe)
  unsigned     JitterSec         = 0;              // Add 0..J seconds to each scheduled backend tick (DEFAULT 0)
  unsigned     MinGapSec         = 3;              // Min spacing between actual NICK attempts

  // ---- State ----
  bool     TimerArmed              = false;        // Is a single-shot backend timer active
  time_t   LastUserActivity        = 0;            // Updated on any user-sent line
  time_t   LastAttempt             = 0;            // Last time we sent NICK <Primary>
  bool     MonitorSupported        = false;        // Saw MONITOR in 005 / ISUPPORT for this connection
  bool     MonitorUsable           = true;         // Cleared if MONITOR add failed (e.g. 734)
  bool     MonitorActive           = false;        // Did we add Primary to MONITOR on this connection?
  bool     MonitorProbeSent        = false;        // Sent a live MONITOR probe on an already-connected session without having seen 005 yet
  bool     MonitorKnownFree        = false;        // In MONITOR mode, last known state for Primary is offline/free; local scheduler retries NICK without MONITOR S polling
  unsigned MonitorLimit            = 0;            // Optional MONITOR=<n> limit from ISUPPORT
  bool     HotReloadDetectPending   = false;        // Fire a one-time MONITOR probe on hot reload even when HavePrimary() is true
  bool     NickAttemptPending        = false;        // Set when we fire NICK <Primary>; used to swallow the 433 response so client scripts don't see it
  int      ISONQueryPending          = 0;            // Counts outstanding module-sent ISONs; 303 is swallowed while > 0, supporting multiple in-flight responses correctly
  unsigned BackendTimerSerial      = 0;            // Unique backend timer label suffix
  CString  BackendTimerLabel;                      // Currently armed backend timer label

  // ---- Helpers ----
  static CString BackendModeToString(EBackendMode mode) {
    switch (mode) {
      case BACKEND_ISON:    return "Ison";
      case BACKEND_MONITOR: return "Monitor";
      default:              return "Auto";
    }
  }

  static bool ParseBackendMode(const CString& s, EBackendMode& mode) {
    CString x = s.AsLower();
    if (x == "auto")    { mode = BACKEND_AUTO; return true; }
    if (x == "ison")    { mode = BACKEND_ISON; return true; }
    if (x == "monitor") { mode = BACKEND_MONITOR; return true; }
    return false;
  }

  // Reject any nick string that could cause IRC protocol injection when
  // concatenated into a PutIRC() line. Primary is concatenated into
  //   NICK <Primary>, ISON <Primary>, MONITOR + <Primary>, MONITOR - <Primary>
  // without further escaping, so any framing-significant character here
  // would let the caller inject additional IRC commands or additional
  // MONITOR list entries. We reject:
  //   • empty
  //   • any C0 control char (CR/LF/NUL/TAB/etc.) and DEL — framing bytes
  //   • space            — IRC parameter separator
  //   • comma            — MONITOR target list separator
  //   • leading ':'      — would be parsed as the trailing-parameter marker
  // This is intentionally narrower than full RFC 1459/2812 nick validation:
  // we are only closing the injection surface, not enforcing server-side
  // nick grammar (servers differ, e.g. on allowed length and casemapping).
  // Any legitimate IRC nick passes.
  static bool IsValidNick(const CString& nick) {
    if (nick.empty()) return false;
    if (nick[0] == ':') return false;
    for (size_t i = 0; i < nick.size(); ++i) {
      unsigned char c = (unsigned char)nick[i];
      if (c < 0x20 || c == 0x7F) return false;
      if (c == ' ' || c == ',') return false;
    }
    return true;
  }

  void LoadPrimary(const CString& arg) {
    // 1) Explicit module argument wins, but only if it passes injection-safe
    //    validation. A bad argument falls through to the next source rather
    //    than being silently accepted and later concatenated into PutIRC().
    if (!arg.empty()) {
      if (IsValidNick(arg)) {
        Primary = arg;
        SetNV("PrimaryNick", Primary);
        return;
      }
      PutModule("Warning: module argument nick contains invalid characters (control/space/comma/leading-':') and was ignored.");
    }
    // 2) Stored NV value. If an old version (pre-1.6.8) persisted a bad
    //    value, or the registry was manually edited, clear it and fall
    //    through so the module cannot resurrect an unsafe nick on every
    //    load.
    CString stored = GetNV("PrimaryNick");
    if (!stored.empty()) {
      if (IsValidNick(stored)) { Primary = stored; return; }
      PutModule("Warning: stored PrimaryNick contained invalid characters and has been cleared.");
      DelNV("PrimaryNick");
    }
    // 3) Use the ZNC-configured nick as the authoritative source, not
    // GetNetwork()->GetNick() which returns the current IRC nick. On a hot reload
    // while connected as an alternate (e.g. fauxpride-), GetNetwork()->GetNick()
    // would return the alternate and the module would poll for the wrong nick.
    // GetUser()->GetNick() is what the user has configured ZNC to use and is the
    // correct fallback when no PrimaryNick is stored.
    CString userNick = GetUser() ? GetUser()->GetNick() : CString("");
    if (IsValidNick(userNick)) {
      Primary = userNick;
    } else {
      // Defensive: even a corrupted ZNC user config shouldn't let an unsafe
      // value reach Primary. Fall back to a known-safe literal.
      Primary = "ZNCUser";
    }
  }

  void LoadNV() {
    Enabled = GetNV("Enabled").empty() ? true : (GetNV("Enabled") == "1");
    {
      CString r = GetNV("BackendMode");
      if (!r.empty()) {
        EBackendMode mode;
        if (ParseBackendMode(r, mode)) BackendMode = mode;
      }
    }
    { CString r=GetNV("IntervalSec");   if (!r.empty()) { unsigned v=r.ToUInt(); if (v>=5 && v<=300) IntervalSec=v; } }
    { CString r=GetNV("IdleGapSec");    if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=30)          IdleGapSec=v; } }
    { CString r=GetNV("StartDelaySec"); if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=600)         StartDelaySec=v; } }
    { CString r=GetNV("JitterSec");     if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=10)          JitterSec=v; } }
    { CString r=GetNV("MinGapSec");     if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=30)          MinGapSec=v; } }
  }

  void SaveNV() {
    SetNV("Enabled",       Enabled ? "1" : "0");
    SetNV("BackendMode",   BackendModeToString(BackendMode));
    SetNV("IntervalSec",   CString(IntervalSec));
    SetNV("IdleGapSec",    CString(IdleGapSec));
    SetNV("StartDelaySec", CString(StartDelaySec));
    SetNV("JitterSec",     CString(JitterSec));
    SetNV("MinGapSec",     CString(MinGapSec));
    SetNV("PrimaryNick",   Primary);
  }

  bool HavePrimary() const {
    return GetNetwork() &&
           GetNetwork()->GetIRCNick().GetNick().Equals(Primary, false);
  }

  bool IsConnected() const {
    return GetNetwork() && GetNetwork()->IsIRCConnected();
  }

  bool UseMonitorBackend() const {
    if (BackendMode == BACKEND_ISON) return false;
    if (!MonitorSupported || !MonitorUsable) return false;
    return true; // Auto and Monitor both use MONITOR when it's actually available/usable
  }

  CString EffectiveBackendName() const {
    return UseMonitorBackend() ? "MONITOR" : "ISON";
  }

  void ArmBackend(unsigned delay) {
    if (!Enabled || TimerArmed) return;
    TimerArmed = true;
    BackendTimerLabel = "backend_once_" + CString(++BackendTimerSerial);
    if (!AddTimer(new CISONOnceTimer(this, delay, BackendTimerLabel))) {
      TimerArmed = false;
      BackendTimerLabel = "";
      PutModule("Warning: failed to arm backend timer.");
    }
  }

  void ForceArmBackend(unsigned delay) {
    if (!Enabled) return;
    if (TimerArmed && !BackendTimerLabel.empty()) RemTimer(BackendTimerLabel);
    TimerArmed = false;
    BackendTimerLabel = "";
    ArmBackend(delay);
  }

  void RearmBackend() {
    TimerArmed = false;
    BackendTimerLabel = "";
    unsigned next = IntervalSec + Rand0To(JitterSec);
    ArmBackend(next);
  }

  void RunBackendCheckNow(bool bIgnoreIdle) {
    if (!Enabled) return;

    // Hot-reload MONITOR detection: fires once at the first backend tick after a hot
    // reload into an already-live session, regardless of whether we currently own the
    // primary nick. MaybeProbeMonitor() is gated by HavePrimary() and would silently
    // skip the probe if we are already on our primary nick, leaving MonitorSupported
    // false for the entire session. This path bypasses that gate so MONITOR support is
    // correctly established before it is ever needed.
    if (HotReloadDetectPending) {
      HotReloadDetectPending = false;
      if (BackendMode != BACKEND_ISON && MonitorUsable && !MonitorSupported &&
          !MonitorProbeSent && !MonitorActive && IsConnected() && !Primary.empty()) {
        PutIRC("MONITOR + " + Primary);
        MonitorProbeSent = true;
        // MonitorActive is not set here; only set on 730/731 server confirmation.
        return; // wait for server response before doing anything else this tick
      }
    }

    if (HavePrimary()) {
      if (MonitorActive) StopMonitor(Primary);
      MonitorKnownFree = false;
      return;
    }

    time_t now = std::time(nullptr);
    if (!bIgnoreIdle && LastUserActivity && (now - LastUserActivity) < (time_t)IdleGapSec) return;

    if (UseMonitorBackend()) {
      if (!MonitorActive) EnsureMonitor();
      else if (MonitorKnownFree) TryReclaim();
    } else if (!MaybeProbeMonitor()) {
      if (MonitorActive) StopMonitor(Primary);
      ISONQueryPending++;  // arm 303 swallow for this poll; counter handles multiple in-flight ISONs
      PutIRC("ISON " + Primary);
    }
  }

  void KickBackendNow(bool bIgnoreIdle) {
    if (!Enabled || Primary.empty()) return;
    RunBackendCheckNow(bIgnoreIdle);
    if (!HavePrimary()) ForceArmBackend(IntervalSec + Rand0To(JitterSec));
  }

  bool MaybeProbeMonitor() {
    if (!Enabled) return false;
    if (BackendMode == BACKEND_ISON) return false;
    if (!MonitorUsable || MonitorSupported || MonitorProbeSent || MonitorActive) return false;
    if (!IsConnected()) return false;
    if (Primary.empty() || HavePrimary()) return false;
    PutIRC("MONITOR + " + Primary);
    MonitorProbeSent = true;
    // MonitorActive is intentionally NOT set here — it is only set to true
    // once the server confirms support via a 730/731 response. Setting it
    // prematurely here would cause other code paths to treat an unconfirmed
    // probe as an active subscription, which can lead to spurious MONITOR -
    // commands and silent fallback failures on servers that drop unknown commands.
    return true;
  }

  void StopMonitor(const CString& nick) {
    if (!MonitorActive || nick.empty()) { MonitorKnownFree = false; return; }
    PutIRC("MONITOR - " + nick);
    MonitorActive = false;
    MonitorProbeSent = false; // allow EnsureMonitor to re-subscribe if needed
    MonitorKnownFree = false;
  }

  void EnsureMonitor() {
    if (!Enabled) return;
    if (HavePrimary()) return;
    if (!UseMonitorBackend()) return;
    if (MonitorActive || MonitorProbeSent) return; // already active or sent but awaiting server confirmation
    PutIRC("MONITOR + " + Primary);
    MonitorProbeSent = true;
    // MonitorActive is intentionally NOT set here. Same policy as MaybeProbeMonitor():
    // MonitorActive is only set to true once the server confirms the subscription via
    // a 730 or 731 numeric. Setting it here optimistically would cause the backend
    // to treat an unconfirmed subscription as active and block the ISON fallback
    // on servers that silently drop the MONITOR command.
  }

  bool MonitorListContainsPrimary(const CString& rawList, bool allowHostmask) const {
    VCString targets;
    rawList.Split(",", targets, false);
    for (const auto& entry : targets) {
      CString nick = allowHostmask ? entry.Token(0, false, "!") : entry;
      if (nick.Equals(Primary, false)) return true;
    }
    return false;
  }

  void TryReclaim() {
    if (!Enabled) return;
    if (HavePrimary()) return;
    time_t now = std::time(nullptr);
    if (LastAttempt && now - LastAttempt < (time_t)MinGapSec) return; // dedupe
    LastAttempt = now;
    NickAttemptPending = true;  // arm 433 swallow for this attempt
    PutIRC("NICK " + Primary);
    if (!AddTimer(new CRearmTimer(this, MinGapSec))) {
      PutModule("Warning: failed to arm reclaim dedupe timer.");
    }
  }

 public:
  MODCONSTRUCTOR(CKeepNickInstant) {}

  bool OnLoad(const CString& sArgs, CString& sMessage) override {
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)(uintptr_t)this);
    LoadPrimary(sArgs.Trim_n());
    LoadNV();
    // Do not send any IRC commands synchronously in OnLoad. Any PutIRC() here fires
    // directly into the send queue without the StartDelay guard, which is problematic
    // during a hot reload into a lagged or busy connection.
    //
    // Distinguish between two cases:
    //   - Hot reload (IsConnected() == true): the connection is already live and stable;
    //     the join-safe StartDelay is unnecessary. Use one normal IntervalSec tick so we
    //     do not fire synchronously but also do not wait 90s to discover MONITOR or begin
    //     reclaim on an already-established session.
    //   - Normal load (not yet connected): ArmBackend here is a no-op since
    //     OnIRCConnected() will call ArmBackend(StartDelaySec) when the connection
    //     actually comes up. The call below is kept for the edge case where the module
    //     is loaded while the network thinks it is connected (e.g. a brief state window).
    if (IsConnected()) {
      HotReloadDetectPending = (BackendMode != BACKEND_ISON); // probe MONITOR support regardless of nick ownership
      ArmBackend(IntervalSec); // hot reload into live session: short delay, no join-burst risk
    } else {
      ArmBackend(StartDelaySec); // normal load: join-safe delay (also armed by OnIRCConnected)
    }
    return true;
  }

  void OnIRCConnected() override {
    // Cancel any armed backend timer before clearing the label. Without RemTimer,
    // the old timer stays in ZNC's queue even though TimerArmed and BackendTimerLabel
    // are cleared, causing ArmBackend below to arm a second timer. Both then fire
    // independently on their next tick, creating duplicate ISON cycles that race
    // against each other and defeat the ISONQueryPending swallow.
    if (!BackendTimerLabel.empty()) RemTimer(BackendTimerLabel);
    TimerArmed = false;
    BackendTimerLabel = "";
    LastAttempt = 0;
    MonitorSupported = false;
    MonitorUsable = true;
    MonitorActive = false;
    MonitorProbeSent = false;
    MonitorKnownFree = false;
    MonitorLimit = 0;
    HotReloadDetectPending = false;
    NickAttemptPending = false;
    ISONQueryPending = 0;
    ArmBackend(StartDelaySec); // join-safe start
  }

  void OnIRCDisconnected() override {
    // Cancel any armed backend timer before clearing the label, for the same reason
    // as OnIRCConnected: without RemTimer the stale timer remains in ZNC's queue and
    // can fire after reconnect, producing a duplicate timer cycle alongside the one
    // armed by OnIRCConnected.
    if (!BackendTimerLabel.empty()) RemTimer(BackendTimerLabel);
    TimerArmed = false;
    BackendTimerLabel = "";
    LastAttempt = 0;
    MonitorSupported = false;
    MonitorUsable = true;
    MonitorActive = false;
    MonitorProbeSent = false;
    MonitorKnownFree = false;
    MonitorLimit = 0;
    HotReloadDetectPending = false;
    NickAttemptPending = false;
    ISONQueryPending = 0;
  }

  // Track user activity: any outbound line from your client through ZNC
  EModRet OnUserRaw(CString& /*line*/) override {
    LastUserActivity = std::time(nullptr);
    return CONTINUE;
  }

  // Handle 005 ISUPPORT, ISON replies, MONITOR numerics, MONITOR probe fallbacks, and visible NICK/QUIT (when you share a channel)
  EModRet OnRaw(CString& sLine) override {
    // Cheap pre-filter: extract only the command token before allocating a VCString.
    // OnRaw fires for every IRC line including PRIVMSG, NOTICE, JOIN, PART, MODE, etc.
    // A full Split() on every line wastes CPU and heap when we only care about 8 types.
    CString cmd = sLine.Token(1);
    if (cmd != "005" && cmd != "303" &&
        cmd != "730" && cmd != "731" && cmd != "734" && cmd != "421" &&
        cmd != "433" &&
        !cmd.Equals("NICK", false) && !cmd.Equals("QUIT", false))
      return CONTINUE;

    VCString v; sLine.Split(" ", v, false);
    if (v.size() < 2) return CONTINUE;

    // ISUPPORT: 005 <me> token token ... :are supported by this server
    if (cmd == "005") {
      for (size_t i = 3; i < v.size(); ++i) {
        const CString& tok = v[i];
        if (tok.StartsWith(":")) break;

        CString key = tok.Token(0, false, "=");
        if (key.Equals("MONITOR", false)) {
          MonitorSupported = true;
          CString val = tok.Token(1, false, "=");
          if (!val.empty()) MonitorLimit = val.ToUInt();
        }
      }
      return CONTINUE;
    }

    // ISON result: 303 <me> :nick1 nick2 ...
    if (cmd == "303") {
      // Older ZNC API compat: trailing starts at token index 3
      CString trailing = sLine.Token(3, true);    // ":nick1 nick2 ..."
      trailing.TrimLeft(CString(":"));           // Trim leading ':'

      bool present = false;
      VCString names; trailing.Split(" ", names, false);
      for (const auto& n : names) {
        if (n.Equals(Primary, false)) { present = true; break; }
      }
      if (!present) TryReclaim();

      // Swallow 303 responses caused by our own ISON polls so they do not appear
      // in the client status window. ISONQueryPending counts outstanding module-sent
      // ISONs; a counter rather than a bool correctly handles multiple in-flight 303
      // responses (e.g. from a brief duplicate timer race). If the count is above zero
      // this 303 is ours — decrement and swallow. Manual /ison commands sent by the
      // user do not increment the counter and are always passed through normally.
      if (ISONQueryPending > 0) { ISONQueryPending--; return HALT; }
      return CONTINUE;
    }

    // MONITOR online/offline numerics
    if (cmd == "730" || cmd == "731") {
      // Server demonstrably speaks MONITOR — this flag is safe to flip
      // unconditionally since any 730/731 proves capability regardless of
      // which nick the reply is about.
      MonitorSupported = true;
      CString trailing = sLine.Token(3, true);
      trailing.TrimLeft(CString(":"));
      bool match = MonitorListContainsPrimary(trailing, cmd == "730");
      if (match) {
        // Only flip "active" and "usable" when the reply is actually for
        // our own subscription target. Previously these were flipped on
        // any 730/731, which allowed a misbehaving or hostile server to
        // send 730/731 for unrelated nicks and make the module believe
        // its own subscription to Primary was confirmed. That could
        // suppress ISON fallback even though we were never actually
        // subscribed to Primary on this connection.
        MonitorUsable = true;
        MonitorActive = true;
        MonitorKnownFree = (cmd == "731");
        if (cmd == "731") TryReclaim();
      }
      return CONTINUE;
    }

    // MONITOR list full for our add; fall back to ISON on this connection
    if (cmd == "734") {
      if (v.size() >= 5 && MonitorListContainsPrimary(v[4], false)) {
        MonitorSupported = true;
        MonitorUsable = false;
        MonitorActive = false;
        MonitorKnownFree = false;
        PutModule("MONITOR list full/unusable for this target on this connection; falling back to ISON.");
        ForceArmBackend(IntervalSec);
      }
      return CONTINUE;
    }

    // MONITOR unsupported on this connection/server; fall back to ISON
    // 421 is matched for any unknown command reply where the rejected command is MONITOR.
    // If MonitorProbeSent is true, the probe was sent by the module — suppress both the
    // 421 numeric from reaching the client (which would show "Server does not recognize
    // MONITOR command" in mIRC) and the PutModule fallback message, since this is expected
    // on ISON-only networks like Undernet and needs no user-visible notification.
    // If MonitorProbeSent is false, the user sent the MONITOR command manually — pass the
    // 421 through to the client and show the informational PutModule message as before.
    if (cmd == "421") {
      if (v.size() >= 4 && v[3].Equals("MONITOR", false)) {
        bool ours = MonitorProbeSent;
        MonitorSupported = false;
        MonitorUsable = false;
        MonitorActive = false;
        MonitorKnownFree = false;
        MonitorProbeSent = false;
        ForceArmBackend(IntervalSec);
        if (ours) return HALT; // swallow — module-generated probe, no client noise
        PutModule("MONITOR unsupported on this connection; falling back to ISON.");
      }
      return CONTINUE;
    }

    // 433 Nickname already in use — swallow responses caused by our own NICK attempts
    // so client-side scripts (e.g. mIRC Peace & Protection) do not see them and activate
    // their own competing retake logic. Only swallowed when NickAttemptPending is set,
    // meaning the module fired the NICK attempt. 433 responses from manual nick changes
    // (NickAttemptPending == false) are passed through normally.
    // Format: :server 433 <currentnick> <attemptednick> :Nickname is already in use
    if (cmd == "433") {
      if (NickAttemptPending && v.size() >= 4 && v[3].Equals(Primary, false)) {
        NickAttemptPending = false;
        return HALT; // swallow — client does not see this 433
      }
      NickAttemptPending = false; // clear stale flag on any 433 regardless
      return CONTINUE;            // not ours — pass through to client
    }

    // Visible events: :old!u@h NICK :new   /   :nick!u@h QUIT :msg
    if (v[0].StartsWith(":")) {
      if (cmd.Equals("NICK", false)) {
        CString oldnick = v[0].TrimPrefix_n(":"); oldnick = oldnick.Token(0, false, "!");
        CString newnick; if (v.size() >= 3 && v[2].StartsWith(":")) newnick = v[2].TrimPrefix_n(":");
        if (oldnick.Equals(Primary, false) && !newnick.Equals(Primary, false)) TryReclaim();
      } else if (cmd.Equals("QUIT", false)) {
        CString qnick = v[0].TrimPrefix_n(":"); qnick = qnick.Token(0, false, "!");
        if (qnick.Equals(Primary, false)) TryReclaim();
      }
    }
    return CONTINUE;
  }

  // Command interface — clear help with defaults
  void OnModCommand(const CString& sCmdLine) override {
    CString cmd = sCmdLine.Token(0).AsLower();

    if (cmd == "enable") {
      Enabled = true;
      SaveNV();
      PutModule("Enabled. Backend checks run only when you don't own the nick.");
      if (!HavePrimary()) KickBackendNow(true);
    }
    else if (cmd == "disable") {
      Enabled = false;
      SaveNV();
      if (MonitorActive) StopMonitor(Primary);
      MonitorKnownFree = false;
      PutModule("Disabled. No backend checks or reclaim attempts.");
    }
    else if (cmd == "setnick") {
      CString nick = sCmdLine.Token(1, true).Trim_n();
      if (nick.empty()) { PutModule("Usage: SetNick <nick> — Set & persist the primary nick to reclaim."); return; }
      if (!IsValidNick(nick)) {
        PutModule("Rejected: nick must not be empty, start with ':', or contain spaces, commas, or control characters. "
                  "(These would be interpreted as IRC framing when the module concatenates the nick into NICK / ISON / MONITOR commands.)");
        return;
      }
      CString old = Primary;
      if (MonitorActive && !old.empty() && !old.Equals(nick, false)) StopMonitor(old);
      Primary = nick;
      SaveNV();
      MonitorUsable = true;
      MonitorProbeSent = false;
      MonitorKnownFree = false;
      PutModule("Primary nick set to: " + Primary);
      if (!HavePrimary()) {
        KickBackendNow(true);
      }
    }
    else if (cmd == "backend") {
      EBackendMode mode;
      if (ParseBackendMode(sCmdLine.Token(1), mode)) {
        BackendMode = mode;
        SaveNV();
        MonitorUsable = true;
        MonitorKnownFree = false;
        if (MonitorActive && !UseMonitorBackend()) StopMonitor(Primary);
        if (!HavePrimary()) {
          KickBackendNow(true);
        }
        PutModule("Backend mode set to " + BackendModeToString(BackendMode) + ". Effective backend now: " + EffectiveBackendName() + ".");
      } else {
        PutModule("Usage: Backend <Auto|Ison|Monitor> — Default Auto.");
      }
    }
    else if (cmd == "interval") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v>=5 && v<=300) { IntervalSec=v; SaveNV(); PutModule("Interval set to " + CString(v) + "s (ISON frequency when backend resolves to ISON and you don't own the nick)."); }
      else PutModule("Usage: Interval <seconds 5-300> — Default 5s.");
    }
    else if (cmd == "idlegap") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=30) { IdleGapSec=v; SaveNV(); PutModule("IdleGap set to " + CString(v) + "s. Skip a backend tick if you've sent anything in the last IdleGap seconds."); }
      else PutModule("Usage: IdleGap <seconds 0-30> — Default 2s.");
    }
    else if (cmd == "startdelay") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=600) { StartDelaySec=v; SaveNV(); PutModule("StartDelay set to " + CString(v) + "s. First backend action after connect (join-safe). Effective next connect."); }
      else PutModule("Usage: StartDelay <seconds 0-600> — Default 90s.");
    }
    else if (cmd == "jitter") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=10) { JitterSec=v; SaveNV(); PutModule("Jitter set to 0.." + CString(v) + "s. Random extra wait added to each scheduled backend tick."); }
      else PutModule("Usage: Jitter <seconds 0-10> — Default 0s.");
    }
    else if (cmd == "mingap") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=30) { MinGapSec=v; SaveNV(); PutModule("MinGap set to " + CString(v) + "s. Minimum spacing between NICK attempts (dedupe)."); }
      else PutModule("Usage: MinGap <seconds 0-30> — Default 3s.");
    }
    else if (cmd == "poke") {
      LastUserActivity = 0; // ensure it's considered idle
      if (MonitorActive && !UseMonitorBackend()) StopMonitor(Primary);
      KickBackendNow(true);
      PutModule("Poke: forcing an immediate backend check now.");
    }
    else if (cmd == "show" || cmd.empty() || cmd == "help") {
      CString cur = GetNetwork() ? GetNetwork()->GetIRCNick().GetNick() : "<none>";
      PutModule("keepnick_instant — auto-backend nick reclaim (MONITOR when available, otherwise ISON; idle-aware & join-safe).");
      PutModule("Version: 1.6.8");
      PutModule("Current state:");
      PutModule("  Status: " + CString(Enabled ? "ENABLED" : "DISABLED"));
      PutModule("  Primary: " + Primary + "   |   Current: " + cur);
      PutModule("  Backend mode: " + BackendModeToString(BackendMode) + "   |   Effective: " + EffectiveBackendName());
      CString monLine = "  MONITOR advertised/detected: " + CString(MonitorSupported ? "yes" : "no");
      if (MonitorLimit) monLine += "   |   Limit: " + CString(MonitorLimit);
      monLine += "   |   Active: " + CString(MonitorActive ? "yes" : "no");
      monLine += "   |   Live probe sent: " + CString(MonitorProbeSent ? "yes" : "no");
      PutModule(monLine);
      PutModule("  Interval: " + CString(IntervalSec) + "s   (default 5s)");
      PutModule("  IdleGap: " + CString(IdleGapSec) + "s     (default 2s)");
      PutModule("  StartDelay: " + CString(StartDelaySec) + "s (default 90s; first backend action after connect)");
      PutModule("  Jitter: 0.." + CString(JitterSec) + "s      (default 0s; random extra wait)");
      PutModule("  MinGap: " + CString(MinGapSec) + "s        (default 3s; dedupe NICK attempts)");
      PutModule("Commands:");
      PutModule("  Enable | Disable");
      PutModule("  SetNick <nick>            — Set primary nick (persisted).");
      PutModule("  Backend <Auto|Ison|Monitor> — Select backend mode (default Auto).");
      PutModule("  Interval <5-300>          — ISON interval when effective backend is ISON (default 5s).");
      PutModule("  IdleGap <0-30>            — Skip backend tick if active in last N seconds (default 2s).");
      PutModule("  StartDelay <0-600>        — First backend action after connect (default 90s).");
      PutModule("  Jitter <0-10>             — Add 0..J seconds to each scheduled backend tick (default 0s).");
      PutModule("  MinGap <0-30>             — Minimum spacing between NICK attempts (default 3s).");
      PutModule("  Poke                      — Force an immediate backend check now (ignores StartDelay and idle suppression; may live-probe MONITOR if needed).");
      PutModule("  Show | Help               — Show this status/help.");
    }
    else {
      PutModule("Unknown command. Try: Help");
    }
  }
};

// ----- timers -----
void CISONOnceTimer::RunJob() {
  auto* m = (CKeepNickInstant*)GetModule();
  m->TimerArmed = false;
  m->BackendTimerLabel = "";

  if (!m->Enabled) return;

  m->RunBackendCheckNow(false);
  m->RearmBackend();
}

void CRearmTimer::RunJob() {
  // After a reactive reclaim attempt (triggered by a visible NICK/QUIT event),
  // verify the reclaim loop is still running. If the reclaim was rejected
  // (e.g. a race where someone else grabbed the nick first), this ensures
  // the backend scheduler picks back up rather than going silent until the
  // next incidentally scheduled tick.
  auto* m = (CKeepNickInstant*)GetModule();
  if (!m->TimerArmed && !m->HavePrimary())
    m->ForceArmBackend(m->IntervalSec + Rand0To(m->JitterSec));
}

template<> void TModInfo<CKeepNickInstant>(CModInfo& Info) {
  Info.SetHasArgs(true);
  Info.SetDescription("Auto-backend, idle-aware, join-safe nick reclaim for ZNC (MONITOR when available with remembered offline state and local retry cadence, otherwise ISON; OnRaw pre-filter, hot-reload MONITOR detect, 433/303/421 swallow, timer cleanup, ISON counter, nick validation, match-gated MONITOR state)");
}
NETWORKMODULEDEFS(CKeepNickInstant, "Auto-backend keepnick (MONITOR/ISON instant-ish, hot-reload safe, local MONITOR retry state) v1.6.8")
