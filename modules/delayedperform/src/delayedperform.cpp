/*
 * delayedperform.cpp — ZNC module to run post-connect commands with delays.
 *
 * Version: 1.2.0  (see DELAYED_PERFORM_VERSION below; also queryable via
 *                  the 'Version' command and shown in 'Help').
 *
 * Features:
 *  - Run multiple user-configured IRC commands after connect (like perform)
 *  - Global default delay (seconds) + optional per-command delay override
 *  - Add/AddSecret/list/del/clear/help/version via /msg *delayedperform
 *  - Per-network persistence via SetNV/GetNV
 *  - Per-entry 'secret' flag (AddSecret) hides a command's text in the
 *    module's List output, the "Added"/"Ran:" echoes, and disconnect logs.
 *
 * Convenience shorthands supported:
 *   /msg, /notice, /join, /part, /quit, /nick, /topic, /mode, /kick, /invite,
 *   /ctcp, /me (requires explicit target), /whois, /away, /oper, /raw, /quote,
 *   service aliases: /ns /cs /hs /ms /os /bs (NickServ etc.)
 * Unknown /verb → fallback to RAW uppercase verb (e.g. /cap ls → "CAP ls").
 * A supported shorthand missing a required argument is rejected.
 *
 * Variables expanded at send time:
 *   %nick%  — current IRC nick (after reconnect/fallback). Works inside strings.
 *            The nick is validated against IRC nick grammar before splicing;
 *            if it contains any character outside that grammar (space, ':',
 *            CR, LF, etc.), the command is skipped rather than sent with a
 *            malformed nick spliced in.
 *
 * Storage format (NV value for each cmd.<key> key):
 *   "<delay>|<flags>|<base64>"          -- v1.1+ entries (flags may be empty)
 *   "<delay>|<base64>"                  -- v1.0 legacy entries (also loaded)
 * Keys are ordered by their number and need not be contiguous; the indices
 * shown by List and accepted by Del are positions in that order. A value that
 * cannot be read is listed as "[corrupt entry]", is never scheduled, and can be
 * removed with Del.
 *
 * Recognized flag characters:
 *   's'  — secret: command text is never echoed in module output
 */

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>   // for CIRCNetwork::IsIRCConnected
#include <algorithm>
#include <climits>  // UINT_MAX
#include <vector>

using std::vector;

// --- Version string (queryable; see CmdVersion / TModInfo / MODULEDEFS) ---
#define DELAYED_PERFORM_VERSION "1.2.0"

class CDelayedPerformModule;

// ---------------- One-shot timer ----------------
class CCmdTimer final : public CTimer {
  public:
    CCmdTimer(CModule* pMod,
              unsigned int uInterval,
              const CString& sLabel,
              const CString& sCommand,
              bool bSecret)
      : CTimer(pMod, uInterval, 1, sLabel, "delayedperform one-shot"),
        m_sCommand(sCommand),
        m_bSecret(bSecret) {}

    void RunJob() override;

  private:
    CString m_sCommand;
    bool    m_bSecret;
};

