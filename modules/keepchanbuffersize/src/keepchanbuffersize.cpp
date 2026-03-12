/*
 * keepchanbuffersize.cpp - Preserve per-channel BufferSize across PART/JOIN
 *
 * Target: ZNC 1.9.1 (network module)
 *
 * Design goals:
 *  - Preserve per-channel buffer (SetBuffer/BufferSize) across fast /cycle.
 *  - Keep behavior robust across clients/scripts (mIRC + Peace&Protection, Textual, etc.).
 *  - Future-friendly: use command strings and multiple hooks (raw + structured).
 *
 * How it works:
 *  - Remember on client PART (OnUserRawMessage + OnUserPartMessage) BEFORE ZNC can drop chan object.
 *  - Also remember on server PART/KICK as fallback.
 *  - Restore on server JOIN (OnJoinMessage) when channel definitely exists.
 *  - Opportunistic early restore on user JOIN if channel already exists.
 *  - Handles JOIN 0 (leave all channels) by snapshotting all channels.
 *
 * Commands (message *keepchanbuffersize):
 *   Status
 *   Enable
 *   Disable
 *   List
 *   Set    <#chan> <lines>
 *   Forget <#chan>
 */

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>
#include <znc/Chan.h>
#include <znc/Message.h>
#include <znc/Utils.h>

class CKeepChanBufferSize : public CModule {
public:
    MODCONSTRUCTOR(CKeepChanBufferSize) {
        AddHelpCommand();

        AddCommand("Status", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdStatus),
                   "", "Show whether the module is enabled and how many channels are remembered.");
        AddCommand("Enable", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdEnable),
                   "", "Enable automatic remember/restore behavior.");
        AddCommand("Disable", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdDisable),
                   "", "Disable automatic remember/restore behavior (data is kept).");

        AddCommand("List", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdList),
                   "", "List remembered channels and their buffer sizes.");

        AddCommand("Set", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdSet),
                   "<#chan> <lines>", "Manually remember a buffer size (and apply now if the channel exists).");

        AddCommand("Forget", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdForget),
                   "<#chan>", "Forget a remembered buffer size for a channel.");
    }

    bool OnLoad(const CString& sArgs, CString& sErrorMsg) override {
        // Default enabled unless explicitly disabled in NV.
        CString sEnabled = GetNV("Enabled");
        if (sEnabled.empty()) {
            m_bEnabled = true;
            SetNV("Enabled", "1");
        } else {
            m_bEnabled = !sEnabled.Equals("0");
        }

        // Optional: LoadMod keepchanbuffersize enabled=0|1
        if (!sArgs.empty()) {
            VCString vs;
            sArgs.Split(" ", vs, false);
            for (const CString& tok : vs) {
                CString k = tok.Token(0, false, "=").AsLower();
                CString v = tok.Token(1, true, "=").AsLower();

                if (k == "enabled") {
                    if (v == "0" || v == "off" || v == "false") {
                        m_bEnabled = false;
                        SetNV("Enabled", "0");
                    } else if (v == "1" || v == "on" || v == "true") {
                        m_bEnabled = true;
                        SetNV("Enabled", "1");
                    }
                }
            }
        }

        // If enabled, apply remembered values to any channels that already exist.
        if (m_bEnabled) {
            ApplyAllExistingChannels();
        }

        return true;
    }

    // -------------------------------------------------------------------------
    // Client -> ZNC hooks
    // -------------------------------------------------------------------------

    // Raw hook: great for ultra-fast /cycle (captures PART before core can drop the channel object).
    EModRet OnUserRawMessage(CMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return CONTINUE;

        const CString& cmd = Message.GetCommand();

        if (cmd.Equals("PART")) {
            // PART can target multiple chans: PART #a,#b :reason
            RememberTargets(Message.GetParam(0));
        } else if (cmd.Equals("JOIN")) {
            // JOIN can be "JOIN 0" or "JOIN #a,#b key1,key2"
            HandleUserJoinTargets(Message.GetParam(0));
        }

        return CONTINUE;
    }

    // Structured PART hook: extra safety (keeps raw hook too).
    EModRet OnUserPartMessage(CPartMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return CONTINUE;

        RememberTargets(Message.GetTarget());
        return CONTINUE;
    }

    // Structured JOIN hook: early restore attempt (server JOIN still restores reliably).
    EModRet OnUserJoinMessage(CJoinMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return CONTINUE;

        HandleUserJoinTargets(Message.GetTarget());
        return CONTINUE;
    }

    // -------------------------------------------------------------------------
    // Server -> ZNC hooks
    // -------------------------------------------------------------------------

    // Restore when *we* join (channel exists for sure).
    void OnJoinMessage(CJoinMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return;

        if (!IsMe(Message.GetNick()))
            return;

        CChan* pChan = Message.GetChan();
        if (!pChan) {
            CString sTarget = NormalizeTarget(Message.GetTarget());
            if (!sTarget.empty())
                pChan = GetNetwork()->FindChan(sTarget);
        }
        if (!pChan)
            return;

        ApplyRememberedToChan(*pChan);
    }

    // Remember on server PART fallback (covers non-client-initiated flows).
    void OnPartMessage(CPartMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return;

        if (!IsMe(Message.GetNick()))
            return;

        CChan* pChan = Message.GetChan();
        if (!pChan) {
            CString sTarget = NormalizeTarget(Message.GetTarget());
            if (!sTarget.empty())
                pChan = GetNetwork()->FindChan(sTarget);
        }
        if (!pChan)
            return;

        RememberFromChan(*pChan);
    }

    // Remember on KICK fallback (if we’re kicked and later rejoin).
    void OnKickMessage(CKickMessage& Message) override {
        if (!m_bEnabled || !GetNetwork())
            return;

        if (!IsMe(Message.GetKickedNick()))
            return;

        CChan* pChan = Message.GetChan();
        if (!pChan) {
            CString sTarget = NormalizeTarget(Message.GetTarget());
            if (!sTarget.empty())
                pChan = GetNetwork()->FindChan(sTarget);
        }
        if (!pChan)
            return;

        RememberFromChan(*pChan);
    }

