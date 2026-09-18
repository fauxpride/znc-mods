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
// Overlapping highlights (extension semantics, since 0.9.0):
// - A qualifying trigger that arrives while an event on the same channel is
//   still collecting its trailing lines does NOT start a second event.
//   Instead it extends the open event: the line is recorded in that event's
//   `after` list, marked as an additional trigger (shown with >>> on replay),
//   and the event's trailing-line target is pushed out so that a full
//   `after` window (using the cap the event started with) follows the new
//   trigger. Repeated triggers keep extending the same event.
// - This removes the duplicated context that overlapping events produced in
//   0.8.0 and earlier, where every line inside both windows was replayed twice.
// - Only a line that could have started an event can extend one: self lines
//   and lines from nick/mask-excluded senders are context only.
// - A trigger that arrives after the open event has already finalized starts
//   a new event as before (with a normal `before` snapshot).
// - Extensions are journaled as `X` records so they survive a restart.
//
// Journaling opt-out (since 0.10.0):
// - Load argument `journal=off` turns off the durable journal entirely. The
//   journal file is then neither read at load nor written during operation,
//   so captured highlight context (channel names, nicks, message text) never
//   reaches disk.
// - The trade-off is durability: with journaling off, open and pending events
//   live only in memory and are lost on unload, restart, or crash.
// - The setting persists in NV storage like the other settings, so a reload
//   without arguments keeps it. `Reset` deliberately does NOT restore it to
//   on, so resetting settings can never silently resume writing to disk.
// - Turning it off leaves any existing journal file untouched and ignored;
//   `Compact` removes that leftover file on an explicit request.
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
#include <type_traits>
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

static const char* kModVersion = "highlightctx 0.12.0";
static const char* kJournalName = "highlightctx.journal";
// Floor for the compaction trigger. The effective trigger point is derived
// from what the last compaction actually achieved (see m_compact_at), because
// comparing against a fixed constant rewrites the journal on every appended
// line once live state alone exceeds it.
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

// Accepts the same on/off vocabulary as the other boolean-ish settings.
// Decimal conversion for the module's own strings.
//
// This deliberately avoids std::to_string. In some libstdc++ versions (seen
// with GCC 11 on Ubuntu 22.04) std::to_string is implemented through
// std::__detail::__to_chars_10_impl, whose function-local __digits tables are
// emitted as STB_GNU_UNIQUE symbols. glibc refuses to unload any library whose
// own unique symbol gets bound, so dlclose() silently does nothing and
// `/znc updatemod` reloads the already-resident old code while reporting
// success — the module only really updates after a full ZNC restart.
//
// Keeping the conversion in this module's own code (internal linkage, no
// function-local statics, no libstdc++ template internals) produces no unique
// symbols on any compiler, so the module stays unloadable. Output is identical
// to std::to_string for every integer type used here.
template <typename T>
static std::string dec_str(T v) {
    char buf[32];
    if (std::is_signed<T>::value) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    } else {
        std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(v));
    }
    return std::string(buf);
}

static bool parse_on_off(const CString& s, bool& out) {
    CString v = s.AsLower().Trim_n();
    if (v == "0" || v == "off" || v == "no" || v == "false" || v == "disable" || v == "disabled") {
        out = false;
        return true;
    }
    if (v == "1" || v == "on" || v == "yes" || v == "true" || v == "enable" || v == "enabled") {
        out = true;
        return true;
    }
    return false;
}

// Duration for max_event_age: <number>[s|m|h|d|w], bare number = seconds,
// and the usual off spellings mean "no expiry". Rejects overflow.
static bool parse_duration_secs(const CString& s, long long& out) {
    CString v = s.AsLower().Trim_n();
    if (v.empty()) return false;
    if (v == "0" || v == "off" || v == "no" || v == "false" || v == "disable" || v == "disabled") {
        out = 0;
        return true;
    }
    char unit = v[v.length() - 1];
    long long mult = 1;
    CString num = v;
    if (unit < '0' || unit > '9') {
        switch (unit) {
            case 's': mult = 1; break;
            case 'm': mult = 60; break;
            case 'h': mult = 3600; break;
            case 'd': mult = 86400; break;
            case 'w': mult = 604800; break;
            default: return false;
        }
        num = v.substr(0, v.length() - 1);
    }
    if (num.empty()) return false;
    errno = 0;
    char* endp = nullptr;
    unsigned long long n = std::strtoull(num.c_str(), &endp, 10);
    if (errno == ERANGE || !endp || *endp != '\0') return false;
    const unsigned long long kMaxSecs = 100ULL * 365ULL * 86400ULL;  // 100 years
    if (n > kMaxSecs / static_cast<unsigned long long>(mult)) return false;
    out = static_cast<long long>(n) * mult;
    return true;
}

