// keepnick_instant.cpp — ISON-only, idle-aware, join-safe nick reclaim for ZNC
// Version: 1.2.0
// Behavior:
//   • Polls with ISON every Interval seconds (default 5s) ONLY when you don't own the Primary nick
//   • Idle-aware: skips a poll if you sent any command in the last IdleGap seconds (default 2s)
//   • Join-safe: first poll is delayed StartDelay seconds after connect (default 90s)
//   • Dedupes: won't send repeated NICK within MinGap seconds (default 3s)
//   • Jitter: adds 0..Jitter seconds (default 0) to each poll so it doesn't align with other timers
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
  CISONOnceTimer(CModule* pMod, unsigned int secs)
    : CTimer(pMod, secs, 1, "ISON poll (single)", "ison_once") {}
  void RunJob() override;
};

class CRearmTimer : public CTimer {
 public:
  CRearmTimer(CModule* pMod, unsigned int secs)
    : CTimer(pMod, secs, 1, "Reclaim dedupe", "rearm") {}
  void RunJob() override;
};

class CKeepNickInstant : public CModule {
  friend class CISONOnceTimer;
  friend class CRearmTimer;

 private:
  // ---- Config (persisted) ----
  CString  Primary;
  bool     Enabled           = true;   // Enable/disable module logic
  unsigned IntervalSec       = 5;      // ISON interval (s) when you DON'T own the nick
  unsigned IdleGapSec        = 2;      // Skip poll if you were active in last N seconds
  unsigned StartDelaySec     = 90;     // First poll after connect (join-safe)
  unsigned JitterSec         = 0;      // Add 0..J seconds to each poll (DEFAULT 0)
  unsigned MinGapSec         = 3;      // Min spacing between actual NICK attempts

  // ---- State ----
  bool   TimerArmed       = false;     // Is a single-shot ISON timer active
  time_t LastUserActivity = 0;         // Updated on any user-sent line
  time_t LastAttempt      = 0;         // Last time we sent NICK <Primary>

  // ---- Helpers ----
  void LoadPrimary(const CString& arg) {
    if (!arg.empty()) { Primary = arg; SetNV("PrimaryNick", Primary); return; }
    CString stored = GetNV("PrimaryNick");
    if (!stored.empty()) { Primary = stored; return; }
    Primary = GetNetwork() && !GetNetwork()->GetNick().empty()
               ? GetNetwork()->GetNick()
               : (GetUser() ? GetUser()->GetNick() : "ZNCUser");
  }

