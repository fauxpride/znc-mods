// highlightctx.cpp — detached-only highlight context capture for ZNC
// Build:  znc-buildmod highlightctx.cpp
// Load:   /msg *status LoadMod --type=network highlightctx [before=8 after=8 require_ignore_drop=auto excludes=#chan1,#chan2]
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
// - Channel exclusions, configurable before/after caps, verbose commands.
//
// Notes:
// - The "before" and "after" values are maxima, not guarantees.
// - If you attach before enough trailing lines arrive, the event is replayed as partial.
// - If ZNC or the VPS dies mid-capture, the event is recovered from the durable journal
//   and replayed as partial on the next attach if it wasn't fully completed.
// - In auto mode, ignore_drop enforcement is armed only if ignore_drop was already loaded
//   when highlightctx itself loaded. If ignore_drop appears later, reload highlightctx if you
//   want the stricter ignore-aware ordering guarantee.

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

static const char* kModVersion = "highlightctx 0.6.0";
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
                   "Reset all settings to compiled-in defaults: before=8, after=8, max_events=disabled, require_ignore_drop=auto, excludes cleared. Does not discard pending or open events.",
                   [this](const CString&) { CmdReset(); });
        AddCommand("AddExclude", "<#channel>",
                   "Exclude a channel from detached highlight capture entirely. Messages from excluded channels do not trigger or contribute context.",
                   [this](const CString& sLine) { CmdAddExclude(sLine); });
        AddCommand("DelExclude", "<#channel>",
                   "Remove a previously excluded channel so it becomes eligible for detached highlight capture again.",
                   [this](const CString& sLine) { CmdDelExclude(sLine); });
        AddCommand("ListExcludes", "",
                   "List all currently excluded channels for this network instance of highlightctx.",
                   [this](const CString&) { CmdListExcludes(); });
        AddCommand("SetRequireIgnoreDrop", "<off|on|auto>",
                   "Set ignore_drop integration mode. off = never require ignore_drop. on = require ignore_drop to be loaded now and keep capture paused whenever it is absent. auto = arm the same strict behavior only if ignore_drop was already loaded when highlightctx itself loaded; if ignore_drop appears later, reload highlightctx to arm the stricter ignore-aware ordering guarantee.",
                   [this](const CString& sLine) { CmdSetRequireIgnoreDrop(sLine); });
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
        return true;
    }

    void OnClientAttached() override {
        ReplayAndClear(/*manual=*/false);
        ClearVolatileState();
    }

    void OnClientDetached() override {
        ClearVolatileState();
    }

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

    unsigned int m_before_max{8};
    unsigned int m_after_max{8};
    unsigned int m_max_events{0};  // 0 = disabled (no cap on pending events)
    ERequireIgnoreMode m_require_ignore_mode{ERequireIgnoreMode::Auto};
    bool m_ignore_drop_present_on_module_load{false};
    bool m_auto_ignore_drop_armed{false};

    std::set<std::string> m_excluded;
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
                VCString chans;
                val.Split(",", chans, false);
                for (const auto& c : chans) {
                    CString chan = c.Trim_n();
                    if (!chan.empty()) m_excluded.insert(lc(chan));
                }
                SaveExcludes();
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

    bool HasIgnoreDropLoaded() const {
        if (!GetNetwork()) return false;
        return (GetNetwork()->GetModules().FindModule("ignore_drop") != nullptr);
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

        FeedOpenEvents(channel, line);

        CString mynick = CurrentNick();
        bool is_self = (lc(line.nick) == lc(mynick));
        if (!is_self && contains_nick_highlight(line.text, mynick)) {
            StartEvent(channel, line);
        }

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
        PutModule("ignore_drop modes: off = never required. on = must already be loaded before highlightctx loads and capture pauses whenever it is absent. auto = the same strict behavior is armed only if ignore_drop was already present when highlightctx itself loaded; if ignore_drop appears later, reload highlightctx to arm the stricter ignore-aware ordering guarantee.");
        PutModule("Primary commands: Status, SetBefore, SetAfter, SetMaxEvents, AddExclude, DelExclude, ListExcludes, SetRequireIgnoreDrop, Reset, ReplayNow, Compact, ClearPending.");
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
        PutModule(CString("ignore_drop was present when highlightctx loaded: ") + (m_ignore_drop_present_on_module_load ? "yes" : "no"));
        PutModule(CString("auto mode armed: ") + ((m_require_ignore_mode == ERequireIgnoreMode::Auto && m_auto_ignore_drop_armed) ? "yes" : "no"));
        PutModule(CString("effective ignore_drop requirement now: ") + (IsIgnoreDropRequirementEffective() ? "yes" : "no"));
        PutModule(CString("ignore_drop currently loaded: ") + (HasIgnoreDropLoaded() ? "yes" : "no"));
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto && !m_auto_ignore_drop_armed && HasIgnoreDropLoaded()) {
            PutModule("note: ignore_drop is loaded now, but auto mode is not armed because ignore_drop was not present when highlightctx loaded. Reload highlightctx after ignore_drop if you want strict ignore-aware ordering semantics.");
        }
        if (GetClient()) {
            PutModule(CString("current client native server-time replay support: ") + (ClientSupportsNativeServerTime(GetClient()) ? "yes" : "no"));
        }
        PutModule(CString("excluded channels: ") + CString(std::to_string(m_excluded.size())));
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
        CString chan = sLine.Token(1, true).Trim_n();
        if (chan.empty()) {
            PutModule("Usage: AddExclude <#channel>");
            return;
        }
        m_excluded.insert(lc(chan));
        SaveExcludes();
        CString msg = "Excluded channel: ";
        msg += chan;
        PutModule(msg);
    }

    void CmdDelExclude(const CString& sLine) {
        CString chan = sLine.Token(1, true).Trim_n();
        if (chan.empty()) {
            PutModule("Usage: DelExclude <#channel>");
            return;
        }
        auto it = m_excluded.find(lc(chan));
        if (it == m_excluded.end()) {
            PutModule("That channel is not currently excluded.");
            return;
        }
        m_excluded.erase(it);
        SaveExcludes();
        CString msg = "Removed exclusion for: ";
        msg += chan;
        PutModule(msg);
    }

    void CmdListExcludes() {
        if (m_excluded.empty()) {
            PutModule("No excluded channels configured.");
            return;
        }
        size_t i = 0;
        for (const auto& chan : m_excluded) {
            ++i;
            PutModule(CString(std::to_string(i)) + ") " + chan.c_str());
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
                msg += " | note: ignore_drop is loaded now but auto mode is unarmed because ignore_drop was not present when highlightctx loaded; reload highlightctx after ignore_drop if you want strict ignore-aware ordering semantics.";
            }
        }
        PutModule(msg);
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
        RecomputeIgnoreDropRuntimeState();
        TrimAllRings();
        SetNV("before_max", "8");
        SetNV("after_max", "8");
        SetNV("max_events", "0");
        SaveIgnoreMode();
        SaveExcludes();
        PutModule("All settings reset to defaults: before=8, after=8, max_events=disabled, require_ignore_drop=auto, excludes cleared.");
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
        "Detached-only highlight context capture with independent live history, durable active-event journaling, native timestamp replay into *highlightctx when supported, and ignore_drop integration modes off/on/auto."
    );
    Info.AddType(CModInfo::NetworkModule);
}

NETWORKMODULEDEFS(CHighlightCtx, "Detached-only highlight context capture with durable active-event journaling")