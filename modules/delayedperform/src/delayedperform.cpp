/*
 * delayedperform.cpp — ZNC module to run post-connect commands with delays.
 *
 * Features:
 *  - Run multiple user-configured IRC commands after connect (like perform)
 *  - Global default delay (seconds) + optional per-command delay override
 *  - Add/list/del/clear/help via /msg *delayedperform
 *  - Per-network persistence via SetNV/GetNV
 *
 * Convenience shorthands supported:
 *   /msg, /notice, /join, /part, /quit, /nick, /topic, /mode, /kick, /invite,
 *   /ctcp, /me (requires explicit target), /whois, /away, /oper, /raw, /quote,
 *   service aliases: /ns /cs /hs /ms /os /bs (NickServ etc.)
 * Unknown /verb → fallback to RAW uppercase verb (e.g. /cap ls → "CAP ls").
 *
 * Variables expanded at send time:
 *   %nick%  — current IRC nick (after reconnect/fallback). Works inside strings, e.g. "*%nick%*".
 */

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>   // for CIRCNetwork::IsIRCConnected
#include <algorithm>
#include <vector>
#include <cctype>   // isdigit
#include <climits>

using std::vector;

class CDelayedPerformModule;

// ---------------- One-shot timer ----------------
class CCmdTimer final : public CTimer {
  public:
    CCmdTimer(CModule* pMod,
              unsigned int uInterval,
              const CString& sLabel,
              const CString& sCommand)
      : CTimer(pMod, uInterval, 1, sLabel, "delayedperform one-shot"),
        m_sCommand(sCommand) {}

    void RunJob() override;

  private:
    CString m_sCommand;
};

// ---------------- Module ----------------
class CDelayedPerformModule final : public CModule {
  public:
    MODCONSTRUCTOR(CDelayedPerformModule) {
      AddHelpCommand();
      AddCommand("Help",      static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdHelp),
                 "", "Show detailed help.");
      AddCommand("SetDelay",  static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdSetDelay),
                 "<seconds>", "Set global default delay (seconds) used when 'add' omits a delay.");
      AddCommand("Add",       static_cast<CModCommand::ModCmdFunc>(&CDelayedPerformModule::CmdAdd),
                 "[seconds] <irc-or-slash-command>",
                 "Add a command. Accepts raw IRC or common '/'-style shorthands.");
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
      vector<std::pair<unsigned, CString>> entries; // (delay, "delay|b64")
      LoadAll(entries);
      if (entries.empty()) return;

      unsigned idx = 0;
      for (const auto& kv : entries) {
        const unsigned delay = kv.first;
        CString cmd;
        if (!DecodeCommand(kv.second, cmd)) continue;

        const CString label = CString("dp#") + CString(idx);
        CCmdTimer* pT = new CCmdTimer(this, delay, label, cmd);
        AddTimer(pT);
        m_vTimers.push_back(pT);
        ++idx;
      }

      PutModule(CString("Scheduled ") + CString(idx) + " delayed command(s).");
    }

    void OnIRCDisconnected() override {
      CleanupTimers();
    }

    // -------- User-visible commands --------

    void CmdHelp(const CString& /*sLine*/) {
      PutModule("delayedperform — run post-connect IRC commands with delays.");
      PutModule("Usage:");
      PutModule("  SetDelay <seconds>           — Set global default delay.");
      PutModule("  Add [seconds] <command>      — Raw IRC or shorthands like /msg, /join.");
      PutModule("  List                         — Show index, per-cmd delay, and command.");
      PutModule("  Del <index|all>              — Delete by index or everything.");
      PutModule("  Clear                        — Same as Del all.");
      PutModule("Shorthands: /msg /notice /join /part /quit /nick /topic /mode /kick /invite");
      PutModule("            /ctcp /me (/me needs a target) /whois /away /oper /raw /quote");
      PutModule("            /ns /cs /hs /ms /os /bs (service aliases).");
      PutModule("Vars: %nick% expands to your current IRC nick at send time (e.g., '*%nick%*').");
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

    void CmdAdd(const CString& sLine) {
      // Syntax:
      //  Add 5 JOIN #znc
      //  Add /msg NickServ IDENTIFY foo
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
        PutModule("Usage: Add [seconds] <irc-or-slash-command>");
        return;
      }

      // Normalize client-style '/...' to raw IRC if applicable
      CString normalized;
      if (NormalizeSlash(cmd, normalized)) {
        cmd = normalized;
      }

      CString enc = CString(cmd).Base64Encode_n();
      CString value = CString(per) + "|" + enc;

      unsigned next = NextIndex();
      SetNV(KeyFor(next), value);

      PutModule("Added [" + CString(next) + "]: delay=" + CString(per) + "s, cmd=" + cmd);
    }

    void CmdList(const CString& /*sLine*/) {
      vector<std::pair<unsigned, CString>> entries; // (delay, "delay|b64")
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
      for (const auto& kv : entries) {
        CString decoded;
        DecodeCommand(kv.second, decoded);
        PutModule(CString(idx) + "     | " + CString(kv.first) + "        | " + decoded);
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
      vector<std::pair<unsigned, CString>> entries;
      LoadAll(entries);

      if (idx >= entries.size()) {
        PutModule("Del: index out of range.");
        return;
      }

      // Rebuild without the selected entry
      vector<std::pair<unsigned, CString>> kept;
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

    // ---------- Helpers ----------

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

    static bool ParseValue(const CString& val, unsigned& delayOut, CString& b64Out) {
      int pos = val.Find("|");
      if (pos < 0) return false;
      CString sDelay = val.Left(pos);
      CString sB64   = val.substr(pos + 1);
      if (!IsUIntStr(sDelay)) return false;
      delayOut = sDelay.ToUInt();
      b64Out = sB64;
      return true;
    }

    static bool DecodeCommand(const CString& stored, CString& outCmd) {
      unsigned d;
      CString b64;
      if (!ParseValue(stored, d, b64)) return false;
      outCmd = b64.Base64Decode_n();
      return true;
    }

    void LoadAll(vector<std::pair<unsigned, CString>>& out) {
      out.clear();
      vector<std::pair<unsigned, CString>> tmp; // (index, raw NV "delay|b64")
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        const CString& k = it->first;
        if (!k.StartsWith("cmd.")) continue;
        CString sNum = k.substr(4);
        if (!IsUIntStr(sNum)) continue;
        tmp.emplace_back(sNum.ToUInt(), it->second);
      }
      std::sort(tmp.begin(), tmp.end(),
                [](const std::pair<unsigned, CString>& a, const std::pair<unsigned, CString>& b){
                  return a.first < b.first;
                });

      for (const auto& kv : tmp) {
        unsigned d; CString b64;
        if (ParseValue(kv.second, d, b64)) {
          out.emplace_back(d, kv.second);
        }
      }
    }

    void RewriteAll(const vector<std::pair<unsigned, CString>>& entries) {
      // Clear existing cmd.* keys
      vector<CString> toDel;
      for (auto it = BeginNV(); it != EndNV(); ++it) {
        if (it->first.StartsWith("cmd.")) toDel.push_back(it->first);
      }
      for (const CString& k : toDel) DelNV(k);

      // Write back contiguous indices 0..N-1 (preserving stored b64)
      unsigned idx = 0;
      for (const auto& e : entries) {
        unsigned d; CString b64;
        if (!ParseValue(e.second, d, b64)) continue;
        SetNV(KeyFor(idx), CString(d) + "|" + b64);
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

    // ---- Variable expansion + send ----
    CString ExpandVars(const CString& in) {
      CString out = in;
      if (GetNetwork()) {
        CString curr = GetNetwork()->GetIRCNick().GetNick(); // current IRC nick
        out.Replace("%nick%", curr); // works inside any surrounding text, e.g., "*%nick%*"
      }
      return out;
    }

    void FireCommand(const CString& sCmd) {
      if (!GetNetwork() || !GetNetwork()->IsIRCConnected()) {
        PutModule("Skipped (not connected): " + sCmd);
        return;
      }
      CString toSend = ExpandVars(sCmd);
      PutIRC(toSend);
      PutModule("Ran: " + toSend);
    }

  private:
    unsigned m_uGlobalDelay{0};
    vector<CTimer*> m_vTimers;

    void CleanupTimers() {
      for (CTimer* pT : m_vTimers) {
        if (pT) RemTimer(pT);
      }
      m_vTimers.clear();
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

    static CString EnsureColon(const CString& s) {
      CString t = s;
      t.TrimLeft();
      if (!t.empty() && t[0] == ':') return s; // keep original
      return CString(":") + s;
    }

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
  p->FireCommand(m_sCommand);
}

// ---- Module metadata ----
template<> void TModInfo<CDelayedPerformModule>(CModInfo& Info) {
  Info.SetWikiPage("Modules");
  Info.SetDescription("Perform commands after connect, with per-command or global delay (+slash aliases).");
}

MODULEDEFS(CDelayedPerformModule, "Perform commands after connect, with per-command or global delay (+slash aliases).")

