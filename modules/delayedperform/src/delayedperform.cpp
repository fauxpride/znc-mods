/*
 * delayedperform.cpp — ZNC module to run post-connect commands with delays.
 *
 * Version: 1.1.1  (see DELAYED_PERFORM_VERSION below; also queryable via
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
 *
 * Variables expanded at send time:
 *   %nick%  — current IRC nick (after reconnect/fallback). Works inside strings.
 *            The nick is validated against IRC nick grammar before splicing;
 *            if it contains any character outside that grammar (space, ':',
 *            CR, LF, etc.), the command is skipped rather than sent with a
 *            malformed nick spliced in.
 *
 * Storage format (NV value for each cmd.<index> key):
 *   "<delay>|<flags>|<base64>"          -- v1.1+ entries (flags may be empty)
 *   "<delay>|<base64>"                  -- v1.0 legacy entries (also loaded)
 *
 * Recognized flag characters:
 *   's'  — secret: command text is never echoed in module output
 */

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>   // for CIRCNetwork::IsIRCConnected
#include <algorithm>
#include <vector>
#include <cctype>   // isdigit

using std::vector;

// --- Version string (queryable; see CmdVersion / TModInfo / MODULEDEFS) ---
#define DELAYED_PERFORM_VERSION "1.1.1"

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
      AddHelpCommand();
      AddCommand("Help",      static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdHelp),
                 "", "Show detailed help.");
      AddCommand("Version",   static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdVersion),
                 "", "Show the module version.");
      AddCommand("SetDelay",  static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdSetDelay),
                 "<seconds>", "Set global default delay (seconds) used when 'Add' omits a delay.");
      AddCommand("Add",       static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdAdd),
                 "[seconds] <irc-or-slash-command>",
                 "Add a command. Accepts raw IRC or common '/'-style shorthands.");
      AddCommand("AddSecret", static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdAddSecret),
                 "[seconds] <irc-or-slash-command>",
                 "Same as Add, but the command text is hidden in List and log output.");
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

      unsigned idx = 0;
      unsigned scheduled = 0;
      for (const Entry& e : entries) {
        CString cmd = DecodeB64(e.b64);
        if (cmd.empty()) { ++idx; continue; } // skip corrupt / empty payload

        const CString label = CString("dp#") + CString(idx);
        CCmdTimer* pT = new CCmdTimer(this, e.delay, label, cmd, IsSecretFlags(e.flags));
        // Fix #6: respect AddTimer's return; on failure we own the pointer.
        if (!AddTimer(pT)) {
          PutModule("Warning: could not schedule entry " + CString(idx)
                    + " (AddTimer returned false).");
          delete pT;
        } else {
          m_vTimers.push_back(pT);
          ++scheduled;
        }
        ++idx;
      }

      PutModule(CString("Scheduled ") + CString(scheduled) + " delayed command(s).");
    }

    void OnIRCDisconnected() override {
      CleanupTimers();
    }

    // -------- User-visible commands --------

    void CmdHelp(const CString& /*sLine*/) {
      PutModule("delayedperform " DELAYED_PERFORM_VERSION
                " — run post-connect IRC commands with delays.");
      PutModule("Usage:");
      PutModule("  Version                      — Show the module version.");
      PutModule("  SetDelay <seconds>           — Set global default delay.");
      PutModule("  Add [seconds] <command>      — Raw IRC or shorthands like /msg, /join.");
      PutModule("  AddSecret [seconds] <cmd>    — Like Add, but command text is hidden in output.");
      PutModule("  List                         — Show index, per-cmd delay, and command.");
      PutModule("  Del <index|all>              — Delete by index or everything.");
      PutModule("  Clear                        — Same as Del all.");
      PutModule("Shorthands: /msg /notice /join /part /quit /nick /topic /mode /kick /invite");
      PutModule("            /ctcp /me (/me needs a target) /whois /away /oper /raw /quote");
      PutModule("            /ns /cs /hs /ms /os /bs (service aliases).");
      PutModule("Vars: %nick% expands to your current IRC nick at send time (e.g., '*%nick%*').");
      PutModule("Use AddSecret for entries that contain passwords (NickServ IDENTIFY, OPER, ...).");
    }

    void CmdVersion(const CString& /*sLine*/) {
      PutModule("delayedperform version " DELAYED_PERFORM_VERSION);
    }

    void CmdSetDelay(const CString& sLine) {
      CString sSec = sLine.Token(1);
      if (sSec.empty() || !IsUIntStr(sSec)) {
        PutModule("Usage: SetDelay <seconds>");
        return;
      }
      unsigned secs = sSec.ToUInt();
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
        if (IsSecretFlags(e.flags)) {
          // Fix #2: never reveal secret command text here.
          displayCmd = "[hidden]";
        } else {
          // Fix #7: surface corrupt entries instead of silently showing blanks.
          CString decoded = DecodeB64(e.b64);
          displayCmd = decoded.empty() ? CString("[corrupt entry]") : decoded;
        }
        PutModule(CString(idx) + "     | " + CString(e.delay) + "        | " + displayCmd);
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

      if (!IsUIntStr(what)) {
        PutModule("Del: index must be a non-negative integer.");
        return;
      }

      unsigned idx = what.ToUInt();
      vector<Entry> entries;
      LoadAll(entries);

      if (idx >= entries.size()) {
        PutModule("Del: index out of range.");
        return;
      }

      vector<Entry> kept;
      kept.reserve(entries.size() - 1);
      for (unsigned i = 0; i < entries.size(); ++i) {
        if (i != idx) kept.push_back(entries[i]);
      }
      RewriteAll(kept);

      PutModule("Deleted entry " + CString(idx) + ".");
    }

    void CmdClear(const CString& /*sLine*/) {
      ClearAll();
      PutModule("All commands deleted.");
    }

    // -------- Called from CCmdTimer --------

    // Fix #1: timers call this just before ZNC deletes them (at end of
    // RunJob for one-shot timers), so m_vTimers never retains dangling
    // pointers into freed memory.
    void ForgetTimer(CTimer* pT) {
      auto it = std::find(m_vTimers.begin(), m_vTimers.end(), pT);
      if (it != m_vTimers.end()) m_vTimers.erase(it);
    }

    void FireCommand(const CString& sCmd, bool bSecret) {
      if (!GetNetwork() || !GetNetwork()->IsIRCConnected()) {
        PutModule(bSecret ? CString("Skipped (not connected): [hidden]")
                          : CString("Skipped (not connected): ") + sCmd);
        return;
      }

      // Fix #4: validate the %nick% substitution source against IRC nick
      // grammar before splicing it into a line destined for PutIRC(). If
      // the current nick contains any character outside that grammar
      // (space, ':', ',', CR, LF, etc.) we refuse to send rather than
      // risk a malformed/split IRC line.
      CString toSend;
      if (!ExpandVars(sCmd, toSend)) {
        PutModule(bSecret ? CString("Skipped (invalid nick for expansion): [hidden]")
                          : CString("Skipped (invalid nick for expansion): ") + sCmd);
        return;
      }

      // Fix #3 (defense-in-depth): even if something slipped past CmdAdd's
      // pre-storage check, refuse to inject CR/LF/NUL into the IRC stream.
      // Note: with the new ExpandVars, a bad nick is already caught above;
      // this remaining check covers stored commands whose body contains
      // control chars from some other source (e.g. imported configs).
      if (HasBadChars(toSend)) {
        PutModule(bSecret ? CString("Skipped (contains control chars): [hidden]")
                          : CString("Skipped (contains control chars): ") + toSend);
        return;
      }

      PutIRC(toSend);
      PutModule(bSecret ? CString("Ran: [hidden]")
                        : CString("Ran: ") + toSend);
    }

  private:
    // ---- Types ----
    struct Entry {
      unsigned delay = 0;
      CString  flags;   // e.g. "s" for secret, "" for normal; future-extensible
      CString  b64;     // base64-encoded command text
    };

    // ---- State ----
    unsigned        m_uGlobalDelay{0};
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
    static bool IsUIntStr(const CString& s) {
      if (s.empty()) return false;
      for (size_t i = 0; i < s.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isdigit(c)) return false;
      }
      return true;
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

    // Fix #4: strict IRC-nick grammar check used before splicing %nick%
    // into a line destined for PutIRC(). We permit letters, digits, and
    // the "special" characters from RFC 2812 nick grammar (including
    // hyphen, which is widely accepted in practice). Anything else —
    // space, ':', ',', '@', '!', CR, LF, NUL, non-ASCII, ... — fails.
    // A non-conforming nick could split or reframe the IRC line; in that
    // case the caller should refuse to send rather than splice it in.
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
      const char* cmdName = bSecret ? "AddSecret" : "Add";

      // Syntax:
      //  Add 5 JOIN #znc
      //  Add /msg NickServ IDENTIFY foo
      //  AddSecret 8 /ns IDENTIFY hunter2
      CString tok1 = sLine.Token(1);
      unsigned per = m_uGlobalDelay;
      CString cmd;

      if (!tok1.empty() && IsUIntStr(tok1)) {
        per = tok1.ToUInt();
        cmd = sLine.Token(2, true);
      } else {
        cmd = sLine.Token(1, true);
      }

      cmd.TrimLeft();
      if (cmd.empty()) {
        PutModule(CString("Usage: ") + cmdName + " [seconds] <irc-or-slash-command>");
        return;
      }

      // Normalize client-style '/...' to raw IRC if applicable
      CString normalized;
      if (NormalizeSlash(cmd, normalized)) {
        cmd = normalized;
      }

      // Fix #3: reject control characters now, before persisting.
      if (HasBadChars(cmd)) {
        PutModule(CString(cmdName) + ": command contains CR/LF/NUL; rejected.");
        return;
      }

      CString enc   = CString(cmd).Base64Encode_n();
      CString value = CString(per) + "|" + MakeFlags(bSecret) + "|" + enc;

      unsigned next = NextIndex();
      SetNV(KeyFor(next), value);

      if (bSecret) {
        // Fix #2: do not echo the stored text of a secret command.
        PutModule("Added [" + CString(next) + "]: delay=" + CString(per) + "s, cmd=[hidden]");
      } else {
        PutModule("Added [" + CString(next) + "]: delay=" + CString(per) + "s, cmd=" + cmd);
      }
    }

    void LoadGlobalDelay() {
      CString v = GetNV("global_delay");
      m_uGlobalDelay = IsUIntStr(v) ? v.ToUInt() : 0;
    }

    static CString KeyFor(unsigned idx) {
      return CString("cmd.") + CString(idx);
    }

    unsigned NextIndex() {
      unsigned maxIdx = 0;
      bool any = false;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        const CString& k = it->first;
        if (k.StartsWith("cmd.")) {
          CString sNum = k.substr(4);
          if (IsUIntStr(sNum)) {
            unsigned n = sNum.ToUInt();
            if (!any || n > maxIdx) maxIdx = n;
            any = true;
          }
        }
      }
      return any ? (maxIdx + 1) : 0;
    }

    // Accepts both the v1.1+ format "delay|flags|b64" and the legacy
    // v1.0 format "delay|b64" (in which case flagsOut is empty).
    // Base64 never contains '|', so the split is unambiguous.
    static bool ParseValue(const CString& val,
                           unsigned& delayOut,
                           CString&  flagsOut,
                           CString&  b64Out) {
      int pos1 = val.Find("|");
      if (pos1 < 0) return false;
      CString sDelay = val.Left(pos1);
      CString rest   = val.substr(pos1 + 1);
      if (!IsUIntStr(sDelay)) return false;
      delayOut = sDelay.ToUInt();

      int pos2 = rest.Find("|");
      if (pos2 < 0) {
        // legacy v1.0 entry: no flags segment
        flagsOut = "";
        b64Out   = rest;
      } else {
        flagsOut = rest.Left(pos2);
        b64Out   = rest.substr(pos2 + 1);
      }
      return true;
    }

    static CString DecodeB64(const CString& b64) {
      if (b64.empty()) return CString();
      return b64.Base64Decode_n();
    }

    void LoadAll(vector<Entry>& out) {
      out.clear();
      vector<std::pair<unsigned, CString>> tmp; // (index, raw NV value)
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        const CString& k = it->first;
        if (!k.StartsWith("cmd.")) continue;
        CString sNum = k.substr(4);
        if (!IsUIntStr(sNum)) continue;
        tmp.emplace_back(sNum.ToUInt(), it->second);
      }
      std::sort(tmp.begin(), tmp.end(),
                [](const std::pair<unsigned, CString>& a,
                   const std::pair<unsigned, CString>& b){
                  return a.first < b.first;
                });

      for (const auto& kv : tmp) {
        Entry e;
        if (ParseValue(kv.second, e.delay, e.flags, e.b64)) {
          out.push_back(std::move(e));
        }
      }
    }

    void RewriteAll(const vector<Entry>& entries) {
      // Clear existing cmd.* keys
      vector<CString> toDel;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        if (it->first.StartsWith("cmd.")) toDel.push_back(it->first);
      }
      for (const CString& k : toDel) DelNV(k);

      // Write back contiguous indices 0..N-1 in the v1.1 format.
      unsigned idx = 0;
      for (const Entry& e : entries) {
        SetNV(KeyFor(idx), CString(e.delay) + "|" + e.flags + "|" + e.b64);
        ++idx;
      }
    }

    void ClearAll() {
      vector<CString> toDel;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        if (it->first.StartsWith("cmd.")) toDel.push_back(it->first);
      }
      for (const CString& k : toDel) DelNV(k);
    }

    // ---- Variable expansion ----
    // Fix #4: return false if a required substitution would splice in
    // an unsafe value (currently: a nick that doesn't conform to IRC
    // nick grammar). Callers should treat false as "skip this send".
    // On true, `out` holds the fully-expanded command; on false, `out`
    // is left unmodified and the caller must not transmit it.
    bool ExpandVars(const CString& in, CString& out) {
      out = in;
      if (out.Find("%nick%") < 0) return true;   // no substitution needed
      if (!GetNetwork()) return true;            // nothing we can substitute

      CString curr = GetNetwork()->GetIRCNick().GetNick();
      if (!IsValidIRCNick(curr)) return false;   // refuse to splice bad nick

      out.Replace("%nick%", curr); // works inside surrounding text, e.g., "*%nick%*"
      return true;
    }

    // ---- Slash-command normalization ----

    // Return true if 'in' starts with '/', placing normalized IRC into 'out'.
    bool NormalizeSlash(const CString& in, CString& out) {
      CString s = in;
      s.TrimLeft();
      if (!s.StartsWith("/")) { out = in; return false; }

      CString verb = s.Token(0);           // includes leading '/'
      CString rest = s.Token(1, true);     // the remainder (may be empty)

      auto U = [](CString x){ x.MakeUpper(); return x; };

      // Service aliases
      if (verb.Equals("/ns", CString::CaseInsensitive) ||
          verb.Equals("/cs", CString::CaseInsensitive) ||
          verb.Equals("/hs", CString::CaseInsensitive) ||
          verb.Equals("/ms", CString::CaseInsensitive) ||
          verb.Equals("/os", CString::CaseInsensitive) ||
          verb.Equals("/bs", CString::CaseInsensitive)) {
        CString svc;
        if      (verb.Equals("/ns", CString::CaseInsensitive)) svc = "NickServ";
        else if (verb.Equals("/cs", CString::CaseInsensitive)) svc = "ChanServ";
        else if (verb.Equals("/hs", CString::CaseInsensitive)) svc = "HostServ";
        else if (verb.Equals("/ms", CString::CaseInsensitive)) svc = "MemoServ";
        else if (verb.Equals("/os", CString::CaseInsensitive)) svc = "OperServ";
        else                                                   svc = "BotServ";
        if (rest.empty()) return false;
        out = "PRIVMSG " + svc + " " + EnsureColon(rest);
        return true;
      }

      // Common shorthands
      if (verb.Equals("/msg", CString::CaseInsensitive)) {
        CString target = s.Token(1);
        CString text   = s.Token(2, true);
        if (target.empty() || text.empty()) return false;
        out = "PRIVMSG " + target + " " + EnsureColon(text);
        return true;
      }

      if (verb.Equals("/notice", CString::CaseInsensitive)) {
        CString target = s.Token(1);
        CString text   = s.Token(2, true);
        if (target.empty() || text.empty()) return false;
        out = "NOTICE " + target + " " + EnsureColon(text);
        return true;
      }

      if (verb.Equals("/ctcp", CString::CaseInsensitive)) {
        CString target = s.Token(1);
        CString payload = s.Token(2, true);
        if (target.empty() || payload.empty()) return false;
        CString ctcp = CString("\x01") + payload + CString("\x01");
        out = "PRIVMSG " + target + " " + EnsureColon(ctcp);
        return true;
      }

      if (verb.Equals("/me", CString::CaseInsensitive)) {
        CString target = s.Token(1);
        CString action = s.Token(2, true);
        if (target.empty() || action.empty()) return false;
        CString ctcp = CString("\x01") + CString("ACTION ") + action + CString("\x01");
        out = "PRIVMSG " + target + " " + EnsureColon(ctcp);
        return true;
      }

      if (verb.Equals("/join", CString::CaseInsensitive)) {
        CString chan = s.Token(1);
        CString key  = s.Token(2);
        if (chan.empty()) return false;
        out = "JOIN " + chan + (key.empty() ? "" : CString(" ") + key);
        return true;
      }

      if (verb.Equals("/part", CString::CaseInsensitive)) {
        CString chan = s.Token(1);
        CString msg  = s.Token(2, true);
        if (chan.empty()) return false;
        out = "PART " + chan + (msg.empty() ? "" : CString(" ") + EnsureColon(msg));
        return true;
      }

      if (verb.Equals("/quit", CString::CaseInsensitive)) {
        CString msg = rest;
        out = "QUIT" + (msg.empty() ? "" : CString(" ") + EnsureColon(msg));
        return true;
      }

      if (verb.Equals("/nick", CString::CaseInsensitive)) {
        CString newnick = s.Token(1);
        if (newnick.empty()) return false;
        out = "NICK " + newnick;
        return true;
      }

      if (verb.Equals("/topic", CString::CaseInsensitive)) {
        CString chan  = s.Token(1);
        CString topic = s.Token(2, true);
        if (chan.empty()) return false;
        out = "TOPIC " + chan + (topic.empty() ? "" : CString(" ") + EnsureColon(topic));
        return true;
      }

      if (verb.Equals("/mode", CString::CaseInsensitive)) {
        CString target = s.Token(1);
        CString args   = s.Token(2, true);
        if (target.empty()) return false;
        out = "MODE " + target + (args.empty() ? "" : CString(" ") + args);
        return true;
      }

      if (verb.Equals("/kick", CString::CaseInsensitive)) {
        CString chan   = s.Token(1);
        CString nick   = s.Token(2);
        CString reason = s.Token(3, true);
        if (chan.empty() || nick.empty()) return false;
        out = "KICK " + chan + " " + nick + (reason.empty() ? "" : CString(" ") + EnsureColon(reason));
        return true;
      }

      if (verb.Equals("/invite", CString::CaseInsensitive)) {
        CString nick = s.Token(1);
        CString chan = s.Token(2);
        if (nick.empty() || chan.empty()) return false;
        out = "INVITE " + nick + " " + chan;
        return true;
      }

      if (verb.Equals("/whois", CString::CaseInsensitive)) {
        CString a = s.Token(1);
        CString b = s.Token(2);
        if (a.empty() && b.empty()) return false;
        if (!a.empty() && b.empty()) { out = "WHOIS " + a; return true; }
        bool bLooksServer = (b.Find(".") >= 0) || (b.Find(":") >= 0);
        if (bLooksServer) out = "WHOIS " + b + " " + a; else out = "WHOIS " + a + " " + b;
        return true;
      }

      if (verb.Equals("/away", CString::CaseInsensitive)) {
        CString msg = rest;
        out = "AWAY" + (msg.empty() ? "" : CString(" ") + EnsureColon(msg));
        return true;
      }

      if (verb.Equals("/oper", CString::CaseInsensitive)) {
        CString name = s.Token(1);
        CString pass = s.Token(2);
        if (name.empty() || pass.empty()) return false;
        out = "OPER " + name + " " + pass;
        return true;
      }

      if (verb.Equals("/raw", CString::CaseInsensitive) ||
          verb.Equals("/quote", CString::CaseInsensitive)) {
        out = rest; // pass-through
        return true;
      }

      // Generic fallback: strip '/', uppercase verb, keep args
      {
        CString bareVerb = verb.substr(1);
        if (bareVerb.empty()) { out = in; return false; }
        out = U(bareVerb) + (rest.empty() ? "" : CString(" ") + rest);
        return true;
      }
    }
};

// ---- Timer fires here (after class is fully defined) ----
void CCmdTimer::RunJob() {
  auto* p = dynamic_cast<CDelayedPerformModule*>(GetModule());
  if (!p) return;
  p->FireCommand(m_sCommand, m_bSecret);
  // Fix #1: drop our pointer from the module's vector before ZNC deletes us.
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
