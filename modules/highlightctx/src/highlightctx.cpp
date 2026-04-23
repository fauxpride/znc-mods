// highlightctx.cpp — detached-only highlight context capture for ZNC
// Build:  znc-buildmod highlightctx.cpp
// Load:   /msg *status LoadMod --type=network highlightctx [before=8 after=8 require_ignore_drop=auto excludes=#chan1,#chan2,BadNick,*!*@evil.example]
// Help:   /msg *highlightctx Help
//
// Design goals:
// - Network module, intended for ZNC 1.9.1+.
// - Detached-only capture: records only when this network has no attached clients.
// - Live hooks only: does not inspect/play normal channel buffers.
// - Independent of normal buffer length: keeps its own in-memory per-channel ring buffer.
// - Strict durability for active highlights: only active/completed highlight events are journaled to disk.
// - RAM-only for ordinary channel chatter.
// - Optional ignore_drop integration with three modes: off, on, auto.
// - Replays into *highlightctx on attach, sorted by channel, then clears delivered events.
// - Replays with native IRCv3 server-time semantics when the client supports the time tag/cap; else falls back to inline timestamp text.
// - Exclusions support channels AND nicks/hostmasks, configurable before/after caps, verbose commands.
//
// Exclusion semantics:
// - Channel exclusions cause the channel to be ignored entirely: no triggers,
//   no context collection, no ring buffering for that channel.
// - Nick exclusions are narrower: messages from the excluded nick or hostmask
//   STILL appear as context for other events (before/after), but CANNOT
//   start a new highlight event. This lets you suppress a noisy user as a
//   trigger source without losing context when they comment around a real
//   trigger from someone else.
// - Nick exclusion masks use RFC 1459 case folding and support `*` / `?`
//   wildcards, matching the syntax used by the ignore_drop module. A mask
//   with neither `!` nor `@` is matched against the sender's nickname only;
//   a mask containing either is matched against the full `nick!ident@host`.
//
// Notes:
// - The "before" and "after" values are maxima, not guarantees.
// - If you attach before enough trailing lines arrive, the event is replayed as partial.
// - If ZNC or the VPS dies mid-capture, the event is recovered from the durable journal
//   and replayed as partial on the next attach if it wasn't fully completed.
// - In auto mode, ignore_drop enforcement is armed only if ignore_drop is positioned
//   ahead of highlightctx in the network's module list, so its hooks fire before ours.
//   The arming state is re-evaluated at these points:
//     * at module OnLoad (original heuristic: is ignore_drop already in the list?)
//     * at ZNC OnBoot (for modules loaded from znc.conf, re-checks actual hook order
//       after all znc.conf modules have loaded — fixes the common /znc restart case
//       where alphabetical ordering puts highlightctx before ignore_drop)
//     * on demand via the Rearm command
//   Runtime load or unload of ignore_drop does not automatically re-arm or disarm,
//   because ZNC 1.9.x dispatches OnModuleLoading/OnModuleUnloading only to
//   global-scope modules. Use Rearm after runtime changes if you want the armed
//   state to reflect them for display purposes.
//   Capture correctness is independently protected at runtime by a live
//   HasIgnoreDropLoaded() check in ShouldCaptureNow(): when the strict requirement
//   is effective but ignore_drop is absent, capture pauses until ignore_drop is
//   available again, regardless of the cached armed flag.
//   Armed semantics are deliberately sticky: once armed, the strict requirement
//   stays effective even if ignore_drop is temporarily unloaded, so capture pauses
//   rather than silently resuming without the protection the user asked for.

#include <znc/znc.h>
#include <znc/Modules.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/Chan.h>
#include <znc/Client.h>
#include <znc/Nick.h>
#include <znc/Message.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

static const char* kModVersion = "highlightctx 0.8.0";
static const char* kJournalName = "highlightctx.journal";
static const size_t kCompactThresholdLines = 512;

enum class ERequireIgnoreMode {
    Off,
    On,
    Auto,
};

static const char* ignore_mode_to_storage(ERequireIgnoreMode mode) {
    switch (mode) {
        case ERequireIgnoreMode::Off: return "off";
        case ERequireIgnoreMode::On:  return "on";
        case ERequireIgnoreMode::Auto:return "auto";
    }
    return "off";
}

static CString ignore_mode_to_cstring(ERequireIgnoreMode mode) {
    return ignore_mode_to_storage(mode);
}

static bool parse_ignore_mode(const CString& s, ERequireIgnoreMode& out) {
    CString v = s.AsLower().Trim_n();
    if (v == "0" || v == "off" || v == "no" || v == "false" || v == "disable" || v == "disabled") {
        out = ERequireIgnoreMode::Off;
        return true;
    }
    if (v == "1" || v == "on" || v == "yes" || v == "true" || v == "enable" || v == "enabled") {
        out = ERequireIgnoreMode::On;
        return true;
    }
    if (v == "auto") {
        out = ERequireIgnoreMode::Auto;
        return true;
    }
    return false;
}

static std::string lc(const CString& in) {
    std::string out(in.c_str());
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static std::string lc(const std::string& in) {
    std::string out(in);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool parse_uint_cstr(const CString& s, unsigned int& out) {
    CString v = s.Trim_n();
    if (v.empty()) return false;
    const char* p = v.c_str();
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    out = v.ToUInt();
    return true;
}

static std::string hex_encode(const std::string& in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        out.push_back(kHex[(c >> 4) & 0x0F]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

static bool hex_value(char c, unsigned char& out) {
    if (c >= '0' && c <= '9') { out = static_cast<unsigned char>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<unsigned char>(10 + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<unsigned char>(10 + (c - 'A')); return true; }
    return false;
}

static bool hex_decode(const std::string& in, std::string& out) {
    out.clear();
    if (in.size() % 2 != 0) return false;
    out.reserve(in.size() / 2);
    for (size_t i = 0; i < in.size(); i += 2) {
        unsigned char hi = 0, lo = 0;
        if (!hex_value(in[i], hi) || !hex_value(in[i + 1], lo)) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

static std::vector<std::string> split_char(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static std::string join_char(const std::vector<std::string>& parts, char delim) {
    std::string out;
    bool first = true;
    for (const auto& p : parts) {
        if (!first) out.push_back(delim);
        first = false;
        out += p;
    }
    return out;
}

static bool is_nick_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) return true;
    switch (c) {
        case '-': case '_': case '[': case ']': case '\\': case '`':
        case '^': case '{': case '}': case '|':
            return true;
        default:
            return false;
    }
}

// RFC 1459 case folding. Adds [ <-> {, ] <-> }, \ <-> |, ~ <-> ^ to ASCII
// A-Z <-> a-z. Matches the folding used by ignore_drop so that nick masks
// compare consistently between the two modules (e.g. [bot] and {bot} fold
// to the same sequence and cannot be used to defeat a mask). Returns the
// folded form as a new string; input is not modified.
static std::string rfc1459_fold(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == '[') {
            out.push_back('{');
        } else if (c == ']') {
            out.push_back('}');
        } else if (c == '\\') {
            out.push_back('|');
        } else if (c == '~') {
            out.push_back('^');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static std::string rfc1459_fold(const CString& in) {
    return rfc1459_fold(std::string(in.c_str()));
}

// Channel prefix detection. The common IRC channel prefix characters are
// #, &, +, !. Some networks permit additional prefixes via the ISUPPORT
// CHANTYPES token, but # covers the overwhelming majority of real channels
// and &+! catch the rest of RFC 2811. Used to classify an exclusion token
// as a channel name vs a nick mask at Add time.
static bool is_channel_name(const CString& s) {
    if (s.empty()) return false;
    char c = s[0];
    return (c == '#' || c == '&' || c == '+' || c == '!');
}

// Iterative * / ? glob matcher with star-backtracking. Inputs are already
// RFC 1459-folded by the caller. Allocation-free. Accepts * for any
// sequence (including empty) and ? for exactly one character. Any other
// byte is matched literally.
static bool wildmatch_folded(const std::string& pattern, const std::string& text) {
    if (pattern == text) return true;
    size_t p = 0, t = 0, star = std::string::npos, match = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

static bool contains_nick_highlight(const CString& text, const CString& nick) {
    std::string hay = lc(text);
    std::string needle = lc(nick);
    if (needle.empty()) return false;

    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        bool left_ok = (pos == 0) || !is_nick_char(hay[pos - 1]);
        bool right_ok = (pos + needle.size() >= hay.size()) || !is_nick_char(hay[pos + needle.size()]);
        if (left_ok && right_ok) return true;
        ++pos;
    }
    return false;
}

static bool ensure_parent_dir_fsync(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return true;
    std::string dir = path.substr(0, slash);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return false;
    bool ok = (::fsync(dfd) == 0);
    ::close(dfd);
    return ok;
}

static bool durable_append_line(const std::string& path, const std::string& line) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;

    std::string data = line;
    if (data.empty() || data.back() != '\n') data.push_back('\n');

    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        p += static_cast<size_t>(w);
        left -= static_cast<size_t>(w);
    }

    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    if (!ok) return false;
    return ensure_parent_dir_fsync(path);
}

static bool durable_replace_file(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        p += static_cast<size_t>(w);
        left -= static_cast<size_t>(w);
    }

    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    if (!ok) return false;

    if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
    return ensure_parent_dir_fsync(path);
}

static bool slurp_lines(const std::string& path, std::vector<std::string>& lines) {
    lines.clear();
    FILE* fp = ::fopen(path.c_str(), "rb");
    if (!fp) return false;

    char* buf = nullptr;
    size_t cap = 0;
    ssize_t n = 0;
    while ((n = ::getline(&buf, &cap, fp)) != -1) {
        std::string line(buf, static_cast<size_t>(n));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        lines.push_back(line);
    }
    if (buf) ::free(buf);
    ::fclose(fp);
    return true;
}

static std::string sanitize_irc_text(const CString& in) {
    std::string out(in.c_str());
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    return out;
}

static std::string format_iso8601_utc(long long ts_sec) {
    std::time_t tt = static_cast<std::time_t>(ts_sec);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    if (gmtime_r(&tt, &tmv) == nullptr) {
        return "1970-01-01T00:00:00.000Z";
    }
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tmv) == 0) {
        return "1970-01-01T00:00:00.000Z";
    }
    return buf;
}

static std::string format_hms_utc(long long ts_sec) {
    std::time_t tt = static_cast<std::time_t>(ts_sec);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    if (gmtime_r(&tt, &tmv) == nullptr) {
        return "00:00:00";
    }
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv) == 0) {
        return "00:00:00";
    }
    return buf;
}

}  // namespace