  void LoadNV() {
    Enabled = GetNV("Enabled").empty() ? true : (GetNV("Enabled") == "1");
    { CString r=GetNV("IntervalSec");   if (!r.empty()) { unsigned v=r.ToUInt(); if (v>=5 && v<=300) IntervalSec=v; } }
    { CString r=GetNV("IdleGapSec");    if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=30)          IdleGapSec=v; } }
    { CString r=GetNV("StartDelaySec"); if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=600)         StartDelaySec=v; } }
    { CString r=GetNV("JitterSec");     if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=10)          JitterSec=v; } }
    { CString r=GetNV("MinGapSec");     if (!r.empty()) { unsigned v=r.ToUInt(); if (v<=30)          MinGapSec=v; } }
  }
  void SaveNV() {
    SetNV("Enabled",       Enabled ? "1" : "0");
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

  void ArmISON(unsigned delay) {
    if (!Enabled || TimerArmed) return;
    TimerArmed = true;
    AddTimer(new CISONOnceTimer(this, delay));
  }
  void RearmISON() {
    TimerArmed = false;
    unsigned next = IntervalSec + Rand0To(JitterSec);
    ArmISON(next);
  }

  void TryReclaim() {
    if (!Enabled) return;
    if (HavePrimary()) return;
    time_t now = std::time(nullptr);
    if (LastAttempt && now - LastAttempt < (time_t)MinGapSec) return; // dedupe
    LastAttempt = now;
    PutIRC("NICK " + Primary);
    AddTimer(new CRearmTimer(this, MinGapSec)); // harmless spacing guard
  }

 public:
  MODCONSTRUCTOR(CKeepNickInstant) {}

  bool OnLoad(const CString& sArgs, CString& sMessage) override {
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)(uintptr_t)this);
    LoadPrimary(sArgs.Trim_n());
    LoadNV();
    ArmISON(StartDelaySec); // join-safe start
    return true;
  }

  void OnIRCConnected() override {
    TimerArmed = false;
    LastAttempt = 0;
    ArmISON(StartDelaySec); // join-safe start
  }

  void OnIRCDisconnected() override {
    TimerArmed = false;
    LastAttempt = 0;
  }

  // Track user activity: any outbound line from your client through ZNC
  EModRet OnUserRaw(CString& /*line*/) override {
    LastUserActivity = std::time(nullptr);
    return CONTINUE;
  }

  // Handle ISON reply and visible NICK/QUIT (when you share a channel)
  EModRet OnRaw(CString& sLine) override {
    VCString v; sLine.Split(" ", v, false);
    if (v.size() < 2) return CONTINUE;
    const CString& cmd = v[1];

    // ISON result: 303 <me> :nick1 nick2 ...
    if (cmd == "303") {
      // Older ZNC API compat: trailing starts at token index 3
      CString trailing = sLine.Token(3, true);    // ":nick1 nick2 ..."
      trailing.TrimLeft(CString(":"));            // Trim leading ':'

      bool present = false;
      VCString names; trailing.Split(" ", names, false);
      for (const auto& n : names) {
        if (n.Equals(Primary, false)) { present = true; break; }
      }
      if (!present) TryReclaim();
      return CONTINUE;
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

    if (cmd == "enable")   { Enabled = true;  SaveNV(); PutModule("Enabled. Polling runs only when you don't own the nick."); if (!TimerArmed) ArmISON(IntervalSec); }
    else if (cmd == "disable"){ Enabled = false; SaveNV(); PutModule("Disabled. No ISON polls or reclaim attempts."); }
    else if (cmd == "setnick") {
      CString nick = sCmdLine.Token(1, true).Trim_n();
      if (nick.empty()) { PutModule("Usage: SetNick <nick> — Set & persist the primary nick to reclaim."); return; }
      Primary = nick; SaveNV(); PutModule("Primary nick set to: " + Primary);
    }
    else if (cmd == "interval") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v>=5 && v<=300) { IntervalSec=v; SaveNV(); PutModule("Interval set to " + CString(v) + "s (ISON frequency when you don't own the nick)."); }
      else PutModule("Usage: Interval <seconds 5-300> — Default 5s.");
    }
    else if (cmd == "idlegap") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=30) { IdleGapSec=v; SaveNV(); PutModule("IdleGap set to " + CString(v) + "s. Skip a poll if you've sent anything in the last IdleGap seconds."); }
      else PutModule("Usage: IdleGap <seconds 0-30> — Default 2s.");
    }
    else if (cmd == "startdelay") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=600) { StartDelaySec=v; SaveNV(); PutModule("StartDelay set to " + CString(v) + "s. First ISON after connect (join-safe). Effective next connect."); }
      else PutModule("Usage: StartDelay <seconds 0-600> — Default 90s.");
    }
    else if (cmd == "jitter") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=10) { JitterSec=v; SaveNV(); PutModule("Jitter set to 0.." + CString(v) + "s. Random extra wait added to each poll."); }
      else PutModule("Usage: Jitter <seconds 0-10> — Default 0s.");
    }
    else if (cmd == "mingap") {
      unsigned v = sCmdLine.Token(1).ToUInt();
      if (v<=30) { MinGapSec=v; SaveNV(); PutModule("MinGap set to " + CString(v) + "s. Minimum spacing between NICK attempts (dedupe)."); }
      else PutModule("Usage: MinGap <seconds 0-30> — Default 3s.");
    }
    else if (cmd == "poke") {
      LastUserActivity = 0; // ensure it's considered idle
      ArmISON(0);
      PutModule("Poke: scheduling an immediate ISON once.");
    }
    else if (cmd == "show" || cmd.empty() || cmd == "help") {
      CString cur = GetNetwork() ? GetNetwork()->GetIRCNick().GetNick() : "<none>";
      PutModule("keepnick_instant — ISON-only nick reclaim (idle-aware & join-safe).");
      PutModule("Version: 1.2.0");
      PutModule("Current state:");
      PutModule("  Status: " + CString(Enabled ? "ENABLED" : "DISABLED"));
      PutModule("  Primary: " + Primary + "   |   Current: " + cur);
      PutModule("  Interval: " + CString(IntervalSec) + "s   (default 5s)");
      PutModule("  IdleGap: " + CString(IdleGapSec) + "s     (default 2s)");
      PutModule("  StartDelay: " + CString(StartDelaySec) + "s (default 90s; first poll after connect)");
      PutModule("  Jitter: 0.." + CString(JitterSec) + "s      (default 0s; random extra wait)");
      PutModule("  MinGap: " + CString(MinGapSec) + "s        (default 3s; dedupe NICK attempts)");
      PutModule("Commands:");
      PutModule("  Enable | Disable");
      PutModule("  SetNick <nick>            — Set primary nick (persisted).");
      PutModule("  Interval <5-300>          — ISON interval when you don't own the nick (default 5s).");
      PutModule("  IdleGap <0-30>            — Skip poll if active in last N seconds (default 2s).");
      PutModule("  StartDelay <0-600>        — First poll after connect (default 90s).");
      PutModule("  Jitter <0-10>             — Add 0..J seconds to each poll (default 0s).");
      PutModule("  MinGap <0-30>             — Minimum spacing between NICK attempts (default 3s).");
      PutModule("  Poke                      — Run one immediate ISON now (ignores StartDelay, still idle-aware).");
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

  if (!m->Enabled) return;
  if (m->HavePrimary()) { m->RearmISON(); return; }

  // Idle-aware: skip if you were active recently
  time_t now = std::time(nullptr);
  if (m->LastUserActivity && (now - m->LastUserActivity) < (time_t)m->IdleGapSec) {
    m->RearmISON(); return;
  }

  // Send single-nick ISON
  m->PutIRC("ISON " + m->Primary);
  m->RearmISON();
}

void CRearmTimer::RunJob() {
  // spacing guard; no action needed
}

template<> void TModInfo<CKeepNickInstant>(CModInfo& Info) {
  Info.SetHasArgs(true);
  Info.SetDescription("ISON-only, idle-aware, join-safe nick reclaim for ZNC");
}
NETWORKMODULEDEFS(CKeepNickInstant, "ISON-only keepnick (instant-ish)")