// ---------------- Module ----------------
class CDelayedPerformModule final : public CModule {
  public:
    MODCONSTRUCTOR(CDelayedPerformModule) {
      // No AddHelpCommand(): it registers ZNC's generic "Help" first, which
      // made the AddCommand("Help", ...) below fail and CmdHelp unreachable.
      // CmdHelp delegates to HandleHelpCommand() when given a filter.
      AddCommand("Help",      static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdHelp),
                 "[filter]", "Show detailed help, or list commands matching a filter.");
      AddCommand("Version",   static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdVersion),
                 "", "Show the module version.");
      AddCommand("SetDelay",  static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdSetDelay),
                 "<seconds>", "Set global default delay (seconds) used when 'Add' omits a delay.");
      AddCommand("Add",       static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdAdd),
                 "[seconds] <irc-or-slash-command>",
                 "Add a command. Accepts raw IRC or common '/'-style shorthands.");
      AddCommand("AddSecret", static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdAddSecret),
                 "[seconds] <irc-or-slash-command>",
                 "Same as Add, but the command text is masked in module output. Not encryption; do not use for credentials.");
      AddCommand("List",      static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdList),
                 "", "List configured commands with their effective delays.");
      AddCommand("Del",       static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdDel),
                 "<index|all>", "Delete one command by index or 'all'.");
      AddCommand("Clear",     static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdClear),
                 "", "Alias for 'Del all'.");
    }

    bool OnLoad(const CString& /*sArgs*/, CString& /*sMessage*/) override {
      LoadGlobalDelay();
      return true;
    }

    void OnIRCConnected() override {
      CleanupTimers(); // just in case
      vector<Entry> entries;
      LoadAll(entries);
      if (entries.empty()) return;

      // Labels carry a per-connection generation, so a timer left from an
      // earlier connection can never make AddTimer refuse a new one.
      ++m_uGeneration;

      unsigned idx = 0;
      unsigned scheduled = 0;
      unsigned unreadable = 0;
      for (const Entry& e : entries) {
        if (!e.bReadable) { ++unreadable; ++idx; continue; }

        const CString label = CString("dp#") + CString(m_uGeneration) + "." + CString(idx);
        CCmdTimer* pT = new CCmdTimer(this, e.delay, label, e.cmd, IsSecretFlags(e.flags));
        // CModule::AddTimer deletes the timer itself when it refuses it, so
        // pT must not be touched after a false return.
        if (!AddTimer(pT)) {
          PutModule("Warning: could not schedule entry " + CString(idx)
                    + " (AddTimer returned false).");
        } else {
          m_vTimers.push_back(pT);
          ++scheduled;
        }
        ++idx;
      }

      CString msg = CString("Scheduled ") + CString(scheduled) + " delayed command(s).";
      if (unreadable) {
        msg += " Skipped " + CString(unreadable)
             + (unreadable == 1 ? " unreadable entry" : " unreadable entries") + "; see List.";
      }
      PutModule(msg);
    }

    void OnIRCDisconnected() override {
      CleanupTimers();
    }

    // -------- User-visible commands --------

    void CmdHelp(const CString& sLine) {
      // With a filter (e.g. "Help add"), keep ZNC's standard filtered table.
      if (!sLine.Token(1).empty()) {
        HandleHelpCommand(sLine);
        return;
      }

      PutModule("delayedperform " DELAYED_PERFORM_VERSION
                " — run post-connect IRC commands with delays.");
      PutModule("Usage:");
      PutModule("  Help [filter]                — Show this help, or commands matching a filter.");
      PutModule("  Version                      — Show the module version.");
      PutModule("  SetDelay <seconds>           — Set global default delay.");
      PutModule("  Add [seconds] <command>      — Raw IRC or shorthands like /msg, /join.");
      PutModule("  AddSecret [seconds] <cmd>    — Like Add, but command text is masked in module output.");
      PutModule("  List                         — Show index, per-cmd delay, and command.");
      PutModule("  Del <index|all>              — Delete by index or everything.");
      PutModule("  Clear                        — Same as Del all.");
      PutModule("Shorthands: /msg /notice /join /part /quit /nick /topic /mode /kick /invite");
      PutModule("            /ctcp /me (/me needs a target) /whois /away /oper /raw /quote");
      PutModule("            /ns /cs /hs /ms /os /bs (service aliases).");
      PutModule("Vars: %nick% expands to your current IRC nick at send time (e.g., '*%nick%*').");
      PutModule("AddSecret masks output only; it is not encryption. Do not use this module for passwords or other credentials.");
    }

    void CmdVersion(const CString& /*sLine*/) {
      PutModule("delayedperform version " DELAYED_PERFORM_VERSION);
    }

    void CmdSetDelay(const CString& sLine) {
      CString sSec = sLine.Token(1);
      unsigned secs = 0;
      if (sSec.empty() || !IsDigits(sSec)) {
        PutModule("Usage: SetDelay <seconds>");
        return;
      }
      if (!ParseUInt(sSec, secs)) {
        PutModule("SetDelay: seconds must be between 0 and 4294967295.");
        return;
      }
      SetNV("global_delay", CString(secs));
      m_uGlobalDelay = secs;
      PutModule("Global delay set to " + CString(secs) + "s.");
    }

    void CmdAdd(const CString& sLine)       { DoAdd(sLine, /*secret=*/false); }
    void CmdAddSecret(const CString& sLine) { DoAdd(sLine, /*secret=*/true);  }

    void CmdList(const CString& /*sLine*/) {
      vector<Entry> entries;
      LoadAll(entries);

      if (entries.empty()) {
        PutModule("No commands configured. Set a global delay with 'SetDelay <s>' and add with 'Add'.");
        PutModule("Current global delay: " + CString(m_uGlobalDelay) + "s");
        return;
      }

      PutModule("Global delay (default): " + CString(m_uGlobalDelay) + "s");
      PutModule("Index | Delay(s) | Command");
      PutModule("------+----------+-----------------------------");

      unsigned idx = 0;
      for (const Entry& e : entries) {
        CString displayCmd;
        if (!e.bReadable) {
          // Surface unreadable entries so they can be seen and deleted.
          displayCmd = "[corrupt entry]";
        } else if (IsSecretFlags(e.flags)) {
          // Never reveal secret command text here.
          displayCmd = "[hidden]";
        } else {
          displayCmd = ForDisplay(e.cmd);
        }
        CString sDelay = e.bDelayOk ? CString(e.delay) : CString("-");
        PutModule(CString(idx) + "     | " + sDelay + "        | " + displayCmd);
        ++idx;
      }
    }

    void CmdDel(const CString& sLine) {
      CString what = sLine.Token(1);
      if (what.empty()) {
        PutModule("Usage: Del <index|all>");
        return;
      }

      if (what.Equals("all", CString::CaseInsensitive)) {
        ClearAll();
        PutModule("All commands deleted.");
        return;
      }

      if (!IsDigits(what)) {
        PutModule("Del: index must be a non-negative integer.");
        return;
      }

      vector<Entry> entries;
      LoadAll(entries);

      unsigned idx = 0;
      if (!ParseUInt(what, idx) || idx >= entries.size()) {
        PutModule("Del: index out of range.");
        return;
      }

      // Remove only this entry's key. The others keep their keys, values and
      // flags; positions close up because List and Del count positions.
      DelNV(entries[idx].key);

      PutModule("Deleted entry " + CString(idx) + ".");
    }

    void CmdClear(const CString& /*sLine*/) {
      ClearAll();
      PutModule("All commands deleted.");
    }

    // -------- Called from CCmdTimer --------

    // Timers call this just before ZNC deletes them (at end of RunJob for
    // one-shot timers), so m_vTimers never retains dangling pointers.
    void ForgetTimer(CTimer* pT) {
      auto it = std::find(m_vTimers.begin(), m_vTimers.end(), pT);
      if (it != m_vTimers.end()) m_vTimers.erase(it);
    }

    void FireCommand(const CString& sCmd, bool bSecret) {
      if (!GetNetwork() || !GetNetwork()->IsIRCConnected()) {
        PutModule(bSecret ? CString("Skipped (not connected): [hidden]")
                          : CString("Skipped (not connected): ") + ForDisplay(sCmd));
        return;
      }

      // Validate the %nick% substitution source against IRC nick grammar
      // before splicing it into a line destined for PutIRC().
      CString toSend;
      if (!ExpandVars(sCmd, toSend)) {
        PutModule(bSecret ? CString("Skipped (invalid nick for expansion): [hidden]")
                          : CString("Skipped (invalid nick for expansion): ") + ForDisplay(sCmd));
        return;
      }

      // Defense in depth: refuse to inject CR/LF/NUL into the IRC stream.
      // Add rejects them, so this covers stored values from other sources
      // (e.g. an edited registry). The message shows the stored command with
      // those bytes escaped, never raw.
      if (HasBadChars(toSend)) {
        PutModule(bSecret ? CString("Skipped (contains control chars): [hidden]")
                          : CString("Skipped (contains control chars): ") + ForDisplay(sCmd));
        return;
      }

      PutIRC(toSend);
      PutModule(bSecret ? CString("Ran: [hidden]")
                        : CString("Ran: ") + toSend);
    }

  private:
    // ---- Types ----
    struct Entry {
      CString  key;               // NV key, e.g. "cmd.7"
      bool     bReadable = false; // value parsed, payload valid and non-empty
      bool     bDelayOk  = false; // delay field parsed (shown even if unreadable)
      unsigned delay = 0;
      CString  flags;             // e.g. "s" for secret, "" for normal; future-extensible
      CString  cmd;               // decoded command text (when readable)
    };

    enum ESlash { SLASH_NONE, SLASH_OK, SLASH_INCOMPLETE };

    // ---- State ----
    unsigned        m_uGlobalDelay{0};
    unsigned        m_uGeneration{0};
    vector<CTimer*> m_vTimers;

    // ---- Timer cleanup ----
    void CleanupTimers() {
      // Swap out first so any reentrant bookkeeping can't observe stale state.
      vector<CTimer*> snapshot;
      snapshot.swap(m_vTimers);
      for (CTimer* pT : snapshot) {
        if (pT) RemTimer(pT);
      }
    }

    // ---- Utilities ----
    static bool IsDigits(const CString& s) {
      if (s.empty()) return false;
      for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
      }
      return true;
    }

    // Digits only, and within unsigned int. CString::ToUInt would silently
    // truncate larger values to 32 bits (4294967296 -> 0).
    static bool ParseUInt(const CString& s, unsigned& out) {
      if (!IsDigits(s)) return false;
      unsigned long long v = 0;
      for (size_t i = 0; i < s.length(); ++i) {
        v = v * 10 + static_cast<unsigned>(s[i] - '0');
        if (v > UINT_MAX) return false;
      }
      out = static_cast<unsigned>(v);
      return true;
    }

    // True for tokens that are clearly meant as a number but are not a
    // plain non-negative integer: a sign, a decimal point, or both
    // ("-3", "+5", "1.5", ".5"). No IRC command starts like that.
    static bool LooksNumeric(const CString& s) {
      size_t i = 0;
      if (i < s.length() && (s[i] == '+' || s[i] == '-')) ++i;
      bool digit = false, dot = false;
      for (; i < s.length(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') { digit = true; continue; }
        if (c == '.' && !dot)     { dot = true;   continue; }
        return false;
      }
      return digit;
    }

    // Reject control characters that could let a stored/expanded command
    // inject additional lines into the IRC stream.
    static bool HasBadChars(const CString& s) {
      for (size_t i = 0; i < s.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r' || c == '\n' || c == '\0') return true;
      }
      return false;
    }

    // Text for module output: CR, LF and NUL are shown as \r, \n and \0, so a
    // stored value can never split or terminate a line the client receives.
    static CString ForDisplay(const CString& s) {
      CString out;
      out.reserve(s.length());
      for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '\r')      out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c == '\0') out += "\\0";
        else                out += c;
      }
      return out;
    }

    // Base64 as ZNC writes it: alphabet only, '=' only as trailing padding.
    // Missing padding is tolerated (ZNC's decoder accepts it).
    static bool IsValidB64(const CString& s) {
      if (s.empty()) return false;
      size_t n = s.length();
      size_t pad = 0;
      while (pad < n && pad < 2 && s[n - 1 - pad] == '=') ++pad;
      size_t body = n - pad;
      for (size_t i = 0; i < body; ++i) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!ok) return false;
      }
      if (pad) return n % 4 == 0;
      return body % 4 != 1;   // a single trailing symbol encodes no byte
    }

    // Strict IRC-nick grammar check used before splicing %nick% into a line
    // destined for PutIRC(). Letters, digits, the RFC 2812 "special"
    // characters, and hyphen. Anything else fails.
    static bool IsValidIRCNick(const CString& nick) {
      if (nick.empty()) return false;
      for (size_t i = 0; i < nick.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(nick[i]);
        bool isLetter  = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool isDigit   = (c >= '0' && c <= '9');
        // RFC 2812 "special": [ ] \ ` _ ^ { | }   (plus hyphen in practice)
        bool isSpecial = (c == '[' || c == ']' || c == '\\' || c == '`' ||
                          c == '_' || c == '^'  || c == '{'  || c == '|' ||
                          c == '}' || c == '-');
        if (!isLetter && !isDigit && !isSpecial) return false;
      }
      return true;
    }

    static bool IsSecretFlags(const CString& flags) {
      for (size_t i = 0; i < flags.length(); ++i) {
        char c = flags[i];
        if (c == 's' || c == 'S') return true;
      }
      return false;
    }

    static CString MakeFlags(bool bSecret) {
      return bSecret ? CString("s") : CString();
    }

    static CString EnsureColon(const CString& s) {
      CString t = s;
      t.TrimLeft();
      if (!t.empty() && t[0] == ':') return s; // keep original
      return CString(":") + s;
    }

    // ---- Core Add logic (shared by Add and AddSecret) ----
    void DoAdd(const CString& sLine, bool bSecret) {
      const CString cmdName = bSecret ? "AddSecret" : "Add";

      // Syntax:
      //  Add 5 JOIN #znc
      //  Add /msg NickServ IDENTIFY foo
      //  AddSecret 8 /ns IDENTIFY hunter2
      CString tok1 = sLine.Token(1);
      unsigned per = m_uGlobalDelay;
      CString cmd;

      if (IsDigits(tok1)) {
        if (!ParseUInt(tok1, per)) {
          PutModule(cmdName + ": the delay must be a whole number of seconds from 0 to 4294967295.");
          return;
        }
        cmd = sLine.Token(2, true);
      } else if (LooksNumeric(tok1)) {
        PutModule(cmdName + ": the delay must be a whole number of seconds from 0 to 4294967295.");
        return;
      } else {
        cmd = sLine.Token(1, true);
      }

      cmd.TrimLeft();
      if (cmd.empty()) {
        PutModule("Usage: " + cmdName + " [seconds] <irc-or-slash-command>");
        return;
      }

      // Normalize client-style '/...' to raw IRC if applicable
      CString normalized, usage;
      switch (NormalizeSlash(cmd, normalized, usage)) {
        case SLASH_OK:
          cmd = normalized;
          break;
        case SLASH_INCOMPLETE:
          PutModule(cmdName + ": incomplete shorthand. Usage: " + usage);
          return;
        case SLASH_NONE:
          break;
      }

      if (cmd.Trim_n().empty()) {
        PutModule("Usage: " + cmdName + " [seconds] <irc-or-slash-command>");
        return;
      }

      // Reject control characters now, before persisting.
      if (HasBadChars(cmd)) {
        PutModule(cmdName + ": command contains CR/LF/NUL; rejected.");
        return;
      }

      unsigned next = 0;
      if (!NextKey(next)) {
        PutModule(cmdName + ": no free entry key (cmd.4294967295 is in use). Delete the last entry or use Clear.");
        return;
      }

      // The index reported is the entry's position in List, which is what
      // Del accepts; with gaps in the stored keys it differs from the key.
      vector<Entry> entries;
      LoadAll(entries);
      const unsigned pos = static_cast<unsigned>(entries.size());

      CString enc   = CString(cmd).Base64Encode_n();
      CString value = CString(per) + "|" + MakeFlags(bSecret) + "|" + enc;
      SetNV(KeyFor(next), value);

      if (bSecret) {
        // Do not echo the stored text of a secret command.
        PutModule("Added [" + CString(pos) + "]: delay=" + CString(per) + "s, cmd=[hidden]");
      } else {
        PutModule("Added [" + CString(pos) + "]: delay=" + CString(per) + "s, cmd=" + cmd);
      }
    }

    void LoadGlobalDelay() {
      unsigned v = 0;
      m_uGlobalDelay = ParseUInt(GetNV("global_delay"), v) ? v : 0;
    }

    static CString KeyFor(unsigned idx) {
      return CString("cmd.") + CString(idx);
    }

    // Numeric part of a cmd.<n> key, if it has one within range.
    static bool KeyNumber(const CString& k, unsigned& n) {
      if (!k.StartsWith("cmd.")) return false;
      return ParseUInt(k.substr(4), n);
    }

    // One above the highest numeric key, so a new entry sorts last. False if
    // the highest key is already the largest possible.
    bool NextKey(unsigned& out) {
      unsigned maxIdx = 0;
      bool any = false;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        unsigned n = 0;
        if (KeyNumber(it->first, n)) {
          if (!any || n > maxIdx) maxIdx = n;
          any = true;
        }
      }
      if (!any) { out = 0; return true; }
      if (maxIdx == UINT_MAX) return false;
      out = maxIdx + 1;
      return true;
    }

    // Parses one stored value into e. Accepts both the v1.1+ format
    // "delay|flags|b64" and the legacy v1.0 format "delay|b64". Base64 never
    // contains '|', so the split is unambiguous.
    static void ParseValue(const CString& val, Entry& e) {
      CString::size_type pos1 = val.find('|');
      if (pos1 == CString::npos) return;
      e.bDelayOk = ParseUInt(val.substr(0, pos1), e.delay);

      CString rest = val.substr(pos1 + 1);
      CString b64;
      CString::size_type pos2 = rest.find('|');
      if (pos2 == CString::npos) {
        b64 = rest;                  // legacy v1.0 entry: no flags segment
      } else {
        e.flags = rest.substr(0, pos2);
        b64     = rest.substr(pos2 + 1);
      }

      if (!e.bDelayOk || !IsValidB64(b64)) return;
      e.cmd = b64.Base64Decode_n();
      e.bReadable = !e.cmd.empty();
    }

    void LoadAll(vector<Entry>& out) {
      out.clear();
      vector<std::pair<unsigned, Entry>> tmp;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        unsigned n = 0;
        if (!KeyNumber(it->first, n)) continue;   // other keys are ignored
        Entry e;
        e.key = it->first;
        ParseValue(it->second, e);
        tmp.emplace_back(n, std::move(e));
      }
      std::sort(tmp.begin(), tmp.end(),
                [](const std::pair<unsigned, Entry>& a,
                   const std::pair<unsigned, Entry>& b){
                  return a.first < b.first;
                });
      for (auto& kv : tmp) out.push_back(std::move(kv.second));
    }

    void ClearAll() {
      vector<CString> toDel;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        if (it->first.StartsWith("cmd.")) toDel.push_back(it->first);
      }
      for (const CString& k : toDel) DelNV(k);
    }

    // ---- Variable expansion ----
    // Return false if a required substitution would splice in an unsafe
    // value (currently: a nick that doesn't conform to IRC nick grammar).
    // On true, `out` holds the fully-expanded command; on false the caller
    // must not transmit it.
    bool ExpandVars(const CString& in, CString& out) {
      out = in;
      // Match case-sensitively, like Replace() below, so only commands that
      // will actually be substituted are nick-validated.
      if (out.Find("%nick%", CString::CaseSensitive) == CString::npos)
        return true;                             // no substitution needed
      if (!GetNetwork()) return true;            // nothing we can substitute

      CString curr = GetNetwork()->GetIRCNick().GetNick();
      if (!IsValidIRCNick(curr)) return false;   // refuse to splice bad nick

      out.Replace("%nick%", curr); // works inside surrounding text, e.g., "*%nick%*"
      return true;
    }

    // ---- Slash-command normalization ----

    // SLASH_NONE: 'in' is not a slash command; store it as raw IRC.
    // SLASH_OK: 'out' holds the raw IRC line.
    // SLASH_INCOMPLETE: a supported shorthand is missing a required
    //   argument; 'usage' holds its usage line and nothing is stored.
    ESlash NormalizeSlash(const CString& in, CString& out, CString& usage) {
      CString s = in;
      s.TrimLeft();
      if (!s.StartsWith("/")) { out = in; return SLASH_NONE; }

      CString verb = s.Token(0);           // includes leading '/'
      CString rest = s.Token(1, true);     // the remainder (may be empty)

      auto Is = [&verb](const char* v){ return verb.Equals(v, CString::CaseInsensitive); };
      auto Incomplete = [&usage](const CString& u){ usage = u; return SLASH_INCOMPLETE; };

      // Service aliases
      if (Is("/ns") || Is("/cs") || Is("/hs") || Is("/ms") || Is("/os") || Is("/bs")) {
        CString svc;
        if      (Is("/ns")) svc = "NickServ";
        else if (Is("/cs")) svc = "ChanServ";
        else if (Is("/hs")) svc = "HostServ";
        else if (Is("/ms")) svc = "MemoServ";
        else if (Is("/os")) svc = "OperServ";
        else                svc = "BotServ";
        if (rest.empty()) return Incomplete(verb.AsLower() + " <text>");
        out = "PRIVMSG " + svc + " " + EnsureColon(rest);
        return SLASH_OK;
      }

      // Common shorthands
      if (Is("/msg") || Is("/notice")) {
        CString target = s.Token(1);
        CString text   = s.Token(2, true);
        if (target.empty() || text.empty()) return Incomplete(verb.AsLower() + " <target> <text>");
        out = CString(Is("/msg") ? "PRIVMSG " : "NOTICE ") + target + " " + EnsureColon(text);
        return SLASH_OK;
      }

      if (Is("/ctcp")) {
        CString target = s.Token(1);
        CString payload = s.Token(2, true);
        if (target.empty() || payload.empty()) return Incomplete("/ctcp <target> <command>");
        CString ctcp = CString("\x01") + payload + CString("\x01");
        out = "PRIVMSG " + target + " " + EnsureColon(ctcp);
        return SLASH_OK;
      }

      if (Is("/me")) {
        CString target = s.Token(1);
        CString action = s.Token(2, true);
        if (target.empty() || action.empty()) return Incomplete("/me <target> <action>");
        CString ctcp = CString("\x01") + CString("ACTION ") + action + CString("\x01");
        out = "PRIVMSG " + target + " " + EnsureColon(ctcp);
        return SLASH_OK;
      }

      if (Is("/join")) {
        CString chan = s.Token(1);
        CString key  = s.Token(2);
        if (chan.empty()) return Incomplete("/join <#chan> [key]");
        out = "JOIN " + chan + (key.empty() ? "" : CString(" ") + key);
        return SLASH_OK;
      }

      if (Is("/part")) {
        CString chan = s.Token(1);
        CString msg  = s.Token(2, true);
        if (chan.empty()) return Incomplete("/part <#chan> [reason]");
        out = "PART " + chan + (msg.empty() ? "" : CString(" ") + EnsureColon(msg));
        return SLASH_OK;
      }

      if (Is("/quit")) {
        out = "QUIT" + (rest.empty() ? "" : CString(" ") + EnsureColon(rest));
        return SLASH_OK;
      }

      if (Is("/nick")) {
        CString newnick = s.Token(1);
        if (newnick.empty()) return Incomplete("/nick <nick>");
        out = "NICK " + newnick;
        return SLASH_OK;
      }

      if (Is("/topic")) {
        CString chan  = s.Token(1);
        CString topic = s.Token(2, true);
        if (chan.empty()) return Incomplete("/topic <#chan> [topic]");
        out = "TOPIC " + chan + (topic.empty() ? "" : CString(" ") + EnsureColon(topic));
        return SLASH_OK;
      }

      if (Is("/mode")) {
        CString target = s.Token(1);
        CString args   = s.Token(2, true);
        if (target.empty()) return Incomplete("/mode <target> [modes]");
        out = "MODE " + target + (args.empty() ? "" : CString(" ") + args);
        return SLASH_OK;
      }

      if (Is("/kick")) {
        CString chan   = s.Token(1);
        CString nick   = s.Token(2);
        CString reason = s.Token(3, true);
        if (chan.empty() || nick.empty()) return Incomplete("/kick <#chan> <nick> [reason]");
        out = "KICK " + chan + " " + nick + (reason.empty() ? "" : CString(" ") + EnsureColon(reason));
        return SLASH_OK;
      }

      if (Is("/invite")) {
        CString nick = s.Token(1);
        CString chan = s.Token(2);
        if (nick.empty() || chan.empty()) return Incomplete("/invite <nick> <#chan>");
        out = "INVITE " + nick + " " + chan;
        return SLASH_OK;
      }

      if (Is("/whois")) {
        CString a = s.Token(1);
        CString b = s.Token(2);
        if (a.empty()) return Incomplete("/whois [server] <nick>");
        if (b.empty()) { out = "WHOIS " + a; return SLASH_OK; }
        bool bLooksServer = (b.Find(".") != CString::npos) || (b.Find(":") != CString::npos);
        if (bLooksServer) out = "WHOIS " + b + " " + a; else out = "WHOIS " + a + " " + b;
        return SLASH_OK;
      }

      if (Is("/away")) {
        out = "AWAY" + (rest.empty() ? "" : CString(" ") + EnsureColon(rest));
        return SLASH_OK;
      }

      if (Is("/oper")) {
        CString name = s.Token(1);
        CString pass = s.Token(2);
        if (name.empty() || pass.empty()) return Incomplete("/oper <name> <password>");
        out = "OPER " + name + " " + pass;
        return SLASH_OK;
      }

      if (Is("/raw") || Is("/quote")) {
        if (rest.Trim_n().empty()) return Incomplete(verb.AsLower() + " <irc-line>");
        out = rest; // pass-through
        return SLASH_OK;
      }

      // Generic fallback: strip '/', uppercase verb, keep args
      CString bareVerb = verb.substr(1);
      if (bareVerb.empty()) return Incomplete("/<command> [arguments]");
      bareVerb.MakeUpper();
      out = bareVerb + (rest.empty() ? "" : CString(" ") + rest);
      return SLASH_OK;
    }
};

// ---- Timer fires here (after class is fully defined) ----
void CCmdTimer::RunJob() {
  auto* p = dynamic_cast<CDelayedPerformModule*>(GetModule());
  if (!p) return;
  p->FireCommand(m_sCommand, m_bSecret);
  // Drop our pointer from the module's vector before ZNC deletes us.
  p->ForgetTimer(this);
}

// ---- Module metadata ----
template<> void TModInfo<CDelayedPerformModule>(CModInfo& Info) {
  Info.SetWikiPage("Modules");
  Info.SetDescription("Perform commands after connect, with per-command or global delay "
                      "(+slash aliases). v" DELAYED_PERFORM_VERSION);
}

MODULEDEFS(CDelayedPerformModule,
           "Perform commands after connect, with per-command or global delay "
           "(+slash aliases). v" DELAYED_PERFORM_VERSION)