class CHighlightCtx : public CModule {
  public:
    MODCONSTRUCTOR(CHighlightCtx) {
        AddHelpCommand();

        AddCommand("Overview", "",
                   "Show a detailed explanation of how highlightctx captures, stores, replays, clears highlight-context events, and how the off/on/auto ignore_drop modes behave, including native timestamp replay.",
                   [this](const CString&) { CmdOverview(); });
        AddCommand("Version", "",
                   "Show the module version marker that can be used to track later iterations of this plugin.",
                   [this](const CString&) { PutModule(CString("Version: ") + kModVersion); });
        AddCommand("Status", "",
                   "Show current settings, exclusion list size, whether ignore_drop is required/effective/present, whether auto mode is armed, and how many open/pending events currently exist.",
                   [this](const CString&) { CmdStatus(); });
        AddCommand("ReplayNow", "",
                   "Finalize any currently open detached highlight captures as partial if needed, replay all pending events into *highlightctx for this client now, then clear the delivered events.",
                   [this](const CString&) { ReplayAndClear(/*manual=*/true); });
        AddCommand("SetBefore", "<count>",
                   "Set the maximum number of messages to snapshot before a highlight trigger. This is a cap, not a guarantee; fewer lines may exist if the detached-only ring has less history.",
                   [this](const CString& sLine) { CmdSetBefore(sLine); });
        AddCommand("SetAfter", "<count>",
                   "Set the maximum number of messages to collect after a highlight trigger. This is a cap, not a guarantee; if you attach before enough later traffic arrives, the event is replayed as partial.",
                   [this](const CString& sLine) { CmdSetAfter(sLine); });
        AddCommand("SetMaxEvents", "<count|0|off>",
                   "Set the maximum number of finalized pending events to keep at once. When the cap is reached the oldest event is silently dropped to make room. Use 0 or off to disable the cap entirely (the default).",
                   [this](const CString& sLine) { CmdSetMaxEvents(sLine); });
        AddCommand("Reset", "",
                   "Reset all settings to compiled-in defaults: before=8, after=8, max_events=disabled, require_ignore_drop=auto, all exclusions (channel and nick/mask) cleared. Does not discard pending or open events.",
                   [this](const CString&) { CmdReset(); });
        AddCommand("AddExclude", "<#channel|nick|mask>",
                   "Exclude a channel, nickname, or nick!ident@host mask from detached highlight capture. Channel exclusions drop the channel entirely (no triggers, no context). Nick/hostmask exclusions are narrower: messages from the excluded sender still appear as context around other triggers, but cannot start a new event. A token starting with a channel prefix character (#, &, +, !) is treated as a channel; anything else is treated as a nick mask. Masks use RFC 1459 case folding and support the * and ? wildcards. A mask with neither ! nor @ matches the sender's nickname only; a mask containing either matches the full nick!ident@host.",
                   [this](const CString& sLine) { CmdAddExclude(sLine); });
        AddCommand("DelExclude", "<#channel|nick|mask|index>",
                   "Remove a previously added exclusion by channel name, nick mask, or by the numeric index shown in ListExcludes. Channel and nick/mask exclusions can both be removed with this command.",
                   [this](const CString& sLine) { CmdDelExclude(sLine); });
        AddCommand("ListExcludes", "",
                   "List all currently configured exclusions for this network instance of highlightctx. Shows both channel and nick/hostmask exclusions, each tagged with its kind, in a single numbered list usable by DelExclude.",
                   [this](const CString&) { CmdListExcludes(); });
        AddCommand("SetRequireIgnoreDrop", "<off|on|auto>",
                   "Set ignore_drop integration mode. off = never require ignore_drop. on = require ignore_drop to be loaded now and keep capture paused whenever it is absent. auto = arm the same strict behavior only if ignore_drop is positioned ahead of highlightctx in the network's module list so its hooks fire before ours; see Rearm to re-check after a runtime change.",
                   [this](const CString& sLine) { CmdSetRequireIgnoreDrop(sLine); });
        AddCommand("Rearm", "",
                   "Re-check ignore_drop presence and hook-order position without reloading the module. Reports whether strict ignore-aware capture is now armed. Note: if ignore_drop is present but positioned after highlightctx in the module list, Rearm cannot fix hook order on its own; unload and reload highlightctx to move it to the end of the list, or reorder the LoadModule lines in znc.conf for the next restart.",
                   [this](const CString&) { CmdRearm(); });
        AddCommand("Compact", "",
                   "Rewrite the durable on-disk journal to the minimum representation needed for currently open/pending events. This is usually automatic after replay/clear and when the journal grows large.",
                   [this](const CString&) { CmdCompact(); });
        AddCommand("ClearPending", "",
                   "Discard all currently pending/open events for this network instance and compact the journal. This is mainly for emergency cleanup/testing.",
                   [this](const CString&) { CmdClearPending(); });
    }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        LoadConfig();
        if (!ApplyLoadArgs(sArgs, sMessage)) return false;

        m_ignore_drop_present_on_module_load = HasIgnoreDropLoaded();
        RecomputeIgnoreDropRuntimeState();

        if (m_require_ignore_mode == ERequireIgnoreMode::On && !m_ignore_drop_present_on_module_load) {
            sMessage = "highlightctx aborted: require_ignore_drop=on, but ignore_drop is not already loaded on this network. Load ignore_drop first or use require_ignore_drop=off/auto.";
            return false;
        }

        TrimAllRings();
        EnsureJournalPath();
        if (!LoadJournal(sMessage)) return false;

