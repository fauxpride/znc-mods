// ignore_drop.cpp — ZNC server-side ignore (Network module)
// Build:  znc-buildmod ignore_drop.cpp
// Load:   /msg *status LoadMod --type=network ignore_drop
// Help:   /msg *ignore_drop help
//
// Goals:
// - Low overhead: pre-lowered masks, fast paths, per-network detached check only when needed.
// - Semantics:
//     ALWAYS  -> live drop (attached/detached) + playback hide
//     DETACHED-> live drop only while this network is detached; playback hide is optional (off by default)
//
// Storage (per network):
//   NV "masks": one per line, "A|mask" or "D|mask"
//   NV "detached_playback": "0" or "1"

#include <znc/znc.h>
#include <znc/Modules.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/Chan.h>
#include <znc/Nick.h>
#include <znc/Message.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cctype>

enum class Scope { ALWAYS, DETACHED };

struct Entry {
    std::string mask_lc; // lowercased mask
    bool nick_only;      // true if pattern has no '!' and no '@'
    Scope scope;
};

// ---------- case-insensitive wildcard match for '*' and '?' ----------
static bool wildmatch_ci(const std::string& pat, const std::string& txt) {
    const size_t P = pat.size(), T = txt.size();
    // Fast path: exact equal
    if (pat == txt) return true;

    std::vector<std::vector<bool>> dp(P + 1, std::vector<bool>(T + 1, false));
    dp[0][0] = true;

    for (size_t i = 1; i <= P; ++i)
        if (pat[i-1] == '*') dp[i][0] = dp[i-1][0];

    auto eq = [](char a, char b){
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };

    for (size_t i = 1; i <= P; ++i) {
        for (size_t j = 1; j <= T; ++j) {
            char pc = pat[i-1], tc = txt[j-1];
            if (pc == '*') {
                dp[i][j] = dp[i-1][j] || dp[i][j-1];
            } else if (pc == '?' || eq(pc, tc)) {
                dp[i][j] = dp[i-1][j-1];
            }
        }
    }
    return dp[P][T];
}

