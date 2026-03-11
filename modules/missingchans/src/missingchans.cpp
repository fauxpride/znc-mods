// missingchans.cpp — ZNC 1.9.1 compatible
// Build: znc-buildmod missingchans.cpp
//
// Changes vs r5:
// - Fixes cases where self-WHOIS DOES include +s/+p channels but module still misses them:
//   * Case-insensitive channel comparisons (avoids #Chan vs #chan false "missing")
//   * More robust parsing of 319 by concatenating params[2..end]
// - Keeps 443 fallback ("already on channel") as additional server-truth
// - Bumps build marker to r6

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>
#include <znc/IRCSock.h>
#include <znc/Chan.h>
#include <znc/User.h>

#include <set>
#include <vector>
#include <cctype>

#define MISSINGCHANS_BUILD "2026-02-11+r6 (robust 319 + case-insensitive chans + 443 fallback)"

// Case-insensitive ordering for CString (good enough for typical channel names)
struct CStringCI {
    bool operator()(const CString& a, const CString& b) const {
        return a.AsLower() < b.AsLower();
    }
};

class CMissingChansMod;

// Timers declared up front; constructors defined after CMissingChansMod is complete
class CRunTimer : public CTimer {
public:
    CRunTimer(CMissingChansMod* pMod, unsigned int uDelaySec, unsigned long long uGen, bool bResetAttempts, const CString& sDesc);
    void RunJob() override;

private:
    CMissingChansMod* m_pMod;
    unsigned long long m_uGen;
    bool m_bResetAttempts;
};

class CJoinAttemptTimer : public CTimer {
public:
    CJoinAttemptTimer(CMissingChansMod* pMod, unsigned int uDelaySec, unsigned int uAttempt, unsigned long long uGen, const CString& sDesc);
    void RunJob() override;

private:
    CMissingChansMod* m_pMod;
    unsigned int m_uAttempt;
    unsigned long long m_uGen;
};

class CMissingChansMod : public CModule {
public:
    MODCONSTRUCTOR(CMissingChansMod) {
        AddHelpCommand();

        m_uDelaySec = 300;
        m_bJoinMissing = false;

        m_sExpectedMode = "all";

        m_bRetryPerform = false;
        m_uRetries = 3;
        m_uRetryStepSec = 20;

        m_sStopPerformOn.clear();
        m_bPerformSuppressed = false;

        m_bWaitingWhois = false;
        m_uAttempt = 0;
        m_uGen = 0;

        m_bHaveLastAttemptMissing = false;
        m_bLastAttemptTriggeredPerform = false;
    }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        (void)sArgs;

        CString s;

        s = GetNV("delay");
        if (!s.empty()) {
            unsigned int v = s.ToUInt();
            if (v >= 1) m_uDelaySec = v;
        }

        s = GetNV("joinmissing");
        if (!s.empty()) m_bJoinMissing = ToBool(s);

        s = GetNV("expectedmode");
        if (!s.empty()) m_sExpectedMode = NormalizeExpectedMode(s);

        s = GetNV("retryperform");
        if (!s.empty()) m_bRetryPerform = ToBool(s);

        s = GetNV("retries");
        if (!s.empty()) {
            unsigned int v = s.ToUInt();
            if (v >= 1) m_uRetries = v;
        }

        s = GetNV("retrystep");
        if (!s.empty()) {
            unsigned int v = s.ToUInt();
            m_uRetryStepSec = v;
        }

        s = GetNV("stopperformon");
        if (!s.empty()) {
            s = s.Trim_n();
            if (s.AsLower() == "off" || s.AsLower() == "none" || s == "-") {
                m_sStopPerformOn.clear();
            } else {
                m_sStopPerformOn = s;
            }
        }

        sMessage = CString("missingchans loaded. Build: ") + MISSINGCHANS_BUILD;
        return true;
    }

    void OnIRCConnected() override {
        const unsigned long long gen = NextGen();
        PutModule("IRC connected. Scheduling verification in " + CString(m_uDelaySec) + "s.");
        AddTimer(new CRunTimer(this, m_uDelaySec, gen, true, "missingchans delayed run"));
    }

    void OnIRCDisconnected() override {
        NextGen();
        ResetVolatileState();
    }

    void OnClientAttached() override {
        if (!m_sAttachNotice.empty()) {
            PutModule(m_sAttachNotice);
            m_sAttachNotice.clear();
        }
    }

    void OnModCommand(const CString& sCommand) override {
        CString cmd = sCommand.Token(0).AsLower();
        CString rest = sCommand.Token(1, true);

        if (cmd.empty() || cmd == "help" || cmd == "h" || cmd == "?") { PrintHelp(); return; }
        if (cmd == "version") { PutModule(CString("missingchans build: ") + MISSINGCHANS_BUILD); return; }
        if (cmd == "status") { PrintStatus(); return; }
        if (cmd == "show") { ShowExpected(); return; }
        if (cmd == "run") { StartCheck(true, 0, true); return; }
        if (cmd == "set") { HandleSet(rest); return; }

        PutModule("Unknown command. Try: HELP");
    }

    EModRet OnNumericMessage(CNumericMessage& msg) override {
        const unsigned int code = msg.GetCode();

        // 443 <me> <user> <channel> :is already on channel
        // Robustly extract channel (don’t assume fixed index), and only accept it if relevant to expected/missing.
        if (code == 443) {
            CString chan;
            const size_t n = msg.GetParams().size();
            for (size_t i = 0; i < n; ++i) {
                CString p = msg.GetParam((unsigned int)i);
                if (!p.empty() && p[0] == ':') p.erase(0, 1);
                CString c = StripPrefix(p);
                if (!c.empty() && (c[0] == '#' || c[0] == '&' || c[0] == '!' || c[0] == '+')) {
                    chan = c;
                    break;
                }
            }

            if (!chan.empty()) {
                // Only learn channels we care about (avoids polluting verified set)
                if (m_expected.find(chan) != m_expected.end() || m_lastAttemptMissing.find(chan) != m_lastAttemptMissing.end()) {
                    const bool wasNew = (m_verifiedJoined.insert(chan).second);
                    if (wasNew) {
                        PutModule(CString("Server confirms you're already on ") + chan +
                                  " (443). Treating it as joined (useful if WHOIS is incomplete).");
                    }
                }
            }
        }

        // WHOIS parsing
        if (!m_bWaitingWhois) return CONTINUE;

        if (code != 319 && code != 318 && code != 401) return CONTINUE;
        if (msg.GetParams().size() < 2) return CONTINUE;

        const CString target = msg.GetParam(1).AsLower();
        if (target != m_sWhoisTargetLower) return CONTINUE;

        if (code == 401) {
            m_bWaitingWhois = false;
            PutModule("WHOIS failed (401). Cannot verify channel membership right now.");
            return CONTINUE;
        }

        if (code == 319) {
            // Robust: some parsers might split the channel list into multiple params.
            // Join params[2..end] into a single string, then split by spaces.
            const size_t n = msg.GetParams().size();
            if (n >= 3) {
                CString chans;
                for (size_t i = 2; i < n; ++i) {
                    CString p = msg.GetParam((unsigned int)i);
                    if (!p.empty() && p[0] == ':') p.erase(0, 1);
                    if (!chans.empty()) chans += " ";
                    chans += p;
                }

                VCString v;
                chans.Split(" ", v, false);
                for (CString tok : v) {
                    if (!tok.empty() && tok[0] == ':') tok.erase(0, 1);
                    CString c = StripPrefix(tok);
                    if (!c.empty()) m_actual.insert(c);
                }
            }
            return CONTINUE;
        }

        if (code == 318) {
            m_bWaitingWhois = false;
            FinishCheck();
            return CONTINUE;
        }

        return CONTINUE;
    }

    void TimerStartCheck(unsigned long long gen, bool resetAttempts) {
        StartCheck(false, gen, resetAttempts);
    }

    void TimerJoinAttempt(unsigned int attempt, unsigned long long gen) {
        DoJoinAttempt(attempt, gen);
    }

private:
    unsigned int m_uDelaySec;
    bool m_bJoinMissing;

    CString m_sExpectedMode;

    bool m_bRetryPerform;
    unsigned int m_uRetries;
    unsigned int m_uRetryStepSec;

    CString m_sStopPerformOn;
    bool m_bPerformSuppressed;

    bool m_bWaitingWhois;
    CString m_sWhoisTarget;
    CString m_sWhoisTargetLower;

    std::set<CString, CStringCI> m_expected;
    std::set<CString, CStringCI> m_actual;
    std::set<CString, CStringCI> m_verifiedJoined;
    std::set<CString, CStringCI> m_missing;

    unsigned int m_uAttempt;
    unsigned long long m_uGen;

    CString m_sAttachNotice;

    std::set<CString, CStringCI> m_lastAttemptMissing;
    bool m_bHaveLastAttemptMissing;
    bool m_bLastAttemptTriggeredPerform;
    CString m_sLastPerformSource;

private:
    void ResetVolatileState() {
        m_bWaitingWhois = false;
        m_expected.clear();
        m_actual.clear();
        m_verifiedJoined.clear();
        m_missing.clear();
        m_sWhoisTarget.clear();
        m_sWhoisTargetLower.clear();
        m_uAttempt = 0;

        m_lastAttemptMissing.clear();
        m_bHaveLastAttemptMissing = false;
        m_bLastAttemptTriggeredPerform = false;
        m_sLastPerformSource.clear();
        m_sAttachNotice.clear();

        m_bPerformSuppressed = false;
    }

    unsigned long long NextGen() {
        m_uGen++;
        if (m_uGen == 0) m_uGen = 1;
        return m_uGen;
    }

    static bool ToBool(const CString& sIn) {
        CString s = sIn.AsLower();
        return (s == "1" || s == "yes" || s == "true" || s == "on" || s == "enable" || s == "enabled");
    }

    static CString NormalizeExpectedMode(const CString& in) {
        CString s = in.AsLower();
        if (s == "all" || s == "everything") return "all";
        if (s == "config" || s == "cfg") return "config";
        if (s == "enabled" || s == "autojoin") return "enabled";
        return "all";
    }

    static CString StripPrefix(CString s) {
        while (!s.empty()) {
            const char c = s[0];
            if (c == '#' || c == '&' || c == '+' || c == '!') break;
            s.erase(0, 1);
        }
        return s;
    }

    static CString TrimSpaces(CString s) {
        while (!s.empty() && std::isspace((unsigned char)s[0])) s.erase(0, 1);
        while (!s.empty() && std::isspace((unsigned char)s[s.size() - 1])) s.erase(s.size() - 1, 1);
        return s;
    }

    void BuildExpected() {
        m_expected.clear();
        if (!GetNetwork()) return;

        const std::vector<CChan*>& v = GetNetwork()->GetChans();
        for (CChan* pChan : v) {
            if (!pChan) continue;

            const CString chan = pChan->GetName();
            if (chan.empty()) continue;

            if (m_sExpectedMode == "all") {
                m_expected.insert(chan);
                continue;
            }

            if (m_sExpectedMode == "config") {
                if (pChan->InConfig()) m_expected.insert(chan);
                continue;
            }

            if (pChan->InConfig() && !pChan->IsDisabled()) {
                m_expected.insert(chan);
            }
        }
    }

    void StartCheck(bool bManual, unsigned long long genFromTimer, bool resetAttempts) {
        if (!GetNetwork() || !GetNetwork()->GetIRCSock()) {
            PutModule("Not connected to IRC right now.");
            return;
        }

        if (genFromTimer == 0) {
            NextGen();
        } else {
            if (genFromTimer != m_uGen) return;
        }

        if (resetAttempts) {
            m_uAttempt = 0;
            m_bHaveLastAttemptMissing = false;
            m_bLastAttemptTriggeredPerform = false;
            m_sLastPerformSource.clear();
            m_bPerformSuppressed = false;

            // Clear verification cache per new cycle
            m_verifiedJoined.clear();
        }

        m_actual.clear();
        m_missing.clear();

        BuildExpected();

        m_sWhoisTarget = GetNetwork()->GetIRCNick().GetNick();
        if (m_sWhoisTarget.empty()) {
            PutModule("Cannot determine current nick for WHOIS.");
            return;
        }
        m_sWhoisTargetLower = m_sWhoisTarget.AsLower();

        m_bWaitingWhois = true;

        if (bManual) {
            PutModule(CString("Verifying channel membership on server via WHOIS ") + m_sWhoisTarget + ".");
        }

        PutIRC(CString("WHOIS ") + m_sWhoisTarget);
    }

    bool IsJoinedServerTruth(const CString& chan) const {
        return (m_actual.find(chan) != m_actual.end()) || (m_verifiedJoined.find(chan) != m_verifiedJoined.end());
    }

    void MaybeSuppressPerform() {
        if (m_bPerformSuppressed) return;
        if (m_sStopPerformOn.empty()) return;

        if (IsJoinedServerTruth(m_sStopPerformOn)) {
            m_bPerformSuppressed = true;
            CString how = (m_actual.find(m_sStopPerformOn) != m_actual.end()) ? "WHOIS" : "443 verification";
            PutModule(CString("StopPerformOn triggered: '") + m_sStopPerformOn +
                      "' is joined on the server (" + how + "). Future attempts will NOT run perform Execute.");
        }
    }

    void FinishCheck() {
        // Missing = expected - (WHOIS_actual U verified_joined)
        for (const CString& e : m_expected) {
            if (!IsJoinedServerTruth(e)) m_missing.insert(e);
        }

        MaybeSuppressPerform();

        CTable t;
        t.AddColumn("Group");
        t.AddColumn("Channel");

        for (const CString& e : m_expected) { t.AddRow(); t.SetCell("Group", "expected"); t.SetCell("Channel", e); }
        for (const CString& a : m_actual)    { t.AddRow(); t.SetCell("Group", "actual");   t.SetCell("Channel", a); }
        for (const CString& v : m_verifiedJoined) {
            if (m_expected.find(v) != m_expected.end()) {
                t.AddRow(); t.SetCell("Group", "verified"); t.SetCell("Channel", v);
            }
        }
        for (const CString& m : m_missing)   { t.AddRow(); t.SetCell("Group", "missing");  t.SetCell("Channel", m); }

        PutModule(t);

        if (m_missing.empty()) {
            PutModule("✔ All expected channels appear joined on the server.");
            return;
        }

        PutModule(CString("✖ Missing channels (server): ") + CString((unsigned int)m_missing.size()) + ".");

        if (m_bJoinMissing) {
            ScheduleNextAttempt();
        } else {
            PutModule("JoinMissing is OFF. Enable with: SET joinmissing on");
        }
    }

    void ScheduleNextAttempt() {
        if (m_missing.empty()) return;

        if (m_uRetries < 1) {
            PutModule("Retries is < 1; not attempting joins.");
            return;
        }

        if (m_uAttempt >= m_uRetries) {
            PutModule("Reached maximum join attempts; stopping.");
            return;
        }

        m_uAttempt++;
        const unsigned int waitSec = m_uRetryStepSec * m_uAttempt;

        PutModule(CString("Scheduling join attempt ") + CString(m_uAttempt) + "/" + CString(m_uRetries) +
                  " in " + CString(waitSec) + "s (RetryStep=" + CString(m_uRetryStepSec) + ").");

        AddTimer(new CJoinAttemptTimer(this, waitSec, m_uAttempt, m_uGen, "missingchans join attempt"));
    }

    CModule* FindPerformModule(CString& outWhere) {
        outWhere.clear();

        if (GetNetwork()) {
            CModule* p = GetNetwork()->GetModules().FindModule("perform");
            if (p) { outWhere = "network"; return p; }
        }

        if (GetUser()) {
            CModule* p = GetUser()->GetModules().FindModule("perform");
            if (p) { outWhere = "user"; return p; }
        }

        return nullptr;
    }

    bool ExecutePerformNow() {
        if (m_bPerformSuppressed) {
            PutModule(CString("RetryPerform is ON, but perform Execute is suppressed (StopPerformOn='") +
                      m_sStopPerformOn + "' is joined).");
            return false;
        }

        CString where;
        CModule* pPerf = FindPerformModule(where);
        if (!pPerf) {
            PutModule("RetryPerform is ON, but perform module is not loaded (user or network).");
            return false;
        }

        PutModule(CString("RetryPerform is ON: triggering perform Execute (") + where + " module).");
        pPerf->OnModCommand("Execute");

        m_bLastAttemptTriggeredPerform = true;
        m_sLastPerformSource = where;
        return true;
    }

    void DoJoinAttempt(unsigned int attempt, unsigned long long gen) {
        if (gen != m_uGen) return;

        if (!GetNetwork() || !GetNetwork()->GetIRCSock()) {
            PutModule("Not connected to IRC; cannot join.");
            return;
        }

        if (m_missing.empty()) {
            PutModule("No missing channels remain; skipping join attempt.");
            return;
        }

        m_lastAttemptMissing = m_missing;
        m_bHaveLastAttemptMissing = true;
        m_bLastAttemptTriggeredPerform = false;
        m_sLastPerformSource.clear();

        if (m_bRetryPerform) {
            ExecutePerformNow();
        }

        // Send JOIN for missing list; 443 replies will populate verifiedJoined if applicable
        for (const CString& chan : m_missing) {
            CString key;
            if (GetNetwork()) {
                CChan* pChan = GetNetwork()->FindChan(chan);
                if (pChan) key = pChan->GetKey();
            }

            CString line = "JOIN " + chan;
            if (!key.empty()) line += " " + key;

            PutIRC(line);
        }

        PutModule(CString("Attempt ") + CString(attempt) + "/" + CString(m_uRetries) +
                  ": sent JOIN for " + CString((unsigned int)m_missing.size()) + " missing channel(s).");

        if (!(GetUser() && GetUser()->IsUserAttached())) {
            m_sAttachNotice = "Attempt sent JOINs for missing channels (see module output for details).";
        }

        // Recheck shortly after JOIN burst
        AddTimer(new CRunTimer(this, 3, m_uGen, false, "missingchans post-join recheck"));
    }

    void ShowExpected() {
        BuildExpected();

        CTable t;
        t.AddColumn("Type");
        t.AddColumn("Channel");

        for (const CString& e : m_expected) {
            t.AddRow(); t.SetCell("Type", "expected"); t.SetCell("Channel", e);
        }

        PutModule(t);
        PutModule("Tip: RUN to verify against the server now.");
    }

    void PrintStatus() {
        CTable t;
        t.AddColumn("Setting");
        t.AddColumn("Value");

        t.AddRow(); t.SetCell("Setting", "Delay");        t.SetCell("Value", CString(m_uDelaySec) + "s");
        t.AddRow(); t.SetCell("Setting", "JoinMissing");  t.SetCell("Value", m_bJoinMissing ? "ON" : "OFF");
        t.AddRow(); t.SetCell("Setting", "ExpectedMode"); t.SetCell("Value", m_sExpectedMode);
        t.AddRow(); t.SetCell("Setting", "RetryPerform"); t.SetCell("Value", m_bRetryPerform ? "ON" : "OFF");
        t.AddRow(); t.SetCell("Setting", "Retries");      t.SetCell("Value", CString(m_uRetries));
        t.AddRow(); t.SetCell("Setting", "RetryStep");    t.SetCell("Value", CString(m_uRetryStepSec) + "s");

        CString sp = m_sStopPerformOn.empty() ? "off" : m_sStopPerformOn;
        t.AddRow(); t.SetCell("Setting", "StopPerformOn"); t.SetCell("Value", sp);
        t.AddRow(); t.SetCell("Setting", "PerformSuppressed(this run)");
        t.SetCell("Value", m_bPerformSuppressed ? "yes" : "no");

        t.AddRow(); t.SetCell("Setting", "VerifiedJoined(count)");
        t.SetCell("Value", CString((unsigned int)m_verifiedJoined.size()));

        PutModule(t);
    }

    void HandleSet(const CString& sRestIn) {
        CString sRest = TrimSpaces(sRestIn);

        CString key = sRest.Token(0).AsLower();
        CString val = TrimSpaces(sRest.Token(1, true));

        if (key.empty()) {
            PutModule("Usage: SET <delay|joinmissing|expectedmode|retryperform|retries|retrystep|stopperformon> <value>");
            return;
        }

        if (key == "delay") {
            unsigned int v = val.ToUInt();
            if (v < 1) { PutModule("Delay must be >= 1."); return; }
            m_uDelaySec = v;
            SetNV("delay", CString(v));
            PutModule(CString("OK. Delay set to ") + CString(v) + "s.");
            return;
        }

        if (key == "joinmissing") {
            m_bJoinMissing = ToBool(val);
            SetNV("joinmissing", m_bJoinMissing ? "1" : "0");
            PutModule(CString("OK. JoinMissing is now ") + (m_bJoinMissing ? "ON." : "OFF."));
            return;
        }

        if (key == "expectedmode") {
            CString m = NormalizeExpectedMode(val);
            m_sExpectedMode = m;
            SetNV("expectedmode", m);
            PutModule(CString("OK. ExpectedMode is now '") + m + "'.");
            return;
        }

        if (key == "retryperform") {
            m_bRetryPerform = ToBool(val);
            SetNV("retryperform", m_bRetryPerform ? "1" : "0");
            PutModule(CString("OK. RetryPerform is now ") + (m_bRetryPerform ? "ON." : "OFF."));
            return;
        }

        if (key == "retries") {
            unsigned int v = val.ToUInt();
            if (v < 1) { PutModule("Retries must be >= 1 (total attempts)."); return; }
            m_uRetries = v;
            SetNV("retries", CString(v));
            PutModule(CString("OK. Retries set to ") + CString(v) + ".");
            return;
        }

        if (key == "retrystep") {
            unsigned int v = val.ToUInt();
            m_uRetryStepSec = v;
            SetNV("retrystep", CString(v));
            PutModule(CString("OK. RetryStep set to ") + CString(v) + "s.");
            return;
        }

        if (key == "stopperformon") {
            CString v = val.Trim_n();
            CString vl = v.AsLower();
            if (v.empty() || vl == "off" || vl == "none" || v == "-") {
                m_sStopPerformOn.clear();
                SetNV("stopperformon", "off");
                PutModule("OK. StopPerformOn is now OFF.");
            } else {
                m_sStopPerformOn = v;
                SetNV("stopperformon", v);
                PutModule(CString("OK. StopPerformOn set to '") + v + "'. If this channel is joined, perform Execute will be suppressed on subsequent attempts.");
            }
            return;
        }

        PutModule("Unknown SET key. Try: HELP");
    }

    void PrintHelp() {
        PutModule("missingchans — verify expected channels vs server WHOIS, optionally rejoin missing ones.");
        PutModule(CString("Build: ") + MISSINGCHANS_BUILD);
        PutModule(" ");
        PutModule("Notes:");
        PutModule("  - This build compares channel names case-insensitively and parses WHOIS 319 robustly.");
        PutModule("  - 443 (already on channel) remains as a fallback verification signal.");
        PutModule(" ");
        PutModule("Key settings:");
        PutModule("  SET expectedmode <all|config|enabled>");
        PutModule("  SET joinmissing <on|off>");
        PutModule("  SET retryperform <on|off>         (uses perform Execute, unless suppressed)");
        PutModule("  SET retries <N>");
        PutModule("  SET retrystep <seconds>           (attempt i waits i*retrystep)");
        PutModule("  SET stopperformon <#chan|off>     (sentinel: if joined, suppress perform Execute in later attempts)");
        PutModule(" ");
        PutModule("Commands:");
        PutModule("  RUN / SHOW / STATUS / VERSION / HELP");
    }
};

