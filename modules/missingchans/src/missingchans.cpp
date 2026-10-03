// missingchans.cpp — ZNC 1.10.3; see ../CHANGELOG.md and ../TESTING.md.
// Build: znc-buildmod missingchans.cpp
// r12: in-place UpdateMod through a resident loader; r11/r10 behavior retained.

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>
#include <znc/IRCSock.h>
#include <znc/Chan.h>
#include <znc/User.h>

#include <set>
#include <vector>
#include <deque>
#include <cctype>
#include <algorithm>
#include <limits>
#include <ctime>
#include <cerrno>
#include <cstring>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#define MISSINGCHANS_BUILD "2026-10-04+r12 (in-place UpdateMod; r11 debug status and r10 recovery fixes retained)"

// Interface between the resident loader and a privately loaded build (see the
// loader section at the end of this file). The layout is frozen: a resident
// r12 loader may load any later build, so fields may only ever be appended.
extern "C" {
struct MissingChansDirectEntryV1 {
    unsigned int uSize;         // sizeof() as compiled by the provider
    unsigned int uAbi;          // 1
    const char* pcBuild;        // MISSINGCHANS_BUILD of the provider
    CModInfo::ModLoader fpLoad; // creates an instance from the provider's code
    unsigned int (*fpLiveInstances)();
    void (*fpSetLoaderNote)(const char* pcNote, bool bWarning);
};
ZNC_EXPORT_LIB_EXPORT const MissingChansDirectEntryV1* MissingChansDirectEntry();
}

// Everything module-specific has internal linkage, independent of compiler
// visibility flags, so no other object can bind to it or collide with it.
namespace {

// Instances created from this copy of the code, and the loader's message for
// the next instance. Both are per loaded copy of the binary.
unsigned int g_uLiveInstances = 0;
CString g_sPendingLoaderNote;
bool g_bPendingLoaderWarning = false;

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

class CWhoisTimeout : public CTimer {
public:
    explicit CWhoisTimeout(CMissingChansMod* pMod);
    void RunJob() override;
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

        ++g_uLiveInstances;
    }

    ~CMissingChansMod() override { --g_uLiveInstances; }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        (void)sArgs;

        // Set by the loader immediately before this instance was created.
        m_sLoaderNote = g_sPendingLoaderNote.empty() ? CString("direct") : g_sPendingLoaderNote;
        const bool bLoaderWarning = g_bPendingLoaderWarning;
        g_sPendingLoaderNote.clear();
        g_bPendingLoaderWarning = false;

        CString s;

        s = GetNV("delay");
        if (!s.empty()) {
            unsigned int v = s.ToUInt();
            if (v >= 1) m_uDelaySec = v;
        }

        s = GetNV("debug");
        if (!s.empty()) m_bDebug = ToBool(s);

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
        if (bLoaderWarning) {
            // UpdateMod does not display OnLoad messages, so also tell clients.
            sMessage += " " + m_sLoaderNote;
            PutModule(m_sLoaderNote);
        }
        return true;
    }

    void OnIRCConnected() override {
        CancelTimers();
        ResetVolatileState();
        const unsigned long long gen = NextGen();
        m_sPhase = "initial delay";
        PutModule("IRC connected. Scheduling verification in " + CString(m_uDelaySec) + "s.");
        AddTimer(new CRunTimer(this, m_uDelaySec, gen, true, "missingchans delayed run"));
    }

    void OnIRCDisconnected() override {
        CancelTimers();
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
        if (cmd == "version") {
            PutModule(CString("missingchans build: ") + MISSINGCHANS_BUILD);
            PutModule("Loader: " + m_sLoaderNote);
            return;
        }
        if (cmd == "status") { PrintStatus(); return; }
        if (cmd == "show") { ShowExpected(); return; }
        if (cmd == "run") { StartCheck(true, 0, true); return; }
        if (cmd == "set") { HandleSet(rest); return; }

        PutModule("Unknown command. Try: HELP");
    }

    EModRet OnUserRawMessage(CMessage& msg) override {
        if (msg.GetCommand().Equals("WHOIS") && !msg.GetParams().empty()) {
            VCString targets;
            msg.GetParams().back().Split(",", targets, false);
            for (const CString& target : targets) {
                m_whoisOrigins.push_back({target.AsLower(), false});
            }
            if (m_whoisOrigins.size() > 128) {
                AbortWhois("WHOIS tracking limit reached; no repair attempted.");
            }
        }
        return CONTINUE;
    }

    EModRet OnRawMessage(CMessage& raw) override {
        // Observe numerics before core routing: route_replies can consume a
        // client's WHOIS before OnNumericMessage would see its terminator.
        if (raw.GetType() != CMessage::Type::Numeric) return CONTINUE;
        CNumericMessage& msg = raw.As<CNumericMessage>();
        const unsigned int code = msg.GetCode();

        // 443 names the user who is already present. It is not generally
        // returned for duplicate JOINs, and an INVITE may name someone else.
        if (code == 443 && msg.GetParams().size() >= 3 &&
            msg.GetParam(1).Equals(GetNetwork()->GetIRCNick().GetNick())) {
            const CString chan = msg.GetParam(2);
            if (m_expected.count(chan) || m_lastAttemptMissing.count(chan)) {
                m_verifiedJoined.insert(chan);
                m_departed.erase(chan);
                MaybeSuppressPerform();
            }
        }

        // Only a matching WHOIS target may consume an origin marker.
        const bool isWhoisRelated =
            (code == 311 || code == 312 || code == 313 || code == 317 ||
             code == 319 || code == 330 || code == 338 || code == 671 ||
             code == 318 || code == 401);

        if (!isWhoisRelated) {
            return CONTINUE;
        }

        // 401 may terminate WHOIS by itself, or be followed by an optional
        // 318. Retire it once and absorb that optional terminator without
        // consuming the next request. A fresh 311 starts a new reply batch.
        if (msg.GetParams().size() >= 2 && !m_sFailedWhoisTarget.empty() &&
            msg.GetParam(1).AsLower() == m_sFailedWhoisTarget) {
            if (code == 318) {
                const bool hide = m_bFailedWhoisOurs;
                m_sFailedWhoisTarget.clear();
                return hide ? HALT : CONTINUE;
            }
            if (code == 311) m_sFailedWhoisTarget.clear();
        }

        const bool matchesFront = msg.GetParams().size() >= 2 &&
            !m_whoisOrigins.empty() &&
            msg.GetParam(1).AsLower() == m_whoisOrigins.front().target;
        const bool isOursAtFront = matchesFront && m_whoisOrigins.front().ours;

        // Module's internal parsing — only when this numeric corresponds to
        // OUR request AND we're still waiting for the WHOIS to complete.
        if (isOursAtFront && m_bWaitingWhois && msg.GetParams().size() >= 2) {
            const CString target = msg.GetParam(1).AsLower();
            if (target == m_sWhoisTargetLower) {
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
                } else if (code == 318) {
                    m_bWaitingWhois = false;
                    RemTimer("missingchans_whois_timeout");
                    m_sPhase = "checked";
                    FinishCheck();
                } else if (code == 401) {
                    m_bWaitingWhois = false;
                    m_sPhase = "WHOIS failed";
                    m_sLastAction = "WHOIS failed (401); no repair attempted";
                    PutModule(m_sLastAction);

                }
            }
        }

        // Pop the request marker on end-of-whois (regardless of whether it
        // was ours or the user's).
        if ((code == 318 || code == 401) && matchesFront) {
            if (code == 401) {
                m_sFailedWhoisTarget = msg.GetParam(1).AsLower();
                m_bFailedWhoisOurs = isOursAtFront;
            }
            m_whoisOrigins.pop_front();
            if (m_whoisOrigins.empty()) m_bWhoisUnsafe = false;
            if (isOursAtFront) RemTimer("missingchans_whois_timeout");
        }

        // Suppress from attached clients only if this batch was ours.
        return isOursAtFront ? HALT : CONTINUE;
    }

    void OnJoinMessage(CJoinMessage& msg) override {
        if (!msg.GetNick().NickEquals(GetNetwork()->GetIRCNick().GetNick())) return;
        const CString chan = msg.GetParam(0);
        m_departed.erase(chan);
        m_missing.erase(chan);
        // ZNC sets IsOn before invoking this hook.
        MaybeSuppressPerform();
    }

    void OnPartMessage(CPartMessage& msg) override {
        if (msg.GetNick().NickEquals(GetNetwork()->GetIRCNick().GetNick()))
            ForgetJoined(msg.GetParam(0));
    }

    void OnKickMessage(CKickMessage& msg) override {
        if (msg.GetKickedNick().Equals(GetNetwork()->GetIRCNick().GetNick()))
            ForgetJoined(msg.GetParam(0));
    }

    void WhoisTimedOut() {
        AbortWhois("WHOIS timed out; no repair attempted. Wait for the old reply before RUN, or reconnect.");
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
    bool m_bDebug = false;

    CString m_sExpectedMode;

    bool m_bRetryPerform;
    unsigned int m_uRetries;
    unsigned int m_uRetryStepSec;

    CString m_sStopPerformOn;
    bool m_bPerformSuppressed;

    bool m_bWaitingWhois;
    CString m_sWhoisTarget;
    CString m_sWhoisTargetLower;

    // FIFO target-aware WHOIS origins. Aborted requests are drained visibly.
    struct WhoisOrigin { CString target; bool ours; };
    std::deque<WhoisOrigin> m_whoisOrigins;
    std::set<CString, CStringCI> m_departed;
    bool m_bWhoisUnsafe = false;
    CString m_sFailedWhoisTarget;
    bool m_bFailedWhoisOurs = false;
    CString m_sPhase = "idle";
    CString m_sLastAction = "none";
    CString m_sLastPerformAt = "never";
    unsigned int m_uPerformCount = 0;

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

    CString m_sLoaderNote;

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
        m_whoisOrigins.clear();
        m_bWhoisUnsafe = false;
        m_sFailedWhoisTarget.clear();
        m_bFailedWhoisOurs = false;

        m_lastAttemptMissing.clear();
        m_bHaveLastAttemptMissing = false;
        m_bLastAttemptTriggeredPerform = false;
        m_sLastPerformSource.clear();
        m_sAttachNotice.clear();

        m_bPerformSuppressed = false;
        m_departed.clear();
        m_sPhase = "idle";
        m_sLastAction = "none";
        m_sLastPerformAt = "never";
        m_uPerformCount = 0;
    }

    void CancelTimers() {
        RemTimer("missingchans_run");
        RemTimer("missingchans_join");
        RemTimer("missingchans_whois_timeout");
    }

    void AbortWhois(const CString& reason) {
        m_bWaitingWhois = false;
        m_bWhoisUnsafe = true;
        // Drain an abandoned reply visibly before accepting another check.
        // Otherwise a late 318 could complete a new check with an empty list.
        for (auto& origin : m_whoisOrigins) origin.ours = false;
        m_bFailedWhoisOurs = false;
        if (m_whoisOrigins.size() > 128) m_whoisOrigins.clear();
        m_actual.clear();
        m_missing.clear();
        CancelTimers();
        NextGen();
        m_sPhase = "verification aborted";
        m_sLastAction = reason;
        PutModule(reason);
    }

    void ForgetJoined(const CString& chan) {
        m_actual.erase(chan);
        m_verifiedJoined.erase(chan);
        m_departed.insert(chan);
        // Suppression stays latched for this cycle. No perpetual monitoring
        // or new retry sequence is started by a PART or KICK.
    }

    void RefreshMissing() {
        BuildExpected();
        m_missing.clear();
        for (const CString& chan : m_expected) {
            if (!IsKnownJoined(chan)) m_missing.insert(chan);
        }
    }

    CString MissingList() const {
        CString list;
        for (const CString& chan : m_missing) {
            if (!list.empty()) list += " ";
            list += chan;
        }
        return list.empty() ? CString("none") : list;
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

    // Use negotiated PREFIX and CHANTYPES, including unusual ranks such as
    // '!'. A symbol that is both a rank and a channel type is stripped only
    // if followed by another rank/channel prefix (e.g. +#chan vs +modeless).
    CString StripPrefix(CString token) const {
        const CIRCSock* sock = GetNetwork() ? GetNetwork()->GetIRCSock() : nullptr;
        const CString perms = sock ? sock->GetPerms() : CString("~&@%+");
        const CString types = sock ? sock->GetISupport("CHANTYPES", "#&!+") : CString("#&!+");
        while (token.size() >= 2 && perms.find(token[0]) != CString::npos) {
            if (types.find(token[0]) != CString::npos &&
                types.find(token[1]) == CString::npos && perms.find(token[1]) == CString::npos) break;
            token.erase(0, 1);
        }
        if (token.empty() || types.find(token[0]) == CString::npos) return CString();
        return token;
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
        if (!GetNetwork() || !GetNetwork()->IsIRCConnected()) {
            PutModule("Not connected to IRC right now.");
            return;
        }

        if (m_bWhoisUnsafe) {
            PutModule("Previous WHOIS is incomplete; wait for its reply or reconnect before RUN.");
            return;
        }
        if (m_bWaitingWhois || std::any_of(m_whoisOrigins.begin(), m_whoisOrigins.end(),
                [](const WhoisOrigin& origin) { return origin.ours; })) {
            PutModule("Verification already in progress; RUN did not reset the current cycle.");
            return;
        }
        if (genFromTimer == 0) {
            CancelTimers();
            NextGen();
        } else {
            if (genFromTimer != m_uGen) return;
        }

        if (resetAttempts) {
            m_uAttempt = 0;
            m_bHaveLastAttemptMissing = false;
            m_bLastAttemptTriggeredPerform = false;
            m_bPerformSuppressed = false;

            // Clear verification cache per new cycle
            m_verifiedJoined.clear();
        }

        m_actual.clear();
        m_missing.clear();
        m_departed.clear();

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

        // Mark this WHOIS as originating from the module BEFORE sending, so the
        // origin marker is in place by the time replies arrive.
        m_sPhase = "waiting for WHOIS";
        m_whoisOrigins.push_back({m_sWhoisTargetLower, true});
        RemTimer("missingchans_whois_timeout");
        AddTimer(new CWhoisTimeout(this));
        PutIRC(CString("WHOIS ") + m_sWhoisTarget);
    }

    bool IsKnownJoined(const CString& chan) const {
        if (m_departed.count(chan)) return false;
        const CChan* pChan = GetNetwork() ? GetNetwork()->FindChan(chan) : nullptr;
        return (pChan && pChan->IsOn()) || m_actual.count(chan) || m_verifiedJoined.count(chan);
    }

    void MaybeSuppressPerform() {
        if (m_bPerformSuppressed) return;
        if (m_sStopPerformOn.empty()) return;

        if (IsKnownJoined(m_sStopPerformOn)) {
            m_bPerformSuppressed = true;
            CString how = "live membership / WHOIS / self-443";
            PutModule(CString("StopPerformOn triggered: '") + m_sStopPerformOn +
                      "' is joined on the server (" + how + "). Future attempts will NOT run perform Execute.");
        }
    }

    void FinishCheck() {
        RefreshMissing();
        MaybeSuppressPerform();

        CTable t;
        t.AddColumn("Group");
        t.AddColumn("Channel");

        for (const CString& e : m_expected) { t.AddRow(); t.SetCell("Group", "expected"); t.SetCell("Channel", e); }
        for (const CString& a : m_actual)    { t.AddRow(); t.SetCell("Group", "actual");   t.SetCell("Channel", a); }
        for (const CString& e : m_expected) {
            const CChan* chan = GetNetwork()->FindChan(e);
            if (chan && chan->IsOn() && !m_actual.count(e)) {
                t.AddRow(); t.SetCell("Group", "live"); t.SetCell("Channel", e);
            }
        }
        for (const CString& v : m_verifiedJoined) {
            if (m_expected.find(v) != m_expected.end()) {
                t.AddRow(); t.SetCell("Group", "verified"); t.SetCell("Channel", v);
            }
        }
        for (const CString& m : m_missing)   { t.AddRow(); t.SetCell("Group", "missing");  t.SetCell("Channel", m); }

        PutModule(t);

        if (m_missing.empty()) {
            m_sPhase = "complete";
            m_sLastAction = "All expected channels joined; no repair needed";
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
            m_sPhase = "retries exhausted";
            PutModule("Reached maximum join attempts; stopping.");
            return;
        }

        m_uAttempt++;
        const unsigned long long wait = static_cast<unsigned long long>(m_uRetryStepSec) * m_uAttempt;
        const unsigned int waitSec = static_cast<unsigned int>(std::max(1ULL,
            std::min(wait, static_cast<unsigned long long>(std::numeric_limits<unsigned int>::max()))));
        m_sPhase = "retry scheduled";

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
        // Calling OnModCommand directly bypasses ZNC's usual context guards.
        // Restore every field, including null client context, on all exits.
        struct ContextGuard {
            CModule* mod;
            CIRCNetwork* network;
            CUser* user;
            CClient* client;
            explicit ContextGuard(CModule* m) : mod(m), network(m->GetNetwork()),
                user(m->GetUser()), client(m->GetClient()) {}
            ~ContextGuard() {
                mod->SetNetwork(network);
                mod->SetUser(user);
                mod->SetClient(client);
            }
        } context(pPerf);
        pPerf->SetNetwork(GetNetwork());
        pPerf->SetUser(GetUser());
        pPerf->SetClient(nullptr);
        CModCallProtector call(*pPerf);
        pPerf->OnModCommand("Execute");

        ++m_uPerformCount;
        const std::time_t now = std::time(nullptr);
        char stamp[32] = {};
        if (const std::tm* utc = std::gmtime(&now))
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", utc);
        m_sLastPerformAt = stamp;
        m_bLastAttemptTriggeredPerform = true;
        m_sLastPerformSource = where;
        return true;
    }

    void DoJoinAttempt(unsigned int attempt, unsigned long long gen) {
        if (gen != m_uGen) return;

        if (!GetNetwork() || !GetNetwork()->IsIRCConnected()) {
            PutModule("Not connected to IRC; cannot join.");
            return;
        }

        if (!m_bJoinMissing || attempt > m_uRetries) {
            m_sPhase = "retry cancelled";
            m_sLastAction = "Pending retry cancelled by current settings";
            PutModule(m_sLastAction);
            return;
        }
        RefreshMissing();
        MaybeSuppressPerform();
        if (m_missing.empty()) {
            m_sPhase = "complete";
            m_sLastAction = "No missing channels remain; skipped pending retry";
            PutModule("No missing channels remain; skipping join attempt.");
            return;
        }

        m_lastAttemptMissing = m_missing;
        m_sLastAction = "Attempt " + CString(attempt) + " on network '" +
            GetNetwork()->GetName() + "'; missing: " + MissingList();
        PutModule(m_sLastAction);
        m_sPhase = "post-join wait";
        m_bHaveLastAttemptMissing = true;
        m_bLastAttemptTriggeredPerform = false;

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

        // Presentation only: retain diagnostics even while hidden.
        if (m_bDebug) {
            auto row = [&](const CString& key, const CString& value) {
                t.AddRow(); t.SetCell("Setting", key); t.SetCell("Value", value);
            };
            row("Debug", "ON");
            unsigned int joined = 0;
            if (GetNetwork()) for (const CChan* chan : GetNetwork()->GetChans())
                if (chan && chan->IsOn()) ++joined;
            row("Network", GetNetwork() ? GetNetwork()->GetName() : CString("none"));
            row("Phase", m_sPhase);
            row("Attempt", CString(m_uAttempt));
            row("LiveJoined(count)", CString(joined));
            row("WhoisJoined(last snapshot)", CString(static_cast<unsigned int>(m_actual.size())));
            row("Missing(last check)", MissingList());
            row("LastAction", m_sLastAction);
            CString lastMissing;
            for (const CString& chan : m_lastAttemptMissing) {
                if (!lastMissing.empty()) lastMissing += " ";
                lastMissing += chan;
            }
            row("LastAttemptMissing", lastMissing.empty() ? CString("none") : lastMissing);
            row("LastAttemptTriggeredPerform", m_bLastAttemptTriggeredPerform ? "yes" : "no");
            row("PerformCalls(this connection)", CString(m_uPerformCount));
            row("LastPerformSource", m_sLastPerformSource.empty() ? CString("none") : m_sLastPerformSource);
            row("LastPerformAt", m_sLastPerformAt);
        }

        PutModule(t);
    }

    void HandleSet(const CString& sRestIn) {
        CString sRest = TrimSpaces(sRestIn);

        CString key = sRest.Token(0).AsLower();
        CString val = TrimSpaces(sRest.Token(1, true));

        if (key.empty()) {
            PutModule("Usage: SET <delay|joinmissing|expectedmode|retryperform|retries|retrystep|stopperformon|debug> <value>");
            return;
        }

        if (key == "debug") {
            m_bDebug = ToBool(val);
            SetNV("debug", m_bDebug ? "1" : "0");
            PutModule(CString("OK. Debug is now ") + (m_bDebug ? "ON." : "OFF."));
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
        PutModule("  - Live joined state and self JOIN/PART/KICK events supplement WHOIS.");
        PutModule("  - Self-targeted 443 is an optional fallback, not a duplicate-JOIN guarantee.");
        PutModule(" ");
        PutModule("Settings (configure via: SET <key> <value>):");
        PutModule("  delay <seconds>");
        PutModule("      Seconds to wait after IRC connect before the first verification cycle.");
        PutModule("      Minimum 1. Default 300.");
        PutModule("  joinmissing <on|off>");
        PutModule("      Whether to actually JOIN missing channels after verification. If OFF,");
        PutModule("      missing channels are only reported. Default OFF.");
        PutModule("  expectedmode <all|config|enabled>");
        PutModule("      Which channels count as 'expected':");
        PutModule("        all     - every channel known to this network (default).");
        PutModule("        config  - only channels present in znc.conf (InConfig=true).");
        PutModule("        enabled - channels in znc.conf that are not disabled.");
        PutModule("  retryperform <on|off>");
        PutModule("      If ON, the 'perform' module's Execute command is invoked before each");
        PutModule("      JOIN attempt (network-scope perform preferred over user-scope). Useful");
        PutModule("      when joins require services auth. Default OFF.");
        PutModule("  retries <N>");
        PutModule("      Maximum number of JOIN attempts per verification cycle. Minimum 1.");
        PutModule("      Default 3.");
        PutModule("  retrystep <seconds>");
        PutModule("      Backoff step between attempts. Attempt i waits (i * retrystep) seconds");
        PutModule("      before firing (minimum wait 1s). Default 20.");
        PutModule("  debug <on|off>");
        PutModule("      Show additional diagnostic fields in STATUS. Default OFF (r9 fields).");
        PutModule("      Presentation only; does not change recovery or clear recorded diagnostics.");
        PutModule("  stopperformon <#channel|off>");
        PutModule("      Sentinel channel. If it appears joined (live state, WHOIS or self-443), perform");
        PutModule("      Execute is suppressed for the rest of this cycle. Use 'off' (or 'none',");
        PutModule("      or '-') to clear. Default off.");
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
    SetName(""); // Do not let cancellation of a pending label delete this callback.
    m_pMod->TimerStartCheck(m_uGen, m_bResetAttempts);
}

CJoinAttemptTimer::CJoinAttemptTimer(CMissingChansMod* pMod, unsigned int uDelaySec, unsigned int uAttempt, unsigned long long uGen, const CString& sDesc)
    : CTimer(static_cast<CModule*>(pMod), uDelaySec, 1, "missingchans_join", sDesc),
      m_pMod(pMod), m_uAttempt(uAttempt), m_uGen(uGen) {}

void CJoinAttemptTimer::RunJob() {
    if (!m_pMod) return;
    SetName(""); // Do not let cancellation of a pending label delete this callback.
    m_pMod->TimerJoinAttempt(m_uAttempt, m_uGen);
}

CWhoisTimeout::CWhoisTimeout(CMissingChansMod* pMod)
    : CTimer(pMod, 30, 1, "missingchans_whois_timeout", "missingchans WHOIS watchdog") {}

void CWhoisTimeout::RunJob() {
    SetName("");
    static_cast<CMissingChansMod*>(GetModule())->WhoisTimedOut();
}

//
// Resident loader (r12).
//
// ZNC opens modules with RTLD_GLOBAL. Any module loaded later may bind its own
// references to weak C++ template code that this file was first to define.
// glibc then keeps this file mapped while that module is loaded, and
// UpdateMod's dlopen() of the same path returns the old, still-mapped code.
// Which modules do that depends on load order, and no source-level visibility
// setting can stop a -O0 build from exporting libstdc++ instantiations.
//
// The fix: ZNC's loader callback below never trusts that the mapped code is
// what is installed. It compares the installed file with the file this copy
// was mapped from. If they differ, it maps the installed file privately
// (RTLD_LOCAL, through a temporary hard link so glibc does not reuse the old
// mapping by name) and creates the instance from that code. Other objects
// cannot bind to a RTLD_LOCAL object (only STB_GNU_UNIQUE symbols could pin
// it, and tests/symbols.py rules those out), so private copies unload when
// closed; they are closed once their last instance is gone. The first loader
// to create an instance keeps itself resident (RTLD_NODELETE), so it is always
// the one ZNC reaches and its bookkeeping is never lost.
//

unsigned int LiveInstances() { return g_uLiveInstances; }

void SetLoaderNote(const char* pcNote, bool bWarning) {
    g_sPendingLoaderNote = pcNote ? pcNote : "";
    g_bPendingLoaderWarning = bWarning;
}

CModule* DirectLoad(ModHandle p, CUser* pUser, CIRCNetwork* pNetwork, const CString& sModName,
                    const CString& sDataPath, CModInfo::EModuleType eType) {
    return new CMissingChansMod(p, pUser, pNetwork, sModName, sDataPath, eType);
}

const char kSelfAnchor = 0; // any address inside this mapping, for dladdr()

struct LoadedBuild {
    void* pHandle = nullptr;
    dev_t dev = 0;
    ino_t ino = 0;
    const MissingChansDirectEntryV1* pDirect = nullptr; // r12 and later
    CModInfo::ModLoader fpLegacyLoad = nullptr;         // r11 and earlier
    CString sBuild;
};

struct LoaderState {
    CString sSelfPath;
    bool bSelfKnown = false;
    dev_t selfDev = 0;
    ino_t selfIno = 0;
    void* pSelfPin = nullptr;
    bool bPinAttempted = false;
    CString sPendingPath;         // module path ZNC resolved for this load
    unsigned long long uSeq = 0;  // temporary link names
    std::vector<LoadedBuild> vBuilds;
    int iLastUsed = -1;           // index in vBuilds; -1 means this copy
};

LoaderState InitialState() {
    LoaderState st;
    Dl_info info;
    if (dladdr(&kSelfAnchor, &info) && info.dli_fname) {
        st.sSelfPath = info.dli_fname;
        struct stat sb;
        if (stat(info.dli_fname, &sb) == 0) {
            st.bSelfKnown = true;
            st.selfDev = sb.st_dev;
            st.selfIno = sb.st_ino;
        }
    }
    return st;
}

// First used by FillModInfo, i.e. right after ZNC maps this file, so the
// recorded file identity is the one this code came from.
LoaderState& State() {
    static LoaderState state = InitialState();
    return state;
}

void PinSelf(LoaderState& st) {
    if (st.bPinAttempted || st.sSelfPath.empty()) return;
    st.bPinAttempted = true;
    st.pSelfPin = dlopen(st.sSelfPath.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
}

CString Errno(int err) { return CString(std::strerror(err)); }

// Close private copies that have no instances left, except the one at iKeep
// (-1: none). Builds older than r12 cannot report their instances and stay
// mapped until restart. Returns the new index of iKeep, or -1.
int ReleaseIdle(LoaderState& st, int iKeep) {
    std::vector<LoadedBuild> kept;
    int iNewKeep = -1, iNewLast = -1;
    for (int i = 0; i < static_cast<int>(st.vBuilds.size()); ++i) {
        const LoadedBuild& build = st.vBuilds[i];
        if (i != iKeep && build.pDirect && build.pDirect->fpLiveInstances() == 0) {
            dlclose(build.pHandle);
            continue;
        }
        if (i == iKeep) iNewKeep = static_cast<int>(kept.size());
        if (i == st.iLastUsed) iNewLast = static_cast<int>(kept.size());
        kept.push_back(build);
    }
    st.vBuilds.swap(kept);
    st.iLastUsed = iNewLast;
    return iNewKeep;
}

// Map the installed file privately. Returns the index in vBuilds, -1 for
// "it is this copy", or -2 with sProblem set.
int LoadPrivate(LoaderState& st, const CString& sPath, const struct stat& sb,
                CModInfo::EModuleType eType, CString& sProblem) {
    const CString::size_type slash = sPath.rfind('/');
    const CString sDir = slash == CString::npos ? CString(".") : sPath.substr(0, slash);
    CString sLink;
    for (int i = 0; i < 100; ++i) {
        sLink = sDir + "/.missingchans-load-" + CString(static_cast<unsigned int>(getpid())) + "-" +
                CString(++st.uSeq);
        if (link(sPath.c_str(), sLink.c_str()) == 0) break;
        const int err = errno;
        if (err != EEXIST) {
            sProblem = "cannot create a temporary link next to " + sPath + ": " + Errno(err);
            return -2;
        }
        sLink.clear();
    }
    if (sLink.empty()) {
        sProblem = "cannot create a unique temporary link next to " + sPath;
        return -2;
    }

    void* pHandle = dlopen(sLink.c_str(), RTLD_NOW | RTLD_LOCAL);
    const char* pcErr = pHandle ? nullptr : dlerror();
    const CString sErr = pcErr ? pcErr : "unknown dlopen error";
    unlink(sLink.c_str()); // the mapping stays valid without the name
    if (!pHandle) {
        sProblem = "cannot load " + sPath + ": " + sErr;
        return -2;
    }
    if (pHandle == st.pSelfPin) {
        dlclose(pHandle);
        return -1;
    }

    const CModuleEntry* (*fpEntry)() = nullptr;
    *reinterpret_cast<void**>(&fpEntry) = dlsym(pHandle, "ZNCModuleEntry");
    const CModuleEntry* pEntry = fpEntry ? fpEntry() : nullptr;
    if (!pEntry) {
        dlclose(pHandle);
        sProblem = sPath + " is not a ZNC module";
        return -2;
    }
    if (std::strcmp(pEntry->pcVersion, VERSION_STR) || std::strcmp(pEntry->pcVersionExtra, VERSION_EXTRA)) {
        sProblem = sPath + " is built for ZNC " + CString(pEntry->pcVersion) + pEntry->pcVersionExtra +
                   ", core is " + VERSION_STR + VERSION_EXTRA;
        dlclose(pHandle);
        return -2;
    }
    if (std::strcmp(pEntry->pcCompileOptions, ZNC_COMPILE_OPTIONS_STRING)) {
        sProblem = sPath + " is built with incompatible ZNC options";
        dlclose(pHandle);
        return -2;
    }

    LoadedBuild build;
    build.pHandle = pHandle;
    build.dev = sb.st_dev;
    build.ino = sb.st_ino;

    const MissingChansDirectEntryV1* (*fpDirect)() = nullptr;
    *reinterpret_cast<void**>(&fpDirect) = dlsym(pHandle, "MissingChansDirectEntry");
    if (fpDirect) {
        const MissingChansDirectEntryV1* pDirect = fpDirect();
        if (!pDirect || pDirect->uAbi < 1 || pDirect->uSize < sizeof(MissingChansDirectEntryV1) ||
            !pDirect->fpLoad || !pDirect->fpLiveInstances || !pDirect->fpSetLoaderNote) {
            dlclose(pHandle);
            sProblem = sPath + " has an unsupported loader interface";
            return -2;
        }
        build.pDirect = pDirect;
        build.sBuild = pDirect->pcBuild ? CString(pDirect->pcBuild).Token(0) : CString("unknown build");
    } else {
        // Builds before r12: use their own ZNC loader, which creates the
        // instance directly. Never call an r12+ loader this way (recursion).
        CModInfo info;
        info.SetName("missingchans");
        info.SetPath(sPath);
        pEntry->fpFillModInfo(info);
        if (!info.GetLoader() || !info.SupportsType(eType)) {
            dlclose(pHandle);
            sProblem = sPath + " cannot be loaded as this module type";
            return -2;
        }
        build.fpLegacyLoad = info.GetLoader();
        build.sBuild = "pre-r12 build";
    }
    st.vBuilds.push_back(build);
    return static_cast<int>(st.vBuilds.size()) - 1;
}

CModule* Instantiate(LoaderState& st, int iBuild, const CString& sNote, bool bWarning,
                     ModHandle p, CUser* pUser, CIRCNetwork* pNetwork, const CString& sModName,
                     const CString& sDataPath, CModInfo::EModuleType eType) {
    st.iLastUsed = iBuild;
    if (iBuild < 0) {
        SetLoaderNote(sNote.c_str(), bWarning);
        return DirectLoad(p, pUser, pNetwork, sModName, sDataPath, eType);
    }
    const LoadedBuild& build = st.vBuilds[iBuild];
    if (build.pDirect) {
        build.pDirect->fpSetLoaderNote(sNote.c_str(), bWarning);
        return build.pDirect->fpLoad(p, pUser, pNetwork, sModName, sDataPath, eType);
    }
    return build.fpLegacyLoad(p, pUser, pNetwork, sModName, sDataPath, eType);
}

// ZNC's loader callback. It must always return an instance: ZNC does not check.
CModule* ResidentLoad(ModHandle p, CUser* pUser, CIRCNetwork* pNetwork, const CString& sModName,
                      const CString& sDataPath, CModInfo::EModuleType eType) {
    LoaderState& st = State();
    PinSelf(st);
    const CString sPath = st.sPendingPath.empty() ? st.sSelfPath : st.sPendingPath;
    const CString sSelfBuild = CString(MISSINGCHANS_BUILD).Token(0);

    CString sProblem;
    struct stat sb;
    int iBuild = -2;
    if (sPath.empty()) {
        sProblem = "the module path is unknown";
    } else if (stat(sPath.c_str(), &sb) != 0) {
        sProblem = "cannot stat " + sPath + ": " + Errno(errno);
    } else if (st.bSelfKnown && sb.st_dev == st.selfDev && sb.st_ino == st.selfIno) {
        iBuild = -1;
    } else {
        for (size_t i = 0; i < st.vBuilds.size(); ++i) {
            if (st.vBuilds[i].dev == sb.st_dev && st.vBuilds[i].ino == sb.st_ino) {
                iBuild = static_cast<int>(i);
                break;
            }
        }
        if (iBuild == -2) iBuild = LoadPrivate(st, sPath, sb, eType, sProblem);
    }

    if (iBuild == -1) {
        ReleaseIdle(st, -1);
        return Instantiate(st, -1, "resident loader " + sSelfBuild + " (in-place UpdateMod enabled)", false,
                           p, pUser, pNetwork, sModName, sDataPath, eType);
    }
    if (iBuild >= 0) {
        iBuild = ReleaseIdle(st, iBuild);
        return Instantiate(st, iBuild, "updated in place by resident loader " + sSelfBuild, false,
                           p, pUser, pNetwork, sModName, sDataPath, eType);
    }

    // Keep the network covered by the code that ran before, and say so.
    const int iFallback = st.iLastUsed;
    const CString sRunning = iFallback >= 0 ? st.vBuilds[iFallback].sBuild : sSelfBuild;
    const CString sWarning = "WARNING: the installed missingchans.so was not loaded (" + sProblem +
                             "). Still running " + sRunning +
                             ". Fix the file and run UpdateMod again, or restart ZNC.";
    return Instantiate(st, iFallback, sWarning, true, p, pUser, pNetwork, sModName, sDataPath, eType);
}

void FillModInfo(CModInfo& Info) {
    State().sPendingPath = Info.GetPath();
    Info.SetDescription("Verify/join missing channels by comparing expected list vs WHOIS (with retry + perform support).");
    Info.SetDefaultType(CModInfo::NetworkModule);
    Info.AddType(CModInfo::NetworkModule);
    Info.SetLoader(ResidentLoad);
    TModInfo<CMissingChansMod>(Info);
}

} // namespace

// Same contract as ZNC's MODULEDEFS, with the resident loader as the loader.
extern "C" {
ZNC_EXPORT_LIB_EXPORT const CModuleEntry* ZNCModuleEntry();
ZNC_EXPORT_LIB_EXPORT const CModuleEntry* ZNCModuleEntry() {
    static const CModuleEntry ThisModule = {VERSION_STR, VERSION_EXTRA, ZNC_COMPILE_OPTIONS_STRING,
                                            FillModInfo};
    return &ThisModule;
}

ZNC_EXPORT_LIB_EXPORT const MissingChansDirectEntryV1* MissingChansDirectEntry() {
    static const MissingChansDirectEntryV1 Entry = {sizeof(MissingChansDirectEntryV1), 1, MISSINGCHANS_BUILD,
                                                   DirectLoad, LiveInstances, SetLoaderNote};
    return &Entry;
}
}