private:
    bool m_bEnabled = true;

    // Normalize channel for stable NV keys.
    static CString NormChan(const CString& sChan) {
        return sChan.AsLower();
    }

    static CString KeyForChan(const CString& sChan) {
        return "buf:" + NormChan(sChan);
    }

    bool IsMe(const CNick& Nick) const {
        if (!GetNetwork())
            return false;
        return Nick.NickEquals(GetNetwork()->GetCurNick());
    }

    bool IsMe(const CString& sNick) const {
        if (!GetNetwork())
            return false;
        return sNick.Equals(GetNetwork()->GetCurNick());
    }

    static CString NormalizeTarget(CString s) {
        s = s.Trim_n();
        if (s.StartsWith(":"))
            s = s.substr(1);
        return s.Trim_n();
    }

    void RememberFromChan(const CChan& Chan) {
        const CString sKey = KeyForChan(Chan.GetName());

        // Only persist if explicitly set for the channel; otherwise do not store (follow defaults).
        if (Chan.HasBufferCountSet()) {
            const unsigned int uBuf = Chan.GetBufferCount();
            SetNV(sKey, CString(uBuf));
        } else {
            DelNV(sKey);
        }
    }

    void RememberTargets(const CString& sTargetsRaw) {
        if (!GetNetwork())
            return;

        CString sTargets = NormalizeTarget(sTargetsRaw);
        if (sTargets.empty())
            return;

        VCString vs;
        sTargets.Split(",", vs, false);

        for (CString sOne : vs) {
            sOne = NormalizeTarget(sOne);
            if (sOne.empty())
                continue;

            CChan* pChan = GetNetwork()->FindChan(sOne);
            if (pChan)
                RememberFromChan(*pChan);
        }
    }

    void HandleUserJoinTargets(const CString& sTargetsRaw) {
        if (!GetNetwork())
            return;

        CString sTargets = NormalizeTarget(sTargetsRaw);
        if (sTargets.empty())
            return;

        // JOIN 0 => leave all channels (snapshot everything before they may be torn down).
        if (sTargets == "0") {
            const std::vector<CChan*>& vChans = GetNetwork()->GetChans();
            for (CChan* pChan : vChans) {
                if (pChan)
                    RememberFromChan(*pChan);
            }
            return;
        }

        // JOIN may contain multiple chans (#a,#b). We do an early restore attempt:
        // if a CChan exists already, apply now; otherwise server OnJoinMessage will apply later.
        VCString vs;
        sTargets.Split(",", vs, false);

        for (CString sOne : vs) {
            sOne = NormalizeTarget(sOne);
            if (sOne.empty())
                continue;

            CChan* pChan = GetNetwork()->FindChan(sOne);
            if (pChan)
                ApplyRememberedToChan(*pChan);
        }
    }

    void ApplyRememberedToChan(CChan& Chan) {
        const CString sKey = KeyForChan(Chan.GetName());
        const CString sVal = GetNV(sKey);
        if (sVal.empty())
            return;

        const unsigned int uWanted = sVal.ToUInt();
        if (uWanted == 0)
            return;

        if (Chan.HasBufferCountSet() && Chan.GetBufferCount() == uWanted)
            return;

        if (!Chan.SetBufferCount(uWanted, false)) {
            PutModule("Warning: failed to restore buffer size for " + Chan.GetName() +
                      " to " + sVal + " (may exceed limits).");
        }
    }

    void ApplyAllExistingChannels() {
        if (!GetNetwork())
            return;

        const std::vector<CChan*>& vChans = GetNetwork()->GetChans();
        for (CChan* pChan : vChans) {
            if (pChan)
                ApplyRememberedToChan(*pChan);
        }
    }

    // NOTE: Must be non-const for ZNC 1.9.1 because BeginNV()/EndNV() aren't const-qualified.
    unsigned int CountRemembered() {
        unsigned int n = 0;
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (it->first.StartsWith("buf:") && !it->second.empty())
                ++n;
        }
        return n;
    }

    // -------------------------------------------------------------------------
    // Commands
    // -------------------------------------------------------------------------

    void CmdStatus(const CString&) {
        PutModule("keepchanbuffersize: " + CString(m_bEnabled ? "ENABLED" : "DISABLED"));
        PutModule("Remembered channels: " + CString(CountRemembered()));
    }

    void CmdEnable(const CString&) {
        m_bEnabled = true;
        SetNV("Enabled", "1");
        PutModule("Enabled. (Stored values kept; will restore on JOIN.)");
        ApplyAllExistingChannels();
    }

    void CmdDisable(const CString&) {
        m_bEnabled = false;
        SetNV("Enabled", "0");
        PutModule("Disabled. (Stored values kept; no automatic remember/restore.)");
    }

    void CmdList(const CString&) {
        CTable Table;
        Table.AddColumn("Channel");
        Table.AddColumn("Buffer");

        unsigned int n = 0;
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (!it->first.StartsWith("buf:"))
                continue;
            if (it->second.empty())
                continue;

            CString sChan = it->first.substr(4); // after "buf:"
            Table.AddRow();
            Table.SetCell("Channel", sChan);
            Table.SetCell("Buffer", it->second);
            ++n;
        }

        if (n == 0) {
            PutModule("No remembered channels.");
            return;
        }

        PutModule(Table);
    }

    void CmdSet(const CString& sLine) {
        const CString sChanArg = sLine.Token(1);
        const CString sBufArg  = sLine.Token(2);

        if (sChanArg.empty() || sBufArg.empty()) {
            PutModule("Usage: Set <#chan> <lines>");
            return;
        }

        const unsigned int uWanted = sBufArg.ToUInt();
        if (uWanted == 0) {
            PutModule("Invalid buffer size (must be a positive integer).");
            return;
        }

        SetNV(KeyForChan(sChanArg), CString(uWanted));
        PutModule("Remembered: " + NormChan(sChanArg) + " => " + CString(uWanted));

        if (GetNetwork()) {
            CChan* pChan = GetNetwork()->FindChan(sChanArg);
            if (pChan) {
                if (!pChan->SetBufferCount(uWanted, false)) {
                    PutModule("Note: stored, but failed to apply immediately (may exceed limits).");
                } else {
                    PutModule("Applied immediately to existing channel object.");
                }
            }
        }
    }

    void CmdForget(const CString& sLine) {
        const CString sChanArg = sLine.Token(1);
        if (sChanArg.empty()) {
            PutModule("Usage: Forget <#chan>");
            return;
        }

        const CString sKey = KeyForChan(sChanArg);
        if (GetNV(sKey).empty()) {
            PutModule("Nothing stored for " + NormChan(sChanArg));
            return;
        }

        DelNV(sKey);
        PutModule("Forgot stored buffer size for " + NormChan(sChanArg));
    }
};

// Network module (NOT global)
NETWORKMODULEDEFS(CKeepChanBufferSize,
                  "Preserve per-channel BufferSize across quick PART/JOIN (incl. /cycle).");