static CString format_duration_secs(long long secs) {
    if (secs <= 0) return "disabled";
    struct { const char* suffix; long long unit; } units[] = {
        {"w", 604800}, {"d", 86400}, {"h", 3600}, {"m", 60}, {"s", 1}};
    for (const auto& u : units) {
        if (secs % u.unit == 0) {
            return CString(dec_str(secs / u.unit)) + u.suffix;
        }
    }
    return CString(dec_str(secs)) + "s";
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
    // Fold both sides with RFC 1459 casemapping so a nick containing
    // [ ] \ ~ matches the { } | ^ forms the server may present, consistently
    // with the self-check and with nick/mask exclusions. Boundary detection is
    // unaffected: both sets are nick characters.
    std::string hay = rfc1459_fold(text);
    std::string needle = rfc1459_fold(nick);
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

// Size of an existing regular file, or -1 if it does not exist / is not one.
static long long file_size_or_missing(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    return static_cast<long long>(st.st_size);
}

static bool remove_file_and_fsync_dir(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return false;
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
                   "Set the maximum number of messages to collect after a highlight trigger. This is a cap, not a guarantee; if you attach before enough later traffic arrives, the event is replayed as partial. A further highlight inside an open event's after window extends that event by a fresh window of the cap the event started with, instead of starting an overlapping event, subject to max_event_lines.",
                   [this](const CString& sLine) { CmdSetAfter(sLine); });
        AddCommand("SetMaxEvents", "<count|0|off>",
                   "Set the maximum number of finalized pending events to keep at once. When the cap is reached the oldest event is silently dropped to make room. Use 0 or off to disable the cap entirely (the default).",
                   [this](const CString& sLine) { CmdSetMaxEvents(sLine); });
        AddCommand("SetMaxEventLines", "<count|0|off>",
                   "Set the maximum total captured lines per event (before + trigger + after). This limits extension only: an event always collects the before/after window it started with. 0/off disables the limit.",
                   [this](const CString& sLine) { CmdSetMaxEventLines(sLine); });
        AddCommand("SetMaxEventAge", "<duration|off>",
                   "Drop pending events older than this duration without replaying them. Accepts 30d, 12h, 90m, 3600s, 2w, or off. This is a retention control, not a size control.",
                   [this](const CString& sLine) { CmdSetMaxEventAge(sLine); });
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
                   "Rewrite the durable on-disk journal to the minimum representation needed for currently open/pending events. This is usually automatic after replay/clear and when the journal grows large. With journal=off, this instead removes a journal file left over from an earlier session.",
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
        CString journal_note;
        if (m_journal_enabled) {
            if (!LoadJournal(sMessage)) return false;
            ReapplyLineCapOnLoad();
            ExpireOldPending();
        } else {
            // Do not read, write, or delete an existing journal here: leaving
            // it untouched keeps the opt-out non-destructive, and Compact
            // removes it on an explicit request.
            m_journal_line_count = 0;
            m_compact_at = kCompactThresholdLines;
            const long long leftover = file_size_or_missing(m_journal_path);
            if (leftover > 0) {
                journal_note = " | note: journal=off, but a journal file from an earlier session still exists on disk and is being ignored; run Compact to remove it";
            }
        }

        SetNV("version_marker", kModVersion);
        sMessage = "Loaded ";
        sMessage += kModVersion;
        sMessage += " | before=";
        sMessage += CString(dec_str(m_before_max));
        sMessage += " after=";
        sMessage += CString(dec_str(m_after_max));
        sMessage += " require_ignore_drop=";
        sMessage += IgnoreModeName();
        if (m_require_ignore_mode == ERequireIgnoreMode::Auto) {
            sMessage += " (armed=";
            sMessage += (m_auto_ignore_drop_armed ? "yes" : "no");
            sMessage += ")";
        }
        sMessage += " journal=";
        sMessage += (m_journal_enabled ? "on" : "off");
        sMessage += " max_events=";
        sMessage += (m_max_events == 0 ? "disabled" : CString(dec_str(m_max_events)).c_str());
        sMessage += " max_event_lines=";
        sMessage += (m_max_event_lines == 0 ? "disabled" : CString(dec_str(m_max_event_lines)).c_str());
        sMessage += " max_event_age=";
        sMessage += format_duration_secs(m_max_event_age_secs);
        sMessage += " excludes=";
        sMessage += CString(dec_str(m_excluded.size()));
        sMessage += "ch/";
        sMessage += CString(dec_str(m_excluded_nicks.size()));
        sMessage += "nick";
        sMessage += journal_note;
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
        // Set only on a copy stored in an event's `after` list, when that
        // line was a qualifying highlight that extended the event. Never set
        // on ring-buffer lines or on the primary trigger. Not part of the
        // serialized line format; journaled separately as an `X` record.
        bool extends_event{false};
    };

    struct Event {
        unsigned long long id{0};
        CString channel;
        std::string channel_lc;   // RFC 1459 case-folded channel key
        long long started_ts{0};
        unsigned int after_cap{0};
        // Number of trailing lines this event must collect before it is
        // complete. Starts at after_cap and grows each time a later trigger
        // extends the event: target = (index of that trigger in `after`) + 1
        // + after_cap. Derived state; rebuilt from B/A/X records on load.
        size_t after_target{0};
        std::vector<CaptureLine> before;
        CaptureLine trigger;
        std::vector<CaptureLine> after;
        bool finalized{false};
        bool partial{false};
        // Set when max_event_lines prevented at least one extension of this
        // event. Journaled as a C record so it survives a restart.
        bool capped{false};
    };

    struct NickExclude {
        std::string mask_folded;  // RFC 1459-folded mask, preserved wildcards
        bool nick_only{true};     // true = match nick only; false = match full nick!ident@host
    };

    unsigned int m_before_max{8};
    unsigned int m_after_max{8};
    // Default since 0.11.0: keep the 100 most recent pending events rather
    // than an unbounded queue. See README for how this value was chosen.
    unsigned int m_max_events{100};
    // 0 = disabled. Maximum total captured lines per event (before + trigger
    // + after). Limits extension only; an event always gets its configured
    // window. Evaluated against the current value, not captured per event.
    unsigned int m_max_event_lines{100};
    // 0 = disabled. Pending events older than this are dropped unreplayed.
    long long m_max_event_age_secs{0};
    // Events dropped since the last replay, reported to the user at replay
    // time and in Status so that shedding is never silent.
    unsigned long long m_dropped_by_cap{0};
    unsigned long long m_dropped_by_age{0};
    ERequireIgnoreMode m_require_ignore_mode{ERequireIgnoreMode::Auto};
    bool m_ignore_drop_present_on_module_load{false};
    bool m_auto_ignore_drop_armed{false};

    std::set<std::string> m_excluded;                 // channel exclusions, RFC 1459 case-folded
    std::vector<NickExclude> m_excluded_nicks;        // nick/hostmask exclusions
    std::map<std::string, std::deque<CaptureLine>> m_ring_by_chan;
    std::map<std::string, std::vector<Event>> m_open_by_chan;
    std::deque<Event> m_pending;

    unsigned long long m_next_id{1};
    std::string m_journal_path;
    size_t m_journal_line_count{0};
    // Line count at which the next compaction is attempted. Recomputed after
    // every successful compaction as max(kCompactThresholdLines, 2 x lines
    // actually written), so the journal must at least double before it is
    // rewritten again. That bounds total rewrite work to O(appends) instead
    // of O(live_lines) per appended line, at the cost of letting the file sit
    // at up to twice the size of live state between compactions.
    size_t m_compact_at{kCompactThresholdLines};
    // When false, the module keeps all state in memory only: the journal is
    // neither read at load nor written during operation, so highlight context
    // never touches disk and does not survive an unload or restart.
    bool m_journal_enabled{true};

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
        if (HasNV("max_event_lines")) {
            m_max_event_lines = GetNV("max_event_lines").ToUInt();
        }
        if (HasNV("max_event_age_secs")) {
            long long parsed = 0;
            if (parse_duration_secs(GetNV("max_event_age_secs"), parsed)) {
                m_max_event_age_secs = parsed;
            }
        }
        if (HasNV("dropped_by_cap")) m_dropped_by_cap = GetNV("dropped_by_cap").ToULongLong();
        if (HasNV("dropped_by_age")) m_dropped_by_age = GetNV("dropped_by_age").ToULongLong();
        if (HasNV("journal_enabled")) {
            bool parsed = true;
            if (parse_on_off(GetNV("journal_enabled"), parsed)) {
                m_journal_enabled = parsed;
            }
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
            if (!chan.empty()) m_excluded.insert(rfc1459_fold(chan));
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
                sError = "Invalid load arg syntax. Use key=value pairs like before=8 after=8 max_events=100 max_event_lines=100 max_event_age=off journal=on require_ignore_drop=auto excludes=#chan1,#chan2";
                return false;
            }

            if (key == "before") {
                unsigned int n = 0;
                if (!parse_uint_cstr(val, n)) {
                    sError = "Invalid value for before.";
                    return false;
                }
                m_before_max = n;
                SetNV("before_max", CString(dec_str(m_before_max)));
            } else if (key == "after") {
                unsigned int n = 0;
                if (!parse_uint_cstr(val, n)) {
                    sError = "Invalid value for after.";
                    return false;
                }
                m_after_max = n;
                SetNV("after_max", CString(dec_str(m_after_max)));
            } else if (key == "max_event_lines") {
                CString vl = val.AsLower();
                if (vl == "off" || vl == "no" || vl == "false" || vl == "disable" || vl == "disabled") {
                    m_max_event_lines = 0;
                } else {
                    unsigned int n = 0;
                    if (!parse_uint_cstr(val, n)) {
                        sError = "Invalid value for max_event_lines; use a number or off/0 to disable.";
                        return false;
                    }
                    m_max_event_lines = n;
                }
                SetNV("max_event_lines", CString(dec_str(m_max_event_lines)));
            } else if (key == "max_event_age") {
                long long secs = 0;
                if (!parse_duration_secs(val, secs)) {
                    sError = "Invalid value for max_event_age; use a duration like 30d, 12h, 90m, or off.";
                    return false;
                }
                m_max_event_age_secs = secs;
                SetNV("max_event_age_secs", CString(dec_str(m_max_event_age_secs)));
            } else if (key == "journal") {
                bool enabled = true;
                if (!parse_on_off(val, enabled)) {
                    sError = "Invalid value for journal; use on or off.";
                    return false;
                }
                m_journal_enabled = enabled;
                SetNV("journal_enabled", m_journal_enabled ? "1" : "0");
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
                SetNV("max_events", CString(dec_str(m_max_events)));
            } else if (key == "excludes") {
                m_excluded.clear();
                m_excluded_nicks.clear();
                VCString items;
                val.Split(",", items, false);
                for (const auto& c : items) {
                    CString item = c.Trim_n();
                    if (item.empty()) continue;
                    if (is_channel_name(item)) {
                        m_excluded.insert(rfc1459_fold(item));
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
        return (m_excluded.find(rfc1459_fold(channel)) != m_excluded.end());
    }

    void EnsureJournalPath() {
        m_journal_path = std::string(GetSavePath().c_str());
        if (!m_journal_path.empty() && m_journal_path.back() != '/') m_journal_path.push_back('/');
        m_journal_path += kJournalName;
    }

    static std::string SerializeLine(const CaptureLine& line) {
        const std::string nick_hex = hex_encode(std::string(line.nick.c_str()));
        const std::string text_hex = hex_encode(std::string(line.text.c_str()));
        std::string raw = dec_str(line.ts_sec) + ";" + std::string(1, line.kind) + ";" + nick_hex + ";" + text_hex;
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

        return std::string("B\t") + dec_str(ev.id) + "\t" +
               hex_encode(std::string(ev.channel.c_str())) + "\t" +
               dec_str(ev.started_ts) + "\t" +
               dec_str(ev.after_cap) + "\t" +
               join_char(before_hex, ',') + "\t" +
               SerializeLine(ev.trigger);
    }

    static std::string SerializeAfter(unsigned long long id, const CaptureLine& line) {
        return std::string("A\t") + dec_str(id) + "\t" + SerializeLine(line);
    }

    // X record: the after-line at zero-based `after_index` of event `id` is a
    // trigger that extended the event. The new target is not stored; it is
    // derived as after_index + 1 + after_cap from the event's B record.
    static std::string SerializeExtend(unsigned long long id, size_t after_index) {
        return std::string("X\t") + dec_str(id) + "\t" + dec_str(after_index);
    }

    // Apply an extension at `after_index` to `ev`. Shared by the live feed
    // path and journal recovery so both compute the target identically.
    // Mark the after-line at after_index as an additional trigger and grow the
    // event's target, unless max_event_lines would be exceeded. Returns true
    // if the target grew. Used by the live feed path AND by journal recovery,
    // so a recovered event has exactly the target it had before the restart.
    bool TryExtend(Event& ev, size_t after_index) {
        ev.after[after_index].extends_event = true;
        const size_t proposed_target = after_index + 1 + static_cast<size_t>(ev.after_cap);
        if (proposed_target <= ev.after_target) return false;
        const size_t proposed_total = ev.before.size() + 1 + proposed_target;
        if (m_max_event_lines > 0 && proposed_total > static_cast<size_t>(m_max_event_lines)) {
            ev.capped = true;
            return false;
        }
        ev.after_target = proposed_target;
        return true;
    }

    static size_t CountExtensions(const Event& ev) {
        size_t n = 0;
        for (const auto& line : ev.after) {
            if (line.extends_event) ++n;
        }
        return n;
    }

    // C record: max_event_lines suppressed at least one extension of this
    // event. Carries no payload beyond the id; older builds ignore unknown
    // record types, so this is safe to downgrade past.
    static std::string SerializeCapped(unsigned long long id) {
        return std::string("C\t") + dec_str(id);
    }

    static std::string SerializeFinalize(unsigned long long id, bool partial) {
        return std::string("F\t") + dec_str(id) + "\t" + (partial ? "1" : "0");
    }

    static std::string SerializeDelivered(unsigned long long id) {
        return std::string("D\t") + dec_str(id);
    }

    bool AppendJournal(const std::string& line) {
        if (!m_journal_enabled) return true;
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
                ev.channel_lc = rfc1459_fold(ev.channel);
                ev.started_ts = started;
                ev.after_cap = static_cast<unsigned int>(after_cap_ul);
                ev.after_target = ev.after_cap;

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
                all.insert(std::make_pair(id, Event())).first->second = ev;
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
            } else if (op == "X") {
                if (parts.size() != 3) continue;
                char* endp1 = nullptr;
                char* endp2 = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp1, 10);
                unsigned long long idx = std::strtoull(parts[2].c_str(), &endp2, 10);
                if (!endp1 || *endp1 != '\0' || !endp2 || *endp2 != '\0') continue;
                if (parts[1].empty() || parts[2].empty()) continue;
                auto it = all.find(id);
                if (it == all.end()) continue;
                // The X record is always written after the A record it refers
                // to, so a valid index is already present. Ignore anything
                // else (e.g. a torn write that lost the A record).
                if (idx >= it->second.after.size()) continue;
                TryExtend(it->second, static_cast<size_t>(idx));
                if (id > max_id) max_id = id;
            } else if (op == "C") {
                if (parts.size() != 2) continue;
                char* endp = nullptr;
                unsigned long long id = std::strtoull(parts[1].c_str(), &endp, 10);
                if (!endp || *endp != '\0' || parts[1].empty()) continue;
                auto it = all.find(id);
                if (it == all.end()) continue;
                it->second.capped = true;
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
                m_open_by_chan.insert(std::make_pair(ev.channel_lc, std::vector<Event>())).first->second.push_back(ev);
            }
        }

        m_next_id = max_id + 1;
        if (m_journal_line_count > m_compact_at) {
            if (!CompactJournalInternal()) {
                sError = "highlightctx loaded, but journal compaction failed; continuing with existing state.";
            }
        }
        return true;
    }

    bool CompactJournalInternal() {
        // With journaling off there is nothing on disk representing current
        // state, and an untouched leftover file is only removed by Compact.
        if (!m_journal_enabled) return true;
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
                for (size_t i = 0; i < ev.after.size(); ++i) {
                    append_line(SerializeAfter(ev.id, ev.after[i]));
                    ++lines_written;
                    if (ev.after[i].extends_event) {
                        append_line(SerializeExtend(ev.id, i));
                        ++lines_written;
                    }
                }
                if (ev.capped) {
                    append_line(SerializeCapped(ev.id));
                    ++lines_written;
                }
            }
        }
        for (const auto& ev : m_pending) {
            append_line(SerializeBegin(ev));
            ++lines_written;
            for (size_t i = 0; i < ev.after.size(); ++i) {
                append_line(SerializeAfter(ev.id, ev.after[i]));
                ++lines_written;
                if (ev.after[i].extends_event) {
                    append_line(SerializeExtend(ev.id, i));
                    ++lines_written;
                }
            }
            if (ev.capped) {
                append_line(SerializeCapped(ev.id));
                ++lines_written;
            }
            append_line(SerializeFinalize(ev.id, ev.partial));
            ++lines_written;
        }

        bool ok = durable_replace_file(m_journal_path, out);
        if (ok) {
            m_journal_line_count = lines_written;
            m_compact_at = std::max(kCompactThresholdLines, lines_written * 2);
        }
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

        ExpireOldPending();

        // If max_events is enabled, drop the oldest pending event(s) to stay within the cap.
        if (m_max_events > 0) {
            while (m_pending.size() > m_max_events) {
                AppendJournal(SerializeDelivered(m_pending.front().id));
                m_pending.pop_front();
                ++m_dropped_by_cap;
                SetNV("dropped_by_cap", CString(dec_str(m_dropped_by_cap)));
            }
        }
    }

    // Drop pending events older than max_event_age. Pending events are stored
    // in finalize order, so this only has to inspect the front of the vector
    // and is O(1) when nothing has expired.
    void ExpireOldPending() {
        if (m_max_event_age_secs <= 0 || m_pending.empty()) return;
        const long long cutoff = static_cast<long long>(::time(nullptr)) - m_max_event_age_secs;
        size_t dropped = 0;
        while (!m_pending.empty() && m_pending.front().started_ts < cutoff) {
            AppendJournal(SerializeDelivered(m_pending.front().id));
            m_pending.pop_front();
            ++dropped;
        }
        if (dropped > 0) {
            m_dropped_by_age += dropped;
            SetNV("dropped_by_age", CString(dec_str(m_dropped_by_age)));
            CompactJournalInternal();
        }
    }

    // TryExtend already reproduces the live target during recovery. This only
    // handles a cap that was LOWERED between sessions, so an event recovered
    // with an older, larger target stops growing under the new limit.
    void ReapplyLineCapOnLoad() {
        if (m_max_event_lines == 0) return;
        for (auto& kv : m_open_by_chan) {
            for (auto& ev : kv.second) {
                const size_t allowed = static_cast<size_t>(m_max_event_lines);
                const size_t base = ev.before.size() + 1;
                if (base + ev.after_target > allowed) {
                    const size_t room = (allowed > base) ? (allowed - base) : 0;
                    const size_t floor_target = static_cast<size_t>(ev.after_cap);
                    const size_t new_target = (room > floor_target) ? room : floor_target;
                    if (new_target < ev.after_target) {
                        ev.after_target = new_target;
                        ev.capped = true;
                    }
                }
            }
        }
    }

    // Tell the user about events shed since the last replay, so that dropping
    // is never silent. Counters are cleared once reported.
    void ReportDroppedEvents() {
        if (m_dropped_by_cap == 0 && m_dropped_by_age == 0) return;
        CString note = "note: ";
        note += CString(dec_str(m_dropped_by_cap + m_dropped_by_age));
        note += " older highlight event(s) were dropped before this replay (";
        bool first = true;
        if (m_dropped_by_cap > 0) {
            note += CString(dec_str(m_dropped_by_cap));
            note += " to stay within max_events=";
            note += CString(dec_str(m_max_events));
            first = false;
        }
        if (m_dropped_by_age > 0) {
            if (!first) note += ", ";
            note += CString(dec_str(m_dropped_by_age));
            note += " expired after max_event_age=";
            note += format_duration_secs(m_max_event_age_secs);
        }
        note += ").";
        ReplayLineToClient(note, static_cast<long long>(::time(nullptr)));
        m_dropped_by_cap = 0;
        m_dropped_by_age = 0;
        SetNV("dropped_by_cap", "0");
        SetNV("dropped_by_age", "0");
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
        if (m_journal_line_count > m_compact_at) {
            CompactJournalInternal();
        }
    }

    void StartEvent(const CString& channel, const CaptureLine& trigger_line) {
        Event ev;
        ev.id = AllocEventId();
        ev.channel = channel;
        ev.channel_lc = rfc1459_fold(channel);
        ev.started_ts = trigger_line.ts_sec;
        ev.after_cap = m_after_max;
        ev.after_target = ev.after_cap;
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
            m_open_by_chan.insert(std::make_pair(ev.channel_lc, std::vector<Event>())).first->second.push_back(ev);
        }
    }

    // Feed a line into every open event on this channel.
    //
    // If `is_trigger` is true (the line is a qualifying highlight that would
    // otherwise start a new event), the most recently started open event on
    // the channel is extended instead: the line is marked as an additional
    // trigger and that event's trailing-line target is pushed out to a full
    // after_cap window past this line. The extension is applied BEFORE the
    // completion check, so a trigger that lands on the last line of the
    // window still extends the event rather than letting it finalize and
    // starting an overlapping one.
    //
    // Only the newest open event is extended. With 0.9.0+ there is normally
    // at most one open event per channel; more can exist only after loading
    // a journal written by 0.8.0 or earlier, and extending just the newest
    // lets the older overlapping ones run out and converge to one.
    //
    // Returns true if the line extended an open event (the caller must then
    // NOT start a new event for it).
    bool FeedOpenEvents(const CString& channel, const CaptureLine& line, bool is_trigger) {
        const std::string chan_l = rfc1459_fold(channel);
        auto it = m_open_by_chan.find(chan_l);
        if (it == m_open_by_chan.end()) return false;
        if (it->second.empty()) {
            m_open_by_chan.erase(it);
            return false;
        }

        const size_t extend_idx = it->second.size() - 1;
        bool extended = false;

        // Finalize completed events in place. Earlier versions rebuilt the
        // vector by copying every surviving Event, which deep-copied all of
        // its before/after lines (two std::strings each) on every incoming
        // message — up to max_event_lines worth of copies per line, per open
        // event. Erasing in place touches only the finalized entries.
        size_t kept = 0;
        for (size_t i = 0; i < it->second.size(); ++i) {
            Event& ev = it->second[i];
            ev.after.push_back(line);
            AppendJournal(SerializeAfter(ev.id, line));
            if (is_trigger && i == extend_idx) {
                // max_event_lines limits extension only: the event always
                // keeps the window it started with, so no cap value can cut
                // into the configured before/after context. A suppressed
                // trigger is still marked, so replay stays honest about it.
                const size_t idx = ev.after.size() - 1;
                const bool was_capped = ev.capped;
                TryExtend(ev, idx);
                AppendJournal(SerializeExtend(ev.id, idx));
                if (ev.capped && !was_capped) AppendJournal(SerializeCapped(ev.id));
                extended = true;
            }
            if (ev.after.size() >= ev.after_target) {
                FinalizeEvent(ev, false);
            } else {
                if (kept != i) it->second[kept] = std::move(ev);
                ++kept;
            }
        }

        if (kept == 0) {
            m_open_by_chan.erase(it);
        } else {
            it->second.resize(kept);
        }
        return extended;
    }

    template <typename TMsg>
    void HandleIncoming(TMsg& Message, char kind) {
        if (!ShouldCaptureNow()) return;
        // Cheap front-of-queue check; O(1) when nothing has expired. Without a
        // timer this is what keeps expiry timely on an active network.
        ExpireOldPending();
        CChan* pChan = Message.GetChan();
        if (!pChan) return;

        const CString channel = pChan->GetName();
        if (channel.empty() || IsExcluded(channel)) return;

        CaptureLine line = MakeCaptureLine(Message, kind);

        // Classify the line before feeding open events, because a qualifying
        // trigger extends an open event instead of starting a second one.
        // Nick-exclusion gate: an excluded sender cannot start (or extend)
        // an event, but their messages are still fed into open events and
        // the ring buffer as ordinary context.
        CString mynick = CurrentNick();
// RFC 1459 casemapping: [ ] \ fold to { } | on the networks this module
        // targets, so self-detection must fold rather than ASCII-lowercase.
        bool is_self = (rfc1459_fold(line.nick) == rfc1459_fold(mynick));
        bool is_trigger = !is_self && contains_nick_highlight(line.text, mynick) &&
                          !IsSenderExcluded(Message, line.nick);

        // Feed already-open events: excluded-nick messages are STILL valid
        // context for other events that were triggered by someone else. This
        // is the intentional difference from channel exclusion, which drops
        // the channel entirely.
        const bool extended = FeedOpenEvents(channel, line, is_trigger);

        if (is_trigger && !extended) {
            StartEvent(channel, line);
        }

        // The ring buffer always receives every eligible channel line,
        // including from excluded senders, so their messages remain
        // available as 'before' context for any future trigger.
        auto& dq = m_ring_by_chan.insert(std::make_pair(rfc1459_fold(channel), std::deque<CaptureLine>())).first->second;
        dq.push_back(line);
        while (dq.size() > m_before_max) dq.pop_front();

        MaybeCompactAfterMutation();
    }

    void ReplayAndClear(bool manual) {
        FinalizeAllOpenAsPartial();
        ExpireOldPending();
        ReportDroppedEvents();
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
            header += CString(dec_str(ev.id));
            header += " (";
            header += (ev.partial ? "partial" : "complete");
            header += ", before=";
            header += CString(dec_str(ev.before.size()));
            header += ", after=";
            header += CString(dec_str(ev.after.size()));
            header += "/";
            header += CString(dec_str(ev.after_target));
            // Only extended events get the extra field, so the header of an
            // event with a single trigger is identical to 0.8.0.
            const size_t extensions = CountExtensions(ev);
            if (extensions > 0) {
                header += ", triggers=";
                header += CString(dec_str(extensions + 1));
            }
            if (ev.capped) header += ", capped";
            header += ")";
            ReplayLineToClient(header, ev.started_ts);

            for (const auto& line : ev.before) {
                ReplayLineToClient(FormatLineForReplay(ev.channel, line, false, false), line.ts_sec);
            }
            ReplayLineToClient(FormatLineForReplay(ev.channel, ev.trigger, true, false), ev.trigger.ts_sec);
            for (const auto& line : ev.after) {
                ReplayLineToClient(FormatLineForReplay(ev.channel, line, line.extends_event, false), line.ts_sec);
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
        PutModule("Overlapping highlights: if another qualifying highlight arrives in the same channel while an event is still collecting its after lines, no second event is started. The existing event is extended instead: that line is marked with >>> as an additional trigger, and the event keeps collecting until a full after window (using the after cap the event started with) has followed the latest trigger. The replay header then shows after=<collected>/<target> and triggers=<n>. Self lines and lines from nick/mask-excluded senders never extend an event. A highlight arriving after the event has already completed starts a new event as usual.");
        PutModule("Replay target: *highlightctx. When the current client supports IRCv3 server-time/time tags, replay is emitted as synthetic raw PRIVMSG lines with original @time values so the client can display historical timestamps natively. If the client does not support that, replay falls back to text prefixed with the original UTC timestamp.");
        PutModule("Events are sorted by channel name first, then by event time/id within each channel. A spacer line is added between events for readability.");
        PutModule("Persistence strategy: ordinary chatter stays only in RAM, while actual highlight events are durably journaled to disk as they happen. That keeps the hot path light but preserves active highlight captures across an unexpected VPS shutdown.");
        PutModule("Growth controls: max_events caps how many finalized events wait for replay (default 100, oldest dropped first). max_event_lines caps the total captured lines in one event (default 100) by limiting extension, never the configured window; a capped event shows 'capped' in its replay header and the next highlight after it finalizes starts a fresh event. max_event_age optionally drops pending events older than a given duration without replaying them, as a retention control (default off). Whenever events are dropped for either reason, the count is reported at the top of the next replay and in Status, so shedding is never silent.");
        PutModule("Journaling can be turned off with the journal=off load argument. Highlight context then never touches disk, at the cost of losing open and pending events on unload, restart, or crash. Status shows which mode is active, and with journaling off, Compact removes a journal file left over from an earlier session.");
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
        PutModule(CString("before cap: ") + CString(dec_str(m_before_max)));
        PutModule(CString("after cap: ") + CString(dec_str(m_after_max)));
        PutModule(CString("max_events: ") + (m_max_events == 0 ? "disabled" : CString(dec_str(m_max_events)).c_str()));
        PutModule(CString("max_event_lines: ") + (m_max_event_lines == 0 ? "disabled" : CString(dec_str(m_max_event_lines)).c_str()));
        PutModule(CString("max_event_age: ") + format_duration_secs(m_max_event_age_secs));
        PutModule(CString("events dropped since last replay: ") + CString(dec_str(m_dropped_by_cap)) +
                  " by max_events, " + CString(dec_str(m_dropped_by_age)) + " by max_event_age");
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
        PutModule(CString("excluded channels: ") + CString(dec_str(m_excluded.size())));
        PutModule(CString("excluded nicks/masks: ") + CString(dec_str(m_excluded_nicks.size())));
        PutModule(CString("open events: ") + CString(dec_str(open_count)));
        PutModule(CString("pending finalized events: ") + CString(dec_str(m_pending.size())));
        PutModule(CString("journal: ") + (m_journal_enabled ? "enabled (events survive restarts)" : "disabled (memory only; events are lost on unload, restart, or crash)"));
        PutModule(CString("journal path: ") + m_journal_path.c_str());
        if (!m_journal_enabled) {
            const long long leftover = file_size_or_missing(m_journal_path);
            if (leftover > 0) {
                PutModule(CString("note: a journal file from an earlier session still exists at that path (") +
                          CString(dec_str(leftover)) + " bytes) and is being ignored. Run Compact to remove it.");
            }
        }
    }

    void CmdSetBefore(const CString& sLine) {
        unsigned int n = 0;
        if (!parse_uint_cstr(sLine.Token(1, false), n)) {
            PutModule("Usage: SetBefore <count>");
            return;
        }
        m_before_max = n;
        SetNV("before_max", CString(dec_str(m_before_max)));
        TrimAllRings();
        PutModule(CString("before cap set to ") + CString(dec_str(m_before_max)));
    }

    void CmdSetAfter(const CString& sLine) {
        unsigned int n = 0;
        if (!parse_uint_cstr(sLine.Token(1, false), n)) {
            PutModule("Usage: SetAfter <count>");
            return;
        }
        m_after_max = n;
        SetNV("after_max", CString(dec_str(m_after_max)));
        PutModule(CString("after cap set to ") + CString(dec_str(m_after_max)) +
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
            const std::string key = rfc1459_fold(tok);
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
            auto it = m_excluded.find(rfc1459_fold(tok));
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
            PutModule(CString(dec_str(i)) + ") " + c + " [channel]");
        }
        // Nick/mask exclusions in insertion order.
        for (const auto& ne : m_excluded_nicks) {
            ++i;
            CString line = CString(dec_str(i)) + ") " + ne.mask_folded.c_str()
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
        SetNV("max_events", CString(dec_str(m_max_events)));

        if (m_max_events == 0) {
            PutModule("max_events set to disabled (no cap on pending events).");
        } else {
            PutModule(CString("max_events set to ") + CString(dec_str(m_max_events)) +
                      " (new events will start dropping the oldest once the cap is reached).");
            if (m_pending.size() > m_max_events) {
                size_t to_drop = m_pending.size() - m_max_events;
                for (size_t i = 0; i < to_drop; ++i) {
                    AppendJournal(SerializeDelivered(m_pending.front().id));
                    m_pending.pop_front();
                    ++m_dropped_by_cap;
                }
                SetNV("dropped_by_cap", CString(dec_str(m_dropped_by_cap)));
                PutModule(CString("Dropped ") + CString(dec_str(to_drop)) +
                          " oldest pending event(s) to enforce the new cap immediately.");
                CompactJournalInternal();
            }
        }
    }

    void CmdSetMaxEventLines(const CString& sLine) {
        CString val = sLine.Token(1, false);
        CString vl = val.AsLower();
        if (val.empty()) {
            PutModule("Usage: SetMaxEventLines <count|0|off>");
            return;
        }
        if (vl == "off" || vl == "no" || vl == "false" || vl == "disable" || vl == "disabled" || vl == "0") {
            m_max_event_lines = 0;
        } else {
            unsigned int n = 0;
            if (!parse_uint_cstr(val, n)) {
                PutModule("Usage: SetMaxEventLines <count|0|off>");
                return;
            }
            m_max_event_lines = n;
        }
        SetNV("max_event_lines", CString(dec_str(m_max_event_lines)));
        if (m_max_event_lines == 0) {
            PutModule("max_event_lines set to disabled (events can grow without limit through extension).");
            return;
        }
        PutModule(CString("max_event_lines set to ") + CString(dec_str(m_max_event_lines)) +
                  " total lines per event. It applies to all capture from now on, including events recovered from the journal; attaching already closed any event that was in progress.");
        const unsigned int natural = m_before_max + 1 + m_after_max;
        if (m_max_event_lines < natural) {
            PutModule(CString("note: that is below the natural size of a single event (before ") +
                      CString(dec_str(m_before_max)) + " + trigger + after " +
                      CString(dec_str(m_after_max)) + " = " + CString(dec_str(natural)) +
                      " lines), so extension is effectively disabled. Events still collect their full configured window.");
        }
    }

    void CmdSetMaxEventAge(const CString& sLine) {
        CString val = sLine.Token(1, false);
        long long secs = 0;
        if (val.empty() || !parse_duration_secs(val, secs)) {
            PutModule("Usage: SetMaxEventAge <duration|off> — e.g. 30d, 12h, 90m, 3600s, 2w, off");
            return;
        }
        m_max_event_age_secs = secs;
        SetNV("max_event_age_secs", CString(dec_str(m_max_event_age_secs)));
        if (m_max_event_age_secs == 0) {
            PutModule("max_event_age set to disabled (pending events are kept until replayed).");
            return;
        }
        // Defensive: while a client is attached the pending queue is empty
        // (attaching replays and clears it), so this normally drops nothing.
        // Expiry that matters happens at load and as traffic arrives.
        ExpireOldPending();
        PutModule(CString("max_event_age set to ") + format_duration_secs(m_max_event_age_secs) +
                  ". Pending events older than that are dropped without being replayed, and the count is reported at your next replay.");
    }

    void CmdReset() {
        m_before_max = 8;
        m_after_max = 8;
        m_max_events = 100;
        m_max_event_lines = 100;
        m_max_event_age_secs = 0;
        m_require_ignore_mode = ERequireIgnoreMode::Auto;
        m_excluded.clear();
        m_excluded_nicks.clear();
        RecomputeIgnoreDropRuntimeState();
        TrimAllRings();
        SetNV("before_max", "8");
        SetNV("after_max", "8");
        SetNV("max_events", "100");
        SetNV("max_event_lines", "100");
        SetNV("max_event_age_secs", "0");
        SaveIgnoreMode();
        SaveExcludes();
        SaveExcludedNicks();
        PutModule("All settings reset to defaults: before=8, after=8, max_events=100, max_event_lines=100, max_event_age=disabled, require_ignore_drop=auto, all exclusions (channel and nick/mask) cleared.");
        PutModule("Pending/open events were not affected. Use ClearPending to discard them.");
        PutModule(CString("The journal setting was left unchanged (currently ") + (m_journal_enabled ? "on" : "off") +
                  "), so Reset never re-enables writing highlight context to disk. Change it with the journal=on/off load argument.");
    }

    void CmdCompact() {
        if (!m_journal_enabled) {
            const long long leftover = file_size_or_missing(m_journal_path);
            if (leftover < 0) {
                PutModule("Journaling is disabled (journal=off) and no journal file exists; nothing to do.");
            } else if (remove_file_and_fsync_dir(m_journal_path)) {
                PutModule(CString("Journaling is disabled (journal=off). Removed the leftover journal file (") +
                          CString(dec_str(leftover)) + " bytes).");
            } else {
                PutModule("Journaling is disabled (journal=off), but removing the leftover journal file failed.");
            }
            return;
        }
        if (CompactJournalInternal()) PutModule("Journal compacted.");
        else PutModule("Journal compaction failed.");
    }

    void CmdClearPending() {
        m_open_by_chan.clear();
        m_pending.clear();
        ClearVolatileState();
        if (!m_journal_enabled) {
            PutModule("Cleared all open/pending highlight events. Journaling is disabled, so nothing was written to disk.");
        } else if (CompactJournalInternal()) {
            PutModule("Cleared all open/pending highlight events and compacted the journal.");
        } else {
            PutModule("Cleared in-memory events, but journal compaction failed.");
        }
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