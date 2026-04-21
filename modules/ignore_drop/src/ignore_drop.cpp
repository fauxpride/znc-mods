// ignore_drop.cpp v1.1.0 — ZNC server-side ignore (Network module)
// Build:  znc-buildmod ignore_drop.cpp
// Load:   /msg *status LoadMod --type=network ignore_drop
// Help:   /msg *ignore_drop help
//
// v1.1.0 security / performance changes:
//   - RFC 1459 case folding ([]\~ fold to {}|^) closes the nick-case bypass.
//   - Fallback playback parser now skips IRCv3 message tags.
//   - Allocation-free iterative wildcard matcher replaces the per-message
//     O(P*T) dynamic-programming table.
//   - Stricter mask validation at Add time + safer NV load path (unknown
//     scope prefixes are now skipped with a warning rather than silently
//     coerced to ALWAYS).
//
// Storage (per network, backwards-compatible with 1.0.0):
//   NV "masks":             one per line, "A|mask" or "D|mask"
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

#define IGNORE_DROP_VERSION "1.1.0"
static constexpr const char* kIgnoreDropVersion = IGNORE_DROP_VERSION;

enum class Scope { ALWAYS, DETACHED };

struct Entry {
    std::string mask_lc; // RFC 1459-folded mask (see fold_char below)
    bool nick_only;      // true if pattern has no '!' and no '@'
    Scope scope;
};

// ---------- RFC 1459 case folding ----------
// Most IRCds use the "rfc1459" casemapping, under which []\~ are the uppercase
// forms of {|}^. Plain ASCII tolower lets a user bypass a mask like "[bot]*"
// by registering "{bot}*". Folding both the mask (at Add/Load time) and the
// incoming sample closes that gap.
static inline unsigned char fold_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
    switch (c) {
        case '[':  return '{';
        case ']':  return '}';
        case '\\': return '|';
        case '~':  return '^';
        default:   return c;
    }
}

static std::string fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back((char)fold_char(c));
    return out;
}
static std::string fold(const CString& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out.push_back((char)fold_char((unsigned char)s[i]));
    return out;
}