// lowercased copy
static std::string lc(CString s) {
    std::string out(s.c_str());
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

class CIgnoreDrop : public CModule {
  public:
    MODCONSTRUCTOR(CIgnoreDrop) {
        AddHelpCommand();
        AddCommand("Add", "<mask> [detached|always]", "Add ignore; scope default: always",
                   [this](const CString& s){ CmdAdd(s); });
        AddCommand("Del", "<#|mask>", "Delete by index or exact mask",
                   [this](const CString& s){ CmdDel(s); });
        AddCommand("List", "", "List ignores with scope",
                   [this](const CString&){ CmdList(); });
        AddCommand("Clear", "", "Remove all ignores",
                   [this](const CString&){ ClearAll(); });
        AddCommand("SetScope", "<#> <detached|always>", "Change scope for an entry",
                   [this](const CString& s){ CmdSetScope(s); });
        AddCommand("SetDetachedPlayback", "<on|off>", "If on, detached rules also hide playback",
                   [this](const CString& s){ CmdSetDetachedPlayback(s); });
        AddCommand("Test", "<nick!ident@host>", "Report whether a sample would be dropped (live/playback) now",
                   [this](const CString& s){ CmdTest(s); });
    }

    bool OnLoad(const CString&, CString&) override {
        m_hideDetachedPlayback = (GetNV("detached_playback") == "1");
        LoadMasks();
        return true;
    }

    // ---- Live traffic (server -> client) ----
    EModRet OnChanMsg   (CNick& N, CChan& , CString& ) override { return LiveMaybeDrop(N); }
    EModRet OnPrivMsg   (CNick& N,         CString& ) override { return LiveMaybeDrop(N); }
    EModRet OnChanNotice(CNick& N, CChan& , CString& ) override { return LiveMaybeDrop(N); }
    EModRet OnPrivNotice(CNick& N,         CString& ) override { return LiveMaybeDrop(N); }
    EModRet OnChanAction(CNick& N, CChan& , CString& ) override { return LiveMaybeDrop(N); }
    EModRet OnPrivAction(CNick& N,         CString& ) override { return LiveMaybeDrop(N); }

    // ---- Buffer playback (when you attach) ----
    EModRet OnChanBufferPlayMessage(CMessage& Msg) override { return PlaybackMaybeDrop(Msg.GetNick()); }
    EModRet OnPrivBufferPlayMessage(CMessage& Msg) override { return PlaybackMaybeDrop(Msg.GetNick()); }

    // Older ZNC fallback (compile even if these hooks don't exist upstream)
    EModRet OnChanBufferPlayLine(CString& sLine) { return PlaybackMaybeDropFromLine(sLine); }
    EModRet OnPrivBufferPlayLine(CString& sLine) { return PlaybackMaybeDropFromLine(sLine); }

  private:
    std::vector<Entry> m_alwaysNick, m_alwaysHost;
    std::vector<Entry> m_detachedNick, m_detachedHost;
    bool m_hideDetachedPlayback{false};

    // ---------- Storage ----------
    static bool IsNickOnly(const std::string& m) {
        return (m.find('!') == std::string::npos) && (m.find('@') == std::string::npos);
    }

    void LoadMasks() {
        m_alwaysNick.clear(); m_alwaysHost.clear();
        m_detachedNick.clear(); m_detachedHost.clear();

        const CString raw = GetNV("masks");
        if (raw.empty()) return;

        VCString lines;
        raw.Split("\n", lines, false);
        for (const auto& line : lines) {
            CString L = line.Trim_n();
            if (L.empty()) continue;

            Scope sc = Scope::ALWAYS;
            CString mask = L;
            if (L.length() >= 2 && L[1] == '|') {
                if (L[0] == 'A') sc = Scope::ALWAYS;
                else if (L[0] == 'D') sc = Scope::DETACHED;
                mask = L.substr(2);
            }
            Entry e{ lc(mask), IsNickOnly(lc(mask)), sc };
            if (sc == Scope::ALWAYS) {
                (e.nick_only ? m_alwaysNick : m_alwaysHost).push_back(e);
            } else {
                (e.nick_only ? m_detachedNick : m_detachedHost).push_back(e);
            }
        }
    }

    void SaveMasks() {
        CString out;
        auto dump = [&](const std::vector<Entry>& v, char tag) {
            for (const auto& e : v) {
                if (!out.empty()) out += "\n";
                // We stored lowercased; it's fine to serialize lowercased.
                out += CString(1, tag) + "|" + e.mask_lc.c_str();
            }
        };
        dump(m_alwaysNick, 'A'); dump(m_alwaysHost, 'A');
        dump(m_detachedNick, 'D'); dump(m_detachedHost, 'D');
        SetNV("masks", out);
    }

    void ClearAll() {
        m_alwaysNick.clear(); m_alwaysHost.clear();
        m_detachedNick.clear(); m_detachedHost.clear();
        SaveMasks();
        PutModule("Cleared.");
    }

    // ---------- Matching ----------
    static bool AnyMatch(const std::vector<Entry>& vec, const std::string& needle) {
        for (const auto& e : vec) if (wildmatch_ci(e.mask_lc, needle)) return true;
        return false;
    }

    bool MatchesAlways(const std::string& nick_lc, const std::string& full_lc) const {
        if (!m_alwaysNick.empty() && AnyMatch(m_alwaysNick, nick_lc)) return true;
        if (!m_alwaysHost.empty() && AnyMatch(m_alwaysHost, full_lc)) return true;
        return false;
    }
    bool MatchesDetached(const std::string& nick_lc, const std::string& full_lc) const {
        if (!m_detachedNick.empty() && AnyMatch(m_detachedNick, nick_lc)) return true;
        if (!m_detachedHost.empty() && AnyMatch(m_detachedHost, full_lc)) return true;
        return false;
    }

    // ---------- Decisions ----------
    EModRet LiveMaybeDrop(const CNick& N) {
        // Build lowercased samples once.
        const std::string nick_l = lc(N.GetNick());
        const std::string full_l = lc(N.GetNick() + "!" + N.GetIdent() + "@" + N.GetHost());

        // Fast path: ALWAYS
        if (MatchesAlways(nick_l, full_l)) return HALT;

        // DETACHED: only if this network is detached (compute attach state lazily)
        if (!m_detachedNick.empty() || !m_detachedHost.empty()) {
            const bool attached =
                (GetNetwork() ? GetNetwork()->IsUserAttached()
                              : (GetUser() ? GetUser()->IsUserAttached() : true));
            if (!attached && MatchesDetached(nick_l, full_l)) return HALT;
        }
        return CONTINUE;
    }

    EModRet PlaybackMaybeDrop(const CNick& N) {
        const std::string nick_l = lc(N.GetNick());
        const std::string full_l = lc(N.GetNick() + "!" + N.GetIdent() + "@" + N.GetHost());

        // ALWAYS entries hide playback
        if (MatchesAlways(nick_l, full_l)) return HALT;

        // Optionally hide playback for DETACHED entries (config)
        if (m_hideDetachedPlayback && MatchesDetached(nick_l, full_l)) return HALT;

        return CONTINUE;
    }

    // Fallback for older *PlayLine hooks: parse prefix for a CNick-like check
    EModRet PlaybackMaybeDropFromLine(const CString& sLine) {
        if (!sLine.empty() && sLine[0] == ':') {
            CString prefix = sLine.Token(0, false, " "); // ":nick!ident@host"
            CString who = prefix.TrimPrefix_n(":");
            CNick fake(who);
            return PlaybackMaybeDrop(fake);
        }
        return CONTINUE;
    }

    // ---------- Small helpers ----------
    static bool IsDigits(const CString& s) {
        const char* p = s.c_str();
        if (!p || !*p) return false;
        for (; *p; ++p) if (*p < '0' || *p > '9') return false;
        return true;
    }
    static CString ScopeStr(Scope s) { return (s == Scope::ALWAYS) ? "always" : "detached"; }

    // ---------- Commands ----------
    void CmdAdd(const CString& sLine) {
        CString maskC = sLine.Token(1, false);
        CString scopeC = sLine.Token(2, false).AsLower();
        if (maskC.empty()) { PutModule("Usage: Add <mask> [detached|always]"); return; }

        Scope sc = Scope::ALWAYS;
        if (scopeC == "detached" || scopeC == "d") sc = Scope::DETACHED;
        else if (!scopeC.empty() && scopeC != "always" && scopeC != "a" && scopeC != "both") {
            PutModule("Unknown scope. Use 'detached' or 'always'.");
            return;
        }

        Entry e{ lc(maskC), IsNickOnly(lc(maskC)), sc };
        if (sc == Scope::ALWAYS) (e.nick_only ? m_alwaysNick : m_alwaysHost).push_back(e);
        else                     (e.nick_only ? m_detachedNick : m_detachedHost).push_back(e);

        SaveMasks();
        PutModule("Added: " + maskC + " [" + ScopeStr(sc) + "]");
    }

    void CmdDel(const CString& sLine) {
        CString arg = sLine.Token(1, true).Trim_n();
        if (arg.empty()) { PutModule("Usage: Del <#|mask>"); return; }

        auto erase_by_index = [&](size_t idx)->bool{
            auto erase_idx = [&](auto& vec)->bool{
                if (idx == 0 || idx > vec.size()) return false;
                vec.erase(vec.begin() + (idx - 1));
                return true;
            };
            // Flattened order for indexing in List(): alwaysNick, alwaysHost, detachedNick, detachedHost
            size_t n1 = m_alwaysNick.size();
            if (idx <= n1) return erase_idx(m_alwaysNick);
            idx -= n1;
            size_t n2 = m_alwaysHost.size();
            if (idx <= n2) return erase_idx(m_alwaysHost);
            idx -= n2;
            size_t n3 = m_detachedNick.size();
            if (idx <= n3) return erase_idx(m_detachedNick);
            idx -= n3;
            size_t n4 = m_detachedHost.size();
            if (idx <= n4) return erase_idx(m_detachedHost);
            return false;
        };

        auto erase_by_mask = [&](const std::string& m)->bool{
            auto erase_match = [&](auto& vec)->bool{
                for (auto it = vec.begin(); it != vec.end(); ++it) {
                    if (it->mask_lc == m) { vec.erase(it); return true; }
                }
                return false;
            };
            return erase_match(m_alwaysNick) || erase_match(m_alwaysHost) ||
                   erase_match(m_detachedNick) || erase_match(m_detachedHost);
        };

        bool ok = false;
        if (IsDigits(arg)) {
            ok = erase_by_index(arg.ToUInt());
        } else {
            ok = erase_by_mask(lc(arg));
        }

        if (!ok) { PutModule("Not found."); return; }
        SaveMasks();
        PutModule("Deleted.");
    }

    void CmdList() {
        size_t i = 0;
        auto dump = [&](const std::vector<Entry>& v, const char* label){
            for (const auto& e : v) {
                ++i;
                CString line = CString(std::to_string(i)) + ") " + e.mask_lc.c_str()
                             + " [" + label + (e.nick_only ? "|nick" : "|host") + "]";
                PutModule(line);
            }
        };
        if (m_alwaysNick.empty() && m_alwaysHost.empty() &&
            m_detachedNick.empty() && m_detachedHost.empty()) {
            PutModule("No ignores set.");
            return;
        }
        dump(m_alwaysNick, "always");
        dump(m_alwaysHost, "always");
        dump(m_detachedNick, "detached");
        dump(m_detachedHost, "detached");
        PutModule(CString("Detached playback: ") + (m_hideDetachedPlayback ? "on" : "off"));
    }

    void CmdSetScope(const CString& sLine) {
        CString idxC = sLine.Token(1, false);
        CString scopeC = sLine.Token(2, false).AsLower();
        if (!IsDigits(idxC) || scopeC.empty()) { PutModule("Usage: SetScope <#> <detached|always>"); return; }

        size_t idx = idxC.ToUInt();
        Scope sc;
        if (scopeC == "detached" || scopeC == "d") sc = Scope::DETACHED;
        else if (scopeC == "always" || scopeC == "a" || scopeC == "both") sc = Scope::ALWAYS;
        else { PutModule("Unknown scope."); return; }

        // Locate entry by flattened index, then move between buckets if needed
        auto move_entry = [&](auto& from, auto& to)->bool{
            if (idx == 0 || idx > from.size()) return false;
            Entry e = from[idx - 1];
            from.erase(from.begin() + (idx - 1));
            e.scope = sc;
            to.push_back(e);
            return true;
        };

        size_t n1 = m_alwaysNick.size();
        if (idx <= n1) {
            Entry e = m_alwaysNick[idx - 1];
            m_alwaysNick.erase(m_alwaysNick.begin() + (idx - 1));
            e.scope = sc;
            (e.nick_only ? (sc == Scope::ALWAYS ? m_alwaysNick : m_detachedNick)
                         : (sc == Scope::ALWAYS ? m_alwaysHost : m_detachedHost)).push_back(e);
        } else {
            idx -= n1;
            size_t n2 = m_alwaysHost.size();
            if (idx <= n2) {
                Entry e = m_alwaysHost[idx - 1];
                m_alwaysHost.erase(m_alwaysHost.begin() + (idx - 1));
                e.scope = sc;
                (e.nick_only ? (sc == Scope::ALWAYS ? m_alwaysNick : m_detachedNick)
                             : (sc == Scope::ALWAYS ? m_alwaysHost : m_detachedHost)).push_back(e);
            } else {
                idx -= n2;
                size_t n3 = m_detachedNick.size();
                if (idx <= n3) {
                    Entry e = m_detachedNick[idx - 1];
                    m_detachedNick.erase(m_detachedNick.begin() + (idx - 1));
                    e.scope = sc;
                    (e.nick_only ? (sc == Scope::ALWAYS ? m_alwaysNick : m_detachedNick)
                                 : (sc == Scope::ALWAYS ? m_alwaysHost : m_detachedHost)).push_back(e);
                } else {
                    idx -= n3;
                    size_t n4 = m_detachedHost.size();
                    if (idx == 0 || idx > n4) { PutModule("Index out of range."); return; }
                    Entry e = m_detachedHost[idx - 1];
                    m_detachedHost.erase(m_detachedHost.begin() + (idx - 1));
                    e.scope = sc;
                    (e.nick_only ? (sc == Scope::ALWAYS ? m_alwaysNick : m_detachedNick)
                                 : (sc == Scope::ALWAYS ? m_alwaysHost : m_detachedHost)).push_back(e);
                }
            }
        }
        SaveMasks();
        PutModule("Scope updated.");
    }

    void CmdSetDetachedPlayback(const CString& sLine) {
        CString arg = sLine.Token(1, false).AsLower();
        if (arg != "on" && arg != "off") { PutModule("Usage: SetDetachedPlayback <on|off>"); return; }
        m_hideDetachedPlayback = (arg == "on");
        SetNV("detached_playback", m_hideDetachedPlayback ? "1" : "0");
        PutModule(CString("Detached playback set to ") + (m_hideDetachedPlayback ? "on" : "off"));
    }

    void CmdTest(const CString& sLine) {
        CString sampleC = sLine.Token(1, true).Trim_n();
        if (sampleC.empty()) { PutModule("Usage: Test <nick!ident@host>"); return; }

        const std::string full_l = lc(sampleC);
        const std::string nick_l = lc(sampleC.Token(0, false, "!"));
        const bool attached =
            (GetNetwork() ? GetNetwork()->IsUserAttached()
                          : (GetUser() ? GetUser()->IsUserAttached() : true));

        bool dropLive = false, dropPlay = false;
        if (MatchesAlways(nick_l, full_l)) { dropLive = true; dropPlay = true; }
        else {
            if (!attached && MatchesDetached(nick_l, full_l)) dropLive = true;
            if (m_hideDetachedPlayback && MatchesDetached(nick_l, full_l)) dropPlay = true;
        }

        PutModule(CString(std::string("Live: ") + (dropLive ? "DROP" : "pass")
                          + " | Playback: " + (dropPlay ? "DROP" : "pass")));
    }
};

template<> void TModInfo<CIgnoreDrop>(CModInfo& Info) {
    Info.SetWikiPage("ignore_drop");
    Info.SetHasArgs(false);
    Info.SetDescription(
        "Network-scoped server-side ignore. ALWAYS: live+playback. "
        "DETACHED: live only while network is detached; playback optional."
    );
    // Network-only to avoid confusion: one instance & NV per network.
    Info.AddType(CModInfo::NetworkModule);
}

USERMODULEDEFS(CIgnoreDrop, "Server-side ignore (network-only, optimized)")