//
// Timer definitions (AFTER CMissingChansMod is complete)
//
CRunTimer::CRunTimer(CMissingChansMod* pMod, unsigned int uDelaySec, unsigned long long uGen, bool bResetAttempts, const CString& sDesc)
    : CTimer(static_cast<CModule*>(pMod), uDelaySec, 1, "missingchans_run", sDesc),
      m_pMod(pMod), m_uGen(uGen), m_bResetAttempts(bResetAttempts) {}

void CRunTimer::RunJob() {
    if (!m_pMod) return;
    m_pMod->TimerStartCheck(m_uGen, m_bResetAttempts);
}

CJoinAttemptTimer::CJoinAttemptTimer(CMissingChansMod* pMod, unsigned int uDelaySec, unsigned int uAttempt, unsigned long long uGen, const CString& sDesc)
    : CTimer(static_cast<CModule*>(pMod), uDelaySec, 1, "missingchans_join", sDesc),
      m_pMod(pMod), m_uAttempt(uAttempt), m_uGen(uGen) {}

void CJoinAttemptTimer::RunJob() {
    if (!m_pMod) return;
    m_pMod->TimerJoinAttempt(m_uAttempt, m_uGen);
}

MODULEDEFS(CMissingChansMod, "Verify/join missing channels by comparing expected list vs WHOIS (with retry + perform support).")