// ---------- Allocation-free iterative wildcard matcher ----------
// Both pat and txt must already be RFC 1459-folded by the caller. Supports
// '*' (any run, including empty) and '?' (exactly one character), using
// classic star-backtracking. Typical IRC masks run at O(P+T); adversarial
// patterns fall back to O(P*T), but in every case there is no heap
// allocation per match (vs. the previous vector<vector<bool>> DP table).
static bool wildmatch_folded(const std::string& pat, const std::string& txt) {
    const size_t P = pat.size(), T = txt.size();
    if (pat == txt) return true;

    size_t p = 0, t = 0;
    size_t star = std::string::npos;
    size_t match = 0;

    while (t < T) {
        if (p < P && pat[p] == '*') {
            star = p++;
            match = t;
        } else if (p < P && (pat[p] == '?' || pat[p] == txt[t])) {
            ++p;
            ++t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < P && pat[p] == '*') ++p;
    return p == P;
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
        AddCommand("Version", "", "Print module version",
                   [this](const CString&){
                       PutModule(CString("ignore_drop version ") + kIgnoreDropVersion);
                   });
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

    // Older ZNC fallback (compile even if these hooks don't exist upstream).
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

    // Input-side mask validation. Returns false and sets `reason` if the mask
    // would corrupt the newline-delimited NV storage or collide with the
    // "A|"/"D|" scope prefix used on disk.
    static bool ValidateMask(const CString& m, CString& reason) {
        if (m.empty()) { reason = "mask is empty"; return false; }
        for (size_t i = 0; i < m.size(); ++i) {
            unsigned char c = (unsigned char)m[i];
            if (c == '\n' || c == '\r' || c == '\0') {
                reason = "mask contains an illegal control character (LF/CR/NUL)";
                return false;
            }
        }
        if (m.size() >= 2 && m[1] == '|' &&
            (m[0] == 'A' || m[0] == 'a' || m[0] == 'D' || m[0] == 'd')) {
            reason = "mask cannot begin with 'A|' or 'D|' (reserved for storage format)";
            return false;
        }
        return true;
    }

    void LoadMasks() {
        m_alwaysNick.clear(); m_alwaysHost.clear();
        m_detachedNick.clear(); m_detachedHost.clear();

        const CString raw = GetNV("masks");
        if (raw.empty()) return;

        VCString lines;
        raw.Split("\n", lines, false);
        size_t skipped = 0;
        for (const auto& line : lines) {
            CString L = line.Trim_n();
            if (L.empty()) continue;

            Scope sc = Scope::ALWAYS;
            CString mask;
            if (L.length() >= 2 && L[1] == '|') {
                if (L[0] == 'A')      { sc = Scope::ALWAYS;   mask = L.substr(2); }
                else if (L[0] == 'D') { sc = Scope::DETACHED; mask = L.substr(2); }
                else {
                    // Unknown scope byte — do NOT silently coerce to ALWAYS.
                    ++skipped;
                    continue;
                }
            } else {
                // Pre-1.1 entries wrote no scope prefix at all; treat those as
                // ALWAYS for backwards compatibility.
                mask = L;
            }
            if (mask.empty()) { ++skipped; continue; }

            // Re-fold on load so legacy ASCII-only lowercased entries are
            // normalised to RFC 1459 form in memory.
            std::string folded = fold(mask);
            Entry e{ folded, IsNickOnly(folded), sc };
            if (sc == Scope::ALWAYS) {
                (e.nick_only ? m_alwaysNick : m_alwaysHost).push_back(e);
            } else {
                (e.nick_only ? m_detachedNick : m_detachedHost).push_back(e);
            }
        }
        if (skipped) {
            PutModule("[ignore_drop] Skipped " + CString(std::to_string(skipped)) +
                      " malformed mask entry/entries during load.");
        }
    }

    void SaveMasks() {
        CString out;
        auto dump = [&](const std::vector<Entry>& v, char tag) {
            for (const auto& e : v) {
                if (!out.empty()) out += "\n";
                // We stored folded; it is fine to serialize folded.
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
        for (const auto& e : vec) if (wildmatch_folded(e.mask_lc, needle)) return true;
        return false;
    }

    bool MatchesAlways(const std::string& nick_f, const std::string& full_f) const {
        if (!m_alwaysNick.empty() && AnyMatch(m_alwaysNick, nick_f)) return true;
        if (!m_alwaysHost.empty() && AnyMatch(m_alwaysHost, full_f)) return true;
        return false;
    }
    bool MatchesDetached(const std::string& nick_f, const std::string& full_f) const {
        if (!m_detachedNick.empty() && AnyMatch(m_detachedNick, nick_f)) return true;
        if (!m_detachedHost.empty() && AnyMatch(m_detachedHost, full_f)) return true;
        return false;
    }

    // ---------- Decisions ----------
    EModRet LiveMaybeDrop(const CNick& N) {
        const std::string nick_f = fold(N.GetNick());
        const std::string full_f = fold(N.GetNick() + "!" + N.GetIdent() + "@" + N.GetHost());

        // Fast path: ALWAYS
        if (MatchesAlways(nick_f, full_f)) return HALT;

        // DETACHED: only if this network is detached (compute attach state lazily)
        if (!m_detachedNick.empty() || !m_detachedHost.empty()) {
            const bool attached =
                (GetNetwork() ? GetNetwork()->IsUserAttached()
                              : (GetUser() ? GetUser()->IsUserAttached() : true));
            if (!attached && MatchesDetached(nick_f, full_f)) return HALT;
        }
        return CONTINUE;
    }

    EModRet PlaybackMaybeDrop(const CNick& N) {
        const std::string nick_f = fold(N.GetNick());
        const std::string full_f = fold(N.GetNick() + "!" + N.GetIdent() + "@" + N.GetHost());

        if (MatchesAlways(nick_f, full_f)) return HALT;
        if (m_hideDetachedPlayback && MatchesDetached(nick_f, full_f)) return HALT;
        return CONTINUE;
    }

    // Fallback for older *PlayLine hooks: parse prefix for a CNick-like check.
    // Handles IRCv3 message tags — a leading "@tag=value;..." block — by
    // skipping past them before looking for the ":prefix".
    EModRet PlaybackMaybeDropFromLine(const CString& sLine) {
        size_t pos = 0;
        const size_t N = sLine.size();

        // IRCv3 message-tag block: "@tag=value;tag2=value2 " ...
        if (pos < N && sLine[pos] == '@') {
            size_t sp = sLine.find(' ', pos);
            if (sp == CString::npos) return CONTINUE; // malformed, fail open
            pos = sp + 1;
            while (pos < N && sLine[pos] == ' ') ++pos;
        }

        if (pos < N && sLine[pos] == ':') {
            size_t sp = sLine.find(' ', pos);
            CString who = (sp == CString::npos)
                ? sLine.substr(pos + 1)
                : sLine.substr(pos + 1, sp - pos - 1);
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

        CString why;
        if (!ValidateMask(maskC, why)) {
            PutModule("Rejected: " + why + ".");
            return;
        }

        Scope sc = Scope::ALWAYS;
        if (scopeC == "detached" || scopeC == "d") sc = Scope::DETACHED;
        else if (!scopeC.empty() && scopeC != "always" && scopeC != "a" && scopeC != "both") {
            PutModule("Unknown scope. Use 'detached' or 'always'.");
            return;
        }

        std::string folded = fold(maskC);
        Entry e{ folded, IsNickOnly(folded), sc };
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
            ok = erase_by_mask(fold(arg));
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

        const std::string full_f = fold(sampleC);
        const std::string nick_f = fold(sampleC.Token(0, false, "!"));
        const bool attached =
            (GetNetwork() ? GetNetwork()->IsUserAttached()
                          : (GetUser() ? GetUser()->IsUserAttached() : true));

        bool dropLive = false, dropPlay = false;
        if (MatchesAlways(nick_f, full_f)) { dropLive = true; dropPlay = true; }
        else {
            if (!attached && MatchesDetached(nick_f, full_f)) dropLive = true;
            if (m_hideDetachedPlayback && MatchesDetached(nick_f, full_f)) dropPlay = true;
        }

        PutModule(CString(std::string("Live: ") + (dropLive ? "DROP" : "pass")
                          + " | Playback: " + (dropPlay ? "DROP" : "pass")));
    }
};

template<> void TModInfo<CIgnoreDrop>(CModInfo& Info) {
    Info.SetWikiPage("ignore_drop");
    Info.SetHasArgs(false);
    Info.SetDescription(
        "Network-scoped server-side ignore (v" IGNORE_DROP_VERSION "). "
        "ALWAYS: live+playback. DETACHED: live only while network is detached; "
        "playback optional."
    );
    // Network-only to avoid confusion: one instance & NV per network.
    Info.AddType(CModInfo::NetworkModule);
}

USERMODULEDEFS(CIgnoreDrop, "Server-side ignore (network-only, optimized) v" IGNORE_DROP_VERSION)