        SetNV("version_marker", kModVersion);
        sMessage = "Loaded ";
        sMessage += kModVersion;
        sMessage += " | before=";
        sMessage += CString(std::to_string(m_before_max));
        sMessage += " after=";
        sMessage += CString(std::to_string(m_after_max));
        sMessage += " require_ignore_drop=";
        sMessage += IgnoreModeName();
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto) {
            sMessage += " (armed=";
            sMessage += (m_auto_ignore_drop_armed ? "yes" : "no");
            sMessage += ")";
        }
        sMessage += " max_events=";
        sMessage += (m_max_events == 0 ? "disabled" : CString(std::to_string(m_max_events)).c_str());
        sMessage += " excludes=";
        sMessage += CString(std::to_string(m_excluded.size()));
        sMessage += "ch/";
        sMessage += CString(std::to_string(m_excluded_nicks.size()));
        sMessage += "nick";
        return true;
    }

    void OnClientAttached() override {
        ReplayAndClear(/*manual=*/false);
        ClearVolatileState();
    }

    void OnClientDetached() override {
        ClearVolatileState();
    }

    bool OnBoot() override {
        // Fires after all znc.conf modules have been loaded. At this point we
        // can inspect actual module-list positions to determine whether
        // ignore_drop's hooks will fire before ours. This fixes the common
        // /znc restart case where alphabetical load order would put
        // highlightctx ahead of ignore_drop — OnLoad's HasIgnoreDropLoaded()
        // would return false at that point, leaving auto mode unarmed, even
        // though both modules end up loaded. By re-checking here we can arm
        // automatically as long as the config order is fixed up so
        // ignore_drop appears before highlightctx in znc.conf.
        RecheckIgnoreDropHookOrder();
        return true;
    }

    // Note on OnModuleLoading / OnModuleUnloading: these hooks exist on
    // CModule's virtual interface, but ZNC 1.9.x dispatches them only via
    // GLOBALMODULECALL, so they fire on global-scope modules only. A network
    // module that overrides them never receives the callback. Runtime load
    // or unload of ignore_drop therefore does NOT update our armed flag
    // automatically. Two mechanisms cover this:
    //   1) ShouldCaptureNow() calls HasIgnoreDropLoaded() live on every
    //      incoming message, so capture correctly pauses when ignore_drop
    //      is absent regardless of the cached armed flag.
    //   2) The Rearm command re-evaluates the flag on demand after any
    //      runtime load/unload that the operator has performed.
    // Armed semantics are deliberately sticky: once armed, the strict
    // ignore_drop requirement stays effective even if ignore_drop is
    // temporarily unloaded, so capture pauses rather than silently
    // resuming without the protection the user asked for.

    EModRet OnChanTextMessage(CTextMessage& Message) override {
        HandleIncoming(Message, 'T');
        return CONTINUE;
    }

    EModRet OnChanNoticeMessage(CNoticeMessage& Message) override {
        HandleIncoming(Message, 'N');
        return CONTINUE;
    }

    EModRet OnChanActionMessage(CActionMessage& Message) override {
        HandleIncoming(Message, 'A');
        return CONTINUE;
    }

  private:
    struct CaptureLine {
        long long ts_sec{0};
        char kind{'T'};
        CString nick;
        CString text;
    };

    struct Event {
        unsigned long long id{0};
        CString channel;
        std::string channel_lc;
        long long started_ts{0};
        unsigned int after_cap{0};
        std::vector<CaptureLine> before;
        CaptureLine trigger;
        std::vector<CaptureLine> after;
        bool finalized{false};
        bool partial{false};
    };

    struct NickExclude {
        std::string mask_folded;  // RFC 1459-folded mask, preserved wildcards
        bool nick_only{true};     // true = match nick only; false = match full nick!ident@host
    };

    unsigned int m_before_max{8};
    unsigned int m_after_max{8};
    unsigned int m_max_events{0};  // 0 = disabled (no cap on pending events)
    ERequireIgnoreMode m_require_ignore_mode{ERequireIgnoreMode::Auto};
    bool m_ignore_drop_present_on_module_load{false};
    bool m_auto_ignore_drop_armed{false};

    std::set<std::string> m_excluded;                 // channel exclusions, lowercased names
    std::vector<NickExclude> m_excluded_nicks;        // nick/hostmask exclusions
    std::map<std::string, std::deque<CaptureLine>> m_ring_by_chan;
    std::map<std::string, std::vector<Event>> m_open_by_chan;
    std::vector<Event> m_pending;

    unsigned long long m_next_id{1};
    std::string m_journal_path;
    size_t m_journal_line_count{0};

    void LoadConfig() {
        if (HasNV("before_max")) {
            m_before_max = GetNV("before_max").ToUInt();
        }
        if (HasNV("after_max")) {
            m_after_max = GetNV("after_max").ToUInt();
        }
        if (HasNV("max_events")) {
            m_max_events = GetNV("max_events").ToUInt();
        }
        if (HasNV("require_ignore_drop_mode")) {
            ERequireIgnoreMode parsed = ERequireIgnoreMode::Auto;
            if (parse_ignore_mode(GetNV("require_ignore_drop_mode"), parsed)) {
                m_require_ignore_mode = parsed;
            }
        } else if (HasNV("require_ignore_drop")) {
            ERequireIgnoreMode legacy = (GetNV("require_ignore_drop") == "1") ? ERequireIgnoreMode::On : ERequireIgnoreMode::Off;
            m_require_ignore_mode = legacy;
            SetNV("require_ignore_drop_mode", ignore_mode_to_storage(m_require_ignore_mode));
        }

        m_excluded.clear();
        VCString lines;
        GetNV("excluded_channels").Split("\n", lines, false);
        for (const auto& c : lines) {
            CString chan = c.Trim_n();
            if (!chan.empty()) m_excluded.insert(lc(chan));
        }

        m_excluded_nicks.clear();
        VCString nlines;
        GetNV("excluded_nicks").Split("\n", nlines, false);
        for (const auto& raw : nlines) {
            CString t = raw.Trim_n();
            if (t.empty()) continue;
            NickExclude ne;
            if (!MakeNickExclude(t, ne)) continue;
            m_excluded_nicks.push_back(std::move(ne));
        }
    }

    void SaveIgnoreMode() {
        SetNV("require_ignore_drop_mode", ignore_mode_to_storage(m_require_ignore_mode));
        SetNV("require_ignore_drop", (m_require_ignore_mode == ERequireIgnoreMode::On) ? "1" : "0");
    }

    bool ApplyLoadArgs(const CString& sArgs, CString& sError) {
        VCString parts;
        sArgs.Split(" ", parts, false);
        for (const auto& raw : parts) {
            CString part = raw.Trim_n();
            if (part.empty()) continue;
            CString key = part.Token(0, false, "=").AsLower();
            CString val = part.Token(1, true, "=");
            if (key.empty() || val.empty()) {
                sError = "Invalid load arg syntax. Use key=value pairs like before=8 after=8 require_ignore_drop=auto excludes=#chan1,#chan2";
                return false;
            }

            if (key == "before") {
                unsigned int n = 0;
                if (!parse_uint_cstr(val, n)) {
                    sError = "Invalid value for before.";
                    return false;
                }
                m_before_max = n;
                SetNV("before_max", CString(std::to_string(m_before_max)));
            } else if (key == "after") {
                unsigned int n = 0;
                if (!parse_uint_cstr(val, n)) {
                    sError = "Invalid value for after.";
                    return false;
                }
                m_after_max = n;
                SetNV("after_max", CString(std::to_string(m_after_max)));
            } else if (key == "require_ignore_drop") {
                ERequireIgnoreMode mode = ERequireIgnoreMode::Auto;
                if (!parse_ignore_mode(val, mode)) {
                    sError = "Invalid value for require_ignore_drop; use off, on, or auto.";
                    return false;
                }
                m_require_ignore_mode = mode;
                SaveIgnoreMode();
            } else if (key == "max_events") {
                CString vl = val.AsLower();
                if (vl == "off" || vl == "no" || vl == "false" || vl == "disable" || vl == "disabled") {
                    m_max_events = 0;
                } else {
                    unsigned int n = 0;
                    if (!parse_uint_cstr(val, n)) {
                        sError = "Invalid value for max_events; use a number or off/0 to disable.";
                        return false;
                    }
                    m_max_events = n;
                }
                SetNV("max_events", CString(std::to_string(m_max_events)));
            } else if (key == "excludes") {
                m_excluded.clear();
                m_excluded_nicks.clear();
                VCString items;
                val.Split(",", items, false);
                for (const auto& c : items) {
                    CString item = c.Trim_n();
                    if (item.empty()) continue;
                    if (is_channel_name(item)) {
                        m_excluded.insert(lc(item));
                    } else {
                        NickExclude ne;
                        if (MakeNickExclude(item, ne)) {
                            m_excluded_nicks.push_back(std::move(ne));
                        }
                        // Silently skip malformed mask tokens on load-args path;
                        // the interactive AddExclude gives richer errors.
                    }
                }
                SaveExcludes();
                SaveExcludedNicks();
            } else {
                sError = "Unknown load arg key: ";
                sError += key;
                return false;
            }
        }
        return true;
    }

    void SaveExcludes() {
        CString out;
        bool first = true;
        for (const auto& c : m_excluded) {
            if (!first) out += "\n";
            first = false;
            out += c.c_str();
        }
        SetNV("excluded_channels", out);
    }

    void SaveExcludedNicks() {
        CString out;
        bool first = true;
        for (const auto& ne : m_excluded_nicks) {
            if (!first) out += "\n";
            first = false;
            out += ne.mask_folded.c_str();
        }
        SetNV("excluded_nicks", out);
    }

    // Build a NickExclude from a user-supplied token. Applies the same
    // validation rules as ignore_drop so behavior is predictable across the
    // two modules: reject empty, reject embedded CR/LF/NUL (would corrupt
    // the newline-delimited NV storage), and otherwise classify as nick-only
    // vs full-mask based on presence of ! or @.
    // Returns true on success and populates `out`; returns false otherwise.
    // The optional `err` param receives a human-readable reason on failure.
    static bool MakeNickExclude(const CString& in, NickExclude& out,
                                CString* err = nullptr) {
        CString t = in.Trim_n();
        if (t.empty()) {
            if (err) *err = "empty mask";
            return false;
        }
        for (char c : std::string(t.c_str())) {
            if (c == '\r' || c == '\n' || c == '\0') {
                if (err) *err = "mask contains control character (CR/LF/NUL)";
                return false;
            }
        }
        const std::string raw(t.c_str());
        const bool has_bang = raw.find('!') != std::string::npos;
        const bool has_at   = raw.find('@') != std::string::npos;
        out.nick_only = !(has_bang || has_at);
        out.mask_folded = rfc1459_fold(raw);
        return true;
    }

    // Does any configured nick exclusion match this sender?
    // `nick_sample` is the sender's nickname; `full_sample` is nick!ident@host.
    // Both are expected to be RFC 1459-folded by the caller.
    bool IsNickExcluded_folded(const std::string& nick_sample,
                               const std::string& full_sample) const {
        for (const auto& ne : m_excluded_nicks) {
            const std::string& sample = ne.nick_only ? nick_sample : full_sample;
            if (wildmatch_folded(ne.mask_folded, sample)) return true;
        }
        return false;
    }

    // Convenience wrapper for the hot path: builds folded samples once from
    // a CaptureLine's nick and the ZNC Message's full prefix (when available).
    template <typename TMsg>
    bool IsSenderExcluded(const TMsg& Message, const CString& nick_raw) const {
        if (m_excluded_nicks.empty()) return false;
        std::string nick_folded = rfc1459_fold(std::string(nick_raw.c_str()));
        // Prefer the message's full nick!ident@host form; fall back to nick-only.
        std::string full;
        const CNick& n = Message.GetNick();
        CString h = n.GetHost();
        CString i = n.GetIdent();
        if (!i.empty() || !h.empty()) {
            full.reserve(nick_raw.size() + i.size() + h.size() + 2);
            full += nick_raw.c_str();
            full += '!';
            full += i.c_str();
            full += '@';
            full += h.c_str();
        } else {
            full = nick_raw.c_str();
        }
        std::string full_folded = rfc1459_fold(full);
        return IsNickExcluded_folded(nick_folded, full_folded);
    }

    bool HasIgnoreDropLoaded() const {
        if (!GetNetwork()) return false;
        return (GetNetwork()->GetModules().FindModule("ignore_drop") != nullptr);
    }

    // Returns true iff ignore_drop is currently in the network module list AND
    // its position is ahead of ours (i.e. its hooks will be dispatched before
    // ours). Returns false if either module is missing from the iteration,
    // or if ignore_drop is positioned at or after our own position.
    bool IsIgnoreDropAheadOfUs() const {
        if (!GetNetwork()) return false;
        const CModules& mods = GetNetwork()->GetModules();
        int my_pos = -1;
        int their_pos = -1;
        int i = 0;
        for (CModule* pMod : mods) {
            if (pMod) {
                if (pMod == this) {
                    my_pos = i;
                } else if (pMod->GetModName().Equals("ignore_drop")) {
                    their_pos = i;
                }
            }
            ++i;
        }
        return (my_pos >= 0 && their_pos >= 0 && their_pos < my_pos);
    }

    // Unified hook-order re-check used by OnBoot and Rearm. Updates
    // m_ignore_drop_present_on_module_load (which despite its legacy name is
    // now semantically "ignore_drop was ahead of us in hook order at last
    // check") and recomputes the auto-armed runtime state.
    void RecheckIgnoreDropHookOrder() {
        m_ignore_drop_present_on_module_load = IsIgnoreDropAheadOfUs();
        RecomputeIgnoreDropRuntimeState();
    }

    void RecomputeIgnoreDropRuntimeState() {
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto) {
            m_auto_ignore_drop_armed = m_ignore_drop_present_on_module_load;
        } else {
            m_auto_ignore_drop_armed = false;
        }
    }

    bool IsIgnoreDropRequirementEffective() const {
        switch (m_require_ignore_mode) {
            case ERequireIgnoreMode::Off:
                return false;
            case ERequireIgnoreMode::On:
                return true;
            case ERequireIgnoreMode::Auto:
                return m_auto_ignore_drop_armed;
        }
        return false;
    }

    CString IgnoreModeName() const {
        return ignore_mode_to_cstring(m_require_ignore_mode);
    }

    bool ShouldCaptureNow() const {
        if (!GetNetwork()) return false;
        if (GetNetwork()->IsUserAttached()) return false;
        if (IsIgnoreDropRequirementEffective() && !HasIgnoreDropLoaded()) return false;
        return true;
    }

    bool IsExcluded(const CString& channel) const {
        return (m_excluded.find(lc(channel)) != m_excluded.end());
    }

    void EnsureJournalPath() {
        m_journal_path = std::string(GetSavePath().c_str());
        if (!m_journal_path.empty() && m_journal_path.back() != '/') m_journal_path.push_back('/');
        m_journal_path += kJournalName;
    }

    static std::string SerializeLine(const CaptureLine& line) {
        const std::string nick_hex = hex_encode(std::string(line.nick.c_str()));
        const std::string text_hex = hex_encode(std::string(line.text.c_str()));
        std::string raw = std::to_string(line.ts_sec) + ";" + std::string(1, line.kind) + ";" + nick_hex + ";" + text_hex;
        return hex_encode(raw);
    }

    static bool DeserializeLine(const std::string& rec_hex, CaptureLine& out) {
        std::string raw;
        if (!hex_decode(rec_hex, raw)) return false;
        std::vector<std::string> parts = split_char(raw, ';');
        if (parts.size() != 4) return false;

        char* endp = nullptr;
        long long ts = std::strtoll(parts[0].c_str(), &endp, 10);
        if (!endp || *endp != '\0') return false;
        if (parts[1].size() != 1) return false;

        std::string nick, text;
        if (!hex_decode(parts[2], nick) || !hex_decode(parts[3], text)) return false;

        out.ts_sec = ts;
        out.kind = parts[1][0];
        out.nick = nick.c_str();
        out.text = text.c_str();
        return true;
    }

    static std::string SerializeBegin(const Event& ev) {
        std::vector<std::string> before_hex;
        before_hex.reserve(ev.before.size());
        for (const auto& line : ev.before) before_hex.push_back(SerializeLine(line));

        return std::string("B\t") + std::to_string(ev.id) + "\t" +
               hex_encode(std::string(ev.channel.c_str())) + "\t" +
               std::to_string(ev.started_ts) + "\t" +
               std::to_string(ev.after_cap) + "\t" +
               join_char(before_hex, ',') + "\t" +
               SerializeLine(ev.trigger);
    }

    static std::string SerializeAfter(unsigned long long id, const CaptureLine& line) {
        return std::string("A\t") + std::to_string(id) + "\t" + SerializeLine(line);
    }

    static std::string SerializeFinalize(unsigned long long id, bool partial) {
        return std::string("F\t") + std::to_string(id) + "\t" + (partial ? "1" : "0");
    }

    static std::string SerializeDelivered(unsigned long long id) {
        return std::string("D\t") + std::to_string(id);
    }

    bool AppendJournal(const std::string& line) {
        if (m_journal_path.empty()) EnsureJournalPath();
        bool ok = durable_append_line(m_journal_path, line);
        if (ok) ++m_journal_line_count;
        return ok;
    }

    bool LoadJournal(CString& sError) {
        std::vector<std::string> lines;
        if (!slurp_lines(m_journal_path, lines)) {
            m_journal_line_count = 0;
            return true;
        }
        m_journal_line_count = lines.size();

        std::map<unsigned long long, Event> all;
        unsigned long long max_id = 0;

        for (const auto& line : lines) {
            if (line.empty()) continue;
            std::vector<std::string> parts = split_char(line, '\t');
            if (parts.empty()) continue;

            const std::string& op = parts[0];
            if (op == "B") {
                if (parts.size() != 7) continue;
                char* endp1 = nullptr;
                char* endp2 = nullptr;
                char* endp3 = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp1, 10);
                long long started = std::strtoll(parts[3].c_str(), &endp2, 10);
                unsigned long after_cap_ul = std::strtoul(parts[4].c_str(), &endp3, 10);
                if (!endp1 || *endp1 != '\0' || !endp2 || *endp2 != '\0' || !endp3 || *endp3 != '\0') continue;

                std::string chan;
                if (!hex_decode(parts[2], chan)) continue;

                Event ev;
                ev.id = id;
                ev.channel = chan.c_str();
                ev.channel_lc = lc(ev.channel);
                ev.started_ts = started;
                ev.after_cap = static_cast<unsigned int>(after_cap_ul);

                if (!parts[5].empty()) {
                    std::vector<std::string> before_hex = split_char(parts[5], ',');
                    for (const auto& bh : before_hex) {
                        if (bh.empty()) continue;
                        CaptureLine cl;
                        if (!DeserializeLine(bh, cl)) continue;
                        ev.before.push_back(cl);
                    }
                }

                if (!DeserializeLine(parts[6], ev.trigger)) continue;
                all[id] = ev;
                if (id > max_id) max_id = id;
            } else if (op == "A") {
                if (parts.size() != 3) continue;
                char* endp = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp, 10);
                if (!endp || *endp != '\0') continue;
                auto it = all.find(id);
                if (it == all.end()) continue;
                CaptureLine cl;
                if (!DeserializeLine(parts[2], cl)) continue;
                it->second.after.push_back(cl);
                if (id > max_id) max_id = id;
            } else if (op == "F") {
                if (parts.size() != 3) continue;
                char* endp = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp, 10);
                if (!endp || *endp != '\0') continue;
                auto it = all.find(id);
                if (it == all.end()) continue;
                it->second.finalized = true;
                it->second.partial = (parts[2] == "1");
                if (id > max_id) max_id = id;
            } else if (op == "D") {
                if (parts.size() != 2) continue;
                char* endp = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp, 10);
                if (!endp || *endp != '\0') continue;
                all.erase(id);
                if (id > max_id) max_id = id;
            }
        }

        m_open_by_chan.clear();
        m_pending.clear();
        for (auto& kv : all) {
            Event& ev = kv.second;
            if (ev.finalized) {
                m_pending.push_back(ev);
            } else {
                m_open_by_chan[ev.channel_lc].push_back(ev);
            }
        }

        m_next_id = max_id + 1;
        if (m_journal_line_count > kCompactThresholdLines) {
            if (!CompactJournalInternal()) {
                sError = "highlightctx loaded, but journal compaction failed; continuing with existing state.";
            }
        }
        return true;
    }

    bool CompactJournalInternal() {
        std::string out;
        auto append_line = [&](const std::string& line) {
            out += line;
            out.push_back('\n');
        };

        size_t lines_written = 0;
        for (const auto& by_chan : m_open_by_chan) {
            for (const auto& ev : by_chan.second) {
                append_line(SerializeBegin(ev));
                ++lines_written;
                for (const auto& line : ev.after) {
                    append_line(SerializeAfter(ev.id, line));
                    ++lines_written;
                }
            }
        }
        for (const auto& ev : m_pending) {
            append_line(SerializeBegin(ev));
            ++lines_written;
            for (const auto& line : ev.after) {
                append_line(SerializeAfter(ev.id, line));
                ++lines_written;
            }
            append_line(SerializeFinalize(ev.id, ev.partial));
            ++lines_written;
        }

        bool ok = durable_replace_file(m_journal_path, out);
        if (ok) m_journal_line_count = lines_written;
        return ok;
    }

    void TrimAllRings() {
        for (auto& kv : m_ring_by_chan) {
            auto& dq = kv.second;
            while (dq.size() > m_before_max) dq.pop_front();
        }
    }

    void ClearVolatileState() {
        m_ring_by_chan.clear();
    }

    unsigned long long AllocEventId() {
        return m_next_id++;
    }

    CString CurrentNick() const {
        if (!GetNetwork()) return "";
        return GetNetwork()->GetCurNick();
    }

    template <typename TMsg>
    CaptureLine MakeCaptureLine(const TMsg& Message, char kind) const {
        CaptureLine line;
        line.ts_sec = Message.GetTime().tv_sec;
        line.kind = kind;
        line.nick = Message.GetNick().GetNick();
        line.text = Message.GetText();
        return line;
    }

    static CString FormatLineForReplay(const CString& channel, const CaptureLine& line, bool mark_trigger, bool add_inline_ts) {
        CString prefix;
        if (add_inline_ts) {
            prefix += "[";
            prefix += format_hms_utc(line.ts_sec).c_str();
            prefix += " UTC] ";
        }
        prefix += "[";
        prefix += channel;
        prefix += "] ";
        if (mark_trigger) prefix += ">>> ";

        if (line.kind == 'A') {
            CString out = prefix;
            out += "* ";
            out += line.nick;
            out += " ";
            out += line.text;
            return out;
        }
        if (line.kind == 'N') {
            CString out = prefix;
            out += "-";
            out += line.nick;
            out += "- ";
            out += line.text;
            return out;
        }
        CString out = prefix;
        out += "<";
        out += line.nick;
        out += "> ";
        out += line.text;
        return out;
    }

    bool ClientSupportsNativeServerTime(CClient* pClient) const {
        if (!pClient) return false;
        return pClient->IsTagEnabled("time") || pClient->IsCapEnabled("server-time");
    }

    CString EscapeMsgTagValue(const CString& in) const {
        CString out;
        for (const char c : std::string(in.c_str())) {
            switch (c) {
                case ';': out += "\\:"; break;
                case ' ': out += "\\s"; break;
                case '\\': out += "\\\\"; break;
                case '\r': break;
                case '\n': break;
                default: out += c; break;
            }
        }
        return out;
    }

    CString BuildReplayRawPrivmsg(const CString& body, long long ts_sec, bool with_time_tag, CClient* pClient) const {
        CString line;
        if (with_time_tag) {
            line += "@time=";
            line += format_iso8601_utc(ts_sec).c_str();
            line += " ";
        }

        CString ident = "*";
        ident += GetModName();
        ident += "!znc@znc.in";

        line += ":";
        line += ident;
        line += " PRIVMSG ";
        line += pClient->GetNick();
        line += " :";
        line += CString(sanitize_irc_text(body).c_str());
        return line;
    }

    void ReplayLineToClient(const CString& body, long long ts_sec) {
        CClient* pClient = GetClient();
        if (pClient && ClientSupportsNativeServerTime(pClient)) {
            PutUser(BuildReplayRawPrivmsg(body, ts_sec, true, pClient));
        } else {
            CString fallback = "[";
            fallback += format_iso8601_utc(ts_sec).c_str();
            fallback += "] ";
            fallback += body;
            PutModule(fallback);
        }
    }

    void FinalizeEvent(Event& ev, bool partial) {
        ev.finalized = true;
        ev.partial = partial;
        m_pending.push_back(ev);
        AppendJournal(SerializeFinalize(ev.id, partial));

        // If max_events is enabled, drop the oldest pending event(s) to stay within the cap.
        if (m_max_events > 0) {
            while (m_pending.size() > m_max_events) {
                AppendJournal(SerializeDelivered(m_pending.front().id));
                m_pending.erase(m_pending.begin());
            }
        }
    }

    void FinalizeAllOpenAsPartial() {
        std::map<std::string, std::vector<Event>> still_open;
        for (auto& kv : m_open_by_chan) {
            for (auto& ev : kv.second) {
                FinalizeEvent(ev, true);
            }
        }
        m_open_by_chan.swap(still_open);
    }

    void MaybeCompactAfterMutation() {
        if (m_journal_line_count > kCompactThresholdLines) {
            CompactJournalInternal();
        }
    }

    void StartEvent(const CString& channel, const CaptureLine& trigger_line) {
        Event ev;
        ev.id = AllocEventId();
        ev.channel = channel;
        ev.channel_lc = lc(channel);
        ev.started_ts = trigger_line.ts_sec;
        ev.after_cap = m_after_max;
        ev.trigger = trigger_line;

        auto rit = m_ring_by_chan.find(ev.channel_lc);
        if (rit != m_ring_by_chan.end()) {
            ev.before.assign(rit->second.begin(), rit->second.end());
        }

        if (!AppendJournal(SerializeBegin(ev))) {
            PutModule("warning: could not durably journal a new highlight event begin record.");
        }

        if (ev.after_cap == 0) {
            FinalizeEvent(ev, false);
        } else {
            m_open_by_chan[ev.channel_lc].push_back(ev);
        }
    }

    void FeedOpenEvents(const CString& channel, const CaptureLine& line) {
        const std::string chan_l = lc(channel);
        auto it = m_open_by_chan.find(chan_l);
        if (it == m_open_by_chan.end()) return;

        std::vector<Event> survivors;
        survivors.reserve(it->second.size());

        for (auto& ev : it->second) {
            ev.after.push_back(line);
            AppendJournal(SerializeAfter(ev.id, line));
            if (ev.after.size() >= ev.after_cap) {
                FinalizeEvent(ev, false);
            } else {
                survivors.push_back(ev);
            }
        }

        if (survivors.empty()) {
            m_open_by_chan.erase(it);
        } else {
            it->second.swap(survivors);
        }
    }

    template <typename TMsg>
    void HandleIncoming(TMsg& Message, char kind) {
        if (!ShouldCaptureNow()) return;
        CChan* pChan = Message.GetChan();
        if (!pChan) return;

        const CString channel = pChan->GetName();
        if (channel.empty() || IsExcluded(channel)) return;

        CaptureLine line = MakeCaptureLine(Message, kind);

        // Feed already-open events first: excluded-nick messages are STILL
        // valid context for other events that were triggered by someone
        // else. This is the intentional difference from channel exclusion,
        // which drops the channel entirely.
        FeedOpenEvents(channel, line);

        CString mynick = CurrentNick();
        bool is_self = (lc(line.nick) == lc(mynick));
        if (!is_self && contains_nick_highlight(line.text, mynick)) {
            // Nick-exclusion gate: an excluded sender cannot START a new
            // event, but their earlier messages are still in this channel's
            // ring buffer and are still being fed into any existing events.
            if (!IsSenderExcluded(Message, line.nick)) {
                StartEvent(channel, line);
            }
        }

        // The ring buffer always receives every eligible channel line,
        // including from excluded senders, so their messages remain
        // available as 'before' context for any future trigger.
        auto& dq = m_ring_by_chan[lc(channel)];
        dq.push_back(line);
        while (dq.size() > m_before_max) dq.pop_front();

        MaybeCompactAfterMutation();
    }

    void ReplayAndClear(bool manual) {
        FinalizeAllOpenAsPartial();
        if (m_pending.empty()) {
            if (manual) PutModule("No pending highlight events.");
            CompactJournalInternal();
            return;
        }

        std::sort(m_pending.begin(), m_pending.end(), [](const Event& a, const Event& b) {
            if (a.channel_lc != b.channel_lc) return a.channel_lc < b.channel_lc;
            if (a.started_ts != b.started_ts) return a.started_ts < b.started_ts;
            return a.id < b.id;
        });

        bool first = true;
        for (const auto& ev : m_pending) {
            if (!first) {
                ReplayLineToClient("------------------------------------------------------------", ev.started_ts);
            }
            first = false;

            CString header = "[";
            header += ev.channel;
            header += "] highlight event #";
            header += CString(std::to_string(ev.id));
            header += " (";
            header += (ev.partial ? "partial" : "complete");
            header += ", before=";
            header += CString(std::to_string(ev.before.size()));
            header += ", after=";
            header += CString(std::to_string(ev.after.size()));
            header += "/";
            header += CString(std::to_string(ev.after_cap));
            header += ")";
            ReplayLineToClient(header, ev.started_ts);

            for (const auto& line : ev.before) {
                ReplayLineToClient(FormatLineForReplay(ev.channel, line, false, false), line.ts_sec);
            }
            ReplayLineToClient(FormatLineForReplay(ev.channel, ev.trigger, true, false), ev.trigger.ts_sec);
            for (const auto& line : ev.after) {
                ReplayLineToClient(FormatLineForReplay(ev.channel, line, false, false), line.ts_sec);
            }
        }

        for (const auto& ev : m_pending) AppendJournal(SerializeDelivered(ev.id));
        m_pending.clear();
        CompactJournalInternal();
    }

    void CmdOverview() {
        PutModule(CString("Version marker: ") + kModVersion);
        PutModule("highlightctx is a detached-only network module that captures its own highlight context using live incoming channel message hooks only.");
        PutModule("It does not inspect the normal playback buffer, so it remains independent of normal channel buffer length and ordinary buffer replay settings.");
        PutModule("When a channel line mentions your current network nick while this network is detached, the module snapshots up to <before> earlier lines from its own per-channel RAM ring and then starts collecting up to <after> later lines.");
        PutModule("Both values are maximum caps, not guarantees. If you attach before enough later lines arrive, the event is replayed as partial with whatever was already recorded.");
        PutModule("Replay target: *highlightctx. When the current client supports IRCv3 server-time/time tags, replay is emitted as synthetic raw PRIVMSG lines with original @time values so the client can display historical timestamps natively. If the client does not support that, replay falls back to text prefixed with the original UTC timestamp.");
        PutModule("Events are sorted by channel name first, then by event time/id within each channel. A spacer line is added between events for readability.");
        PutModule("Persistence strategy: ordinary chatter stays only in RAM, while actual highlight events are durably journaled to disk as they happen. That keeps the hot path light but preserves active highlight captures across an unexpected VPS shutdown.");
        PutModule("Exclusions: AddExclude accepts both channel names (starting with #, &, +, or !) and nick/hostmask patterns. Channel exclusions drop the channel entirely. Nick/mask exclusions are narrower: messages from the excluded sender still appear as before/after context for other events, but cannot start a new event themselves. Masks use RFC 1459 case folding and support * and ? wildcards, matching the syntax used by ignore_drop. A mask containing ! or @ is matched against the full nick!ident@host; otherwise it is matched against the nickname only.");
        PutModule("ignore_drop modes: off = never required. on = must already be loaded before highlightctx loads and capture pauses whenever it is absent. auto = the same strict behavior is armed only if ignore_drop is positioned ahead of highlightctx in the network's module list so its hooks fire before ours. Arming is re-evaluated at OnLoad, at OnBoot for znc.conf-loaded modules, and on demand via Rearm. Runtime load or unload of ignore_drop does not automatically re-arm; ZNC dispatches those lifecycle hooks to global-scope modules only, so Rearm is the supported way to refresh the state after a runtime change. Runtime capture is always protected by an independent HasIgnoreDropLoaded() check inside ShouldCaptureNow(), so when the strict requirement is effective but ignore_drop is absent, capture pauses regardless of the armed flag. Armed state is sticky by design: once armed, it stays armed across ignore_drop unload/reload so capture does not silently resume without the protection the user asked for.");
        PutModule("Primary commands: Status, SetBefore, SetAfter, SetMaxEvents, AddExclude, DelExclude, ListExcludes, SetRequireIgnoreDrop, Rearm, Reset, ReplayNow, Compact, ClearPending.");
    }

    void CmdStatus() {
        size_t open_count = 0;
        for (const auto& kv : m_open_by_chan) open_count += kv.second.size();

        PutModule(CString("Version marker: ") + kModVersion);
        PutModule(CString("Detached-only capture active now: ") + (ShouldCaptureNow() ? "yes" : "no"));
        PutModule(CString("Network attached right now: ") + ((GetNetwork() && GetNetwork()->IsUserAttached()) ? "yes" : "no"));
        PutModule(CString("before cap: ") + CString(std::to_string(m_before_max)));
        PutModule(CString("after cap: ") + CString(std::to_string(m_after_max)));
        PutModule(CString("max_events: ") + (m_max_events == 0 ? "disabled" : CString(std::to_string(m_max_events)).c_str()));
        PutModule(CString("require_ignore_drop mode: ") + IgnoreModeName());
        PutModule(CString("ignore_drop ahead of highlightctx in hook order: ") + (m_ignore_drop_present_on_module_load ? "yes" : "no"));
        PutModule(CString("auto mode armed: ") + ((m_require_ignore_mode == ERequireIgnoreMode::Auto && m_auto_ignore_drop_armed) ? "yes" : "no"));
        PutModule(CString("effective ignore_drop requirement now: ") + (IsIgnoreDropRequirementEffective() ? "yes" : "no"));
        PutModule(CString("ignore_drop currently loaded: ") + (HasIgnoreDropLoaded() ? "yes" : "no"));
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto && !m_auto_ignore_drop_armed && HasIgnoreDropLoaded()) {
            PutModule("note: ignore_drop is loaded now, but auto mode is not armed because ignore_drop is positioned at or after highlightctx in the network module list, so its hooks fire after ours. Try Rearm to re-check; if that still reports unarmed, unload and reload highlightctx so it ends up after ignore_drop in the list, or fix the LoadModule order in znc.conf for the next restart.");
        }
        if (GetClient()) {
            PutModule(CString("current client native server-time replay support: ") + (ClientSupportsNativeServerTime(GetClient()) ? "yes" : "no"));
        }
        PutModule(CString("excluded channels: ") + CString(std::to_string(m_excluded.size())));
        PutModule(CString("excluded nicks/masks: ") + CString(std::to_string(m_excluded_nicks.size())));
        PutModule(CString("open events: ") + CString(std::to_string(open_count)));
        PutModule(CString("pending finalized events: ") + CString(std::to_string(m_pending.size())));
        PutModule(CString("journal path: ") + m_journal_path.c_str());
    }

    void CmdSetBefore(const CString& sLine) {
        unsigned int n = 0;
        if (!parse_uint_cstr(sLine.Token(1, false), n)) {
            PutModule("Usage: SetBefore <count>");
            return;
        }
        m_before_max = n;
        SetNV("before_max", CString(std::to_string(m_before_max)));
        TrimAllRings();
        PutModule(CString("before cap set to ") + CString(std::to_string(m_before_max)));
    }

    void CmdSetAfter(const CString& sLine) {
        unsigned int n = 0;
        if (!parse_uint_cstr(sLine.Token(1, false), n)) {
            PutModule("Usage: SetAfter <count>");
            return;
        }
        m_after_max = n;
        SetNV("after_max", CString(std::to_string(m_after_max)));
        PutModule(CString("after cap set to ") + CString(std::to_string(m_after_max)) +
                  " (new events use the new cap; already-open events keep the cap they started with)");
    }

    void CmdAddExclude(const CString& sLine) {
        CString tok = sLine.Token(1, true).Trim_n();
        if (tok.empty()) {
            PutModule("Usage: AddExclude <#channel|nick|mask>");
            return;
        }
        if (is_channel_name(tok)) {
            // Channel path — unchanged from 0.7.0 semantics.
            const std::string key = lc(tok);
            auto ins = m_excluded.insert(key);
            SaveExcludes();
            CString msg = ins.second ? "Excluded channel: " : "Channel already excluded: ";
            msg += tok;
            PutModule(msg);
            return;
        }
        // Nick-exclusion path.
        NickExclude ne;
        CString err;
        if (!MakeNickExclude(tok, ne, &err)) {
            CString msg = "Rejected nick/mask exclusion '";
            msg += tok;
            msg += "': ";
            msg += err;
            PutModule(msg);
            return;
        }
        // Reject exact duplicates (folded mask comparison).
        for (const auto& existing : m_excluded_nicks) {
            if (existing.mask_folded == ne.mask_folded && existing.nick_only == ne.nick_only) {
                CString msg = "Nick/mask already excluded: ";
                msg += tok;
                PutModule(msg);
                return;
            }
        }
        m_excluded_nicks.push_back(ne);
        SaveExcludedNicks();
        CString msg = "Excluded ";
        msg += (ne.nick_only ? "nick: " : "mask: ");
        msg += tok;
        msg += " (stored as: ";
        msg += ne.mask_folded.c_str();
        msg += ")";
        PutModule(msg);
    }

    void CmdDelExclude(const CString& sLine) {
        CString tok = sLine.Token(1, true).Trim_n();
        if (tok.empty()) {
            PutModule("Usage: DelExclude <#channel|nick|mask|index>");
            return;
        }

        // Numeric index path — index into the same numbered order that
        // ListExcludes prints: channels first (sorted), then nicks (insertion order).
        {
            unsigned int idx = 0;
            if (parse_uint_cstr(tok, idx) && idx > 0) {
                // Reproduce the listing order from ListExcludes.
                std::vector<CString> chans;
                chans.reserve(m_excluded.size());
                for (const auto& c : m_excluded) chans.push_back(c.c_str());
                std::sort(chans.begin(), chans.end());

                const size_t nchans = chans.size();
                const size_t nnicks = m_excluded_nicks.size();
                if (idx <= nchans) {
                    CString removed = chans[idx - 1];
                    m_excluded.erase(std::string(removed.c_str()));
                    SaveExcludes();
                    CString msg = "Removed channel exclusion: ";
                    msg += removed;
                    PutModule(msg);
                    return;
                }
                size_t nick_idx = idx - nchans;
                if (nick_idx >= 1 && nick_idx <= nnicks) {
                    NickExclude gone = m_excluded_nicks[nick_idx - 1];
                    m_excluded_nicks.erase(m_excluded_nicks.begin() + (nick_idx - 1));
                    SaveExcludedNicks();
                    CString msg = "Removed ";
                    msg += (gone.nick_only ? "nick exclusion: " : "mask exclusion: ");
                    msg += gone.mask_folded.c_str();
                    PutModule(msg);
                    return;
                }
                PutModule(CString("Index ") + tok + " is out of range.");
                return;
            }
        }

        // Channel-name path.
        if (is_channel_name(tok)) {
            auto it = m_excluded.find(lc(tok));
            if (it == m_excluded.end()) {
                PutModule("That channel is not currently excluded.");
                return;
            }
            m_excluded.erase(it);
            SaveExcludes();
            CString msg = "Removed channel exclusion: ";
            msg += tok;
            PutModule(msg);
            return;
        }

        // Nick/mask path: match against the folded form.
        const std::string target = rfc1459_fold(tok);
        for (auto it = m_excluded_nicks.begin(); it != m_excluded_nicks.end(); ++it) {
            if (it->mask_folded == target) {
                NickExclude gone = *it;
                m_excluded_nicks.erase(it);
                SaveExcludedNicks();
                CString msg = "Removed ";
                msg += (gone.nick_only ? "nick exclusion: " : "mask exclusion: ");
                msg += tok;
                PutModule(msg);
                return;
            }
        }
        PutModule("That nick/mask is not currently excluded.");
    }

    void CmdListExcludes() {
        if (m_excluded.empty() && m_excluded_nicks.empty()) {
            PutModule("No exclusions configured.");
            return;
        }
        size_t i = 0;
        // Channels first (sorted). This matches the numbering used by
        // DelExclude <index>.
        std::vector<CString> chans;
        chans.reserve(m_excluded.size());
        for (const auto& c : m_excluded) chans.push_back(c.c_str());
        std::sort(chans.begin(), chans.end());
        for (const auto& c : chans) {
            ++i;
            PutModule(CString(std::to_string(i)) + ") " + c + " [channel]");
        }
        // Nick/mask exclusions in insertion order.
        for (const auto& ne : m_excluded_nicks) {
            ++i;
            CString line = CString(std::to_string(i)) + ") " + ne.mask_folded.c_str()
                         + " [" + (ne.nick_only ? "nick" : "mask") + "]";
            PutModule(line);
        }
    }

    void CmdSetRequireIgnoreDrop(const CString& sLine) {
        ERequireIgnoreMode mode = ERequireIgnoreMode::Auto;
        if (!parse_ignore_mode(sLine.Token(1, false), mode)) {
            PutModule("Usage: SetRequireIgnoreDrop <off|on|auto>");
            return;
        }
        if (mode == ERequireIgnoreMode::On && !HasIgnoreDropLoaded()) {
            PutModule("Cannot set require_ignore_drop to on because ignore_drop is not currently loaded on this network.");
            return;
        }
        m_require_ignore_mode = mode;
        RecomputeIgnoreDropRuntimeState();
        SaveIgnoreMode();

        CString msg = "require_ignore_drop mode set to ";
        msg += IgnoreModeName();
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto) {
            msg += " (armed=";
            msg += (m_auto_ignore_drop_armed ? "yes" : "no");
            msg += ")";
            if (!m_auto_ignore_drop_armed && HasIgnoreDropLoaded()) {
                msg += " | note: ignore_drop is loaded now but auto mode is unarmed because ignore_drop is positioned at or after highlightctx in the module list; try Rearm to re-check, or unload and reload highlightctx to move it after ignore_drop.";
            }
        }
        PutModule(msg);
    }

    void CmdRearm() {
        // Re-check ignore_drop presence and hook-order position on demand.
        // This updates the armed flag but CANNOT fix hook order by itself —
        // hook dispatch order is a function of module-list position, which is
        // fixed at the time each module was added to the list. The user-
        // visible value here is (a) diagnosing the current state accurately
        // without a module reload, and (b) clearing a stale "unarmed" state
        // if something has changed (e.g. ignore_drop was unloaded and then
        // re-loaded in the correct order at runtime via LoadMod).
        if (!GetNetwork()) {
            PutModule("Cannot rearm: no network context.");
            return;
        }

        const bool was_armed = m_auto_ignore_drop_armed;
        RecheckIgnoreDropHookOrder();

        const bool now_loaded = HasIgnoreDropLoaded();
        const bool now_ahead  = m_ignore_drop_present_on_module_load;

        PutModule(CString("ignore_drop currently loaded: ") + (now_loaded ? "yes" : "no"));
        PutModule(CString("ignore_drop ahead of highlightctx in hook order: ") + (now_ahead ? "yes" : "no"));
        PutModule(CString("require_ignore_drop mode: ") + IgnoreModeName());
        PutModule(CString("auto mode armed: ") + ((m_require_ignore_mode == ERequireIgnoreMode::Auto && m_auto_ignore_drop_armed) ? "yes" : "no"));
        PutModule(CString("effective ignore_drop requirement now: ") + (IsIgnoreDropRequirementEffective() ? "yes" : "no"));

        if (m_require_ignore_mode == ERequireIgnoreMode::Auto) {
            if (!now_loaded) {
                PutModule("Rearm result: not armed (ignore_drop is not loaded on this network).");
            } else if (!now_ahead) {
                PutModule("Rearm result: not armed. ignore_drop is loaded but positioned at or after highlightctx in the module list, so its hooks fire after ours. To fix hook order: UnloadMod highlightctx then LoadMod --type=network highlightctx, or reorder the LoadModule lines in znc.conf for the next restart.");
            } else {
                PutModule("Rearm result: armed. ignore_drop is ahead of highlightctx in hook order.");
            }
        } else {
            PutModule("Note: require_ignore_drop mode is not 'auto'; Rearm updates the ahead-in-hook-order state for diagnostics but does not change behavior outside auto mode.");
        }

        if (was_armed && !m_auto_ignore_drop_armed) {
            PutModule("Warning: auto mode transitioned from armed to unarmed as a result of this re-check.");
        } else if (!was_armed && m_auto_ignore_drop_armed) {
            PutModule("auto mode transitioned from unarmed to armed as a result of this re-check.");
        }
    }

    void CmdSetMaxEvents(const CString& sLine) {
        CString val = sLine.Token(1, false).Trim_n();
        if (val.empty()) {
            PutModule("Usage: SetMaxEvents <count|0|off>  (0 or off = disabled, no cap on pending events)");
            return;
        }
        CString vl = val.AsLower();
        if (vl == "off" || vl == "no" || vl == "false" || vl == "disable" || vl == "disabled") {
            m_max_events = 0;
        } else {
            unsigned int n = 0;
            if (!parse_uint_cstr(val, n)) {
                PutModule("Usage: SetMaxEvents <count|0|off>  (0 or off = disabled, no cap on pending events)");
                return;
            }
            m_max_events = n;
        }
        SetNV("max_events", CString(std::to_string(m_max_events)));

        if (m_max_events == 0) {
            PutModule("max_events set to disabled (no cap on pending events).");
        } else {
            PutModule(CString("max_events set to ") + CString(std::to_string(m_max_events)) +
                      " (new events will start dropping the oldest once the cap is reached).");
            if (m_pending.size() > m_max_events) {
                size_t to_drop = m_pending.size() - m_max_events;
                for (size_t i = 0; i < to_drop; ++i) {
                    AppendJournal(SerializeDelivered(m_pending.front().id));
                    m_pending.erase(m_pending.begin());
                }
                PutModule(CString("Dropped ") + CString(std::to_string(to_drop)) +
                          " oldest pending event(s) to enforce the new cap immediately.");
                CompactJournalInternal();
            }
        }
    }

    void CmdReset() {
        m_before_max = 8;
        m_after_max = 8;
        m_max_events = 0;
        m_require_ignore_mode = ERequireIgnoreMode::Auto;
        m_excluded.clear();
        m_excluded_nicks.clear();
        RecomputeIgnoreDropRuntimeState();
        TrimAllRings();
        SetNV("before_max", "8");
        SetNV("after_max", "8");
        SetNV("max_events", "0");
        SaveIgnoreMode();
        SaveExcludes();
        SaveExcludedNicks();
        PutModule("All settings reset to defaults: before=8, after=8, max_events=disabled, require_ignore_drop=auto, all exclusions (channel and nick/mask) cleared.");
        PutModule("Pending/open events were not affected. Use ClearPending to discard them.");
    }

    void CmdCompact() {
        if (CompactJournalInternal()) PutModule("Journal compacted.");
        else PutModule("Journal compaction failed.");
    }

    void CmdClearPending() {
        m_open_by_chan.clear();
        m_pending.clear();
        ClearVolatileState();
        if (CompactJournalInternal()) PutModule("Cleared all open/pending highlight events and compacted the journal.");
        else PutModule("Cleared in-memory events, but journal compaction failed.");
    }
};

template<> void TModInfo<CHighlightCtx>(CModInfo& Info) {
    Info.SetWikiPage("highlightctx");
    Info.SetHasArgs(true);
    Info.SetDescription(
        "Detached-only highlight context capture with independent live history, durable active-event journaling, native timestamp replay into *highlightctx when supported, channel and nick/hostmask exclusions, and ignore_drop integration modes off/on/auto."
    );
    Info.AddType(CModInfo::NetworkModule);
}

NETWORKMODULEDEFS(CHighlightCtx, "Detached-only highlight context capture with durable active-event journaling")