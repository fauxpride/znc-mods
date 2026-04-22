/*
 * keepchanbuffersize.cpp - Preserve per-channel BufferSize across PART/JOIN
 *
 * Version: 1.2.0
 * Target:  ZNC 1.9.1 (network module)
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
 *   Version
 *   List
 *   Set    <#chan> <lines>
 *   Forget <#chan>
 *
 * Note: The Enable/Disable toggle was removed in 1.2.0. The module is always
 * active when loaded; to stop it from acting, UnloadMod it. Previously stored
 * buffer values and the `enabled=0|1` load argument are honored for backward
 * compatibility but the argument now prints a deprecation notice.
 */

#include <znc/Modules.h>
#include <znc/IRCNetwork.h>
#include <znc/Chan.h>
#include <znc/Message.h>
#include <znc/Utils.h>
#include <znc/znc.h>

#include <utility>
#include <vector>

#define KEEPCHANBUFFERSIZE_VERSION "1.2.0"

class CKeepChanBufferSize : public CModule {
public:
    MODCONSTRUCTOR(CKeepChanBufferSize) {
        AddHelpCommand();

        AddCommand("Status", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdStatus),
                   "", "Show how many channels have a remembered buffer size.");
        AddCommand("Version", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdVersion),
                   "", "Show the module version.");

        AddCommand("List", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdList),
                   "", "List remembered channels and their buffer sizes.");

        AddCommand("Set", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdSet),
                   "<#chan> <lines>", "Manually remember a buffer size (and apply now if the channel exists).");

        AddCommand("Forget", static_cast<CModCommand::ModCmdFunc>(&CKeepChanBufferSize::CmdForget),
                   "<#chan>", "Forget a remembered buffer size for a channel.");
    }

    bool OnLoad(const CString& sArgs, CString& sErrorMsg) override {
        // 1.2.0 migration: the Enable/Disable toggle was removed, so the
        // "Enabled" NV key from 1.0.x - 1.1.x is dead state. Clean it up
        // silently on load; this is idempotent (DelNV is a no-op on a missing
        // key). Per-channel buf:* entries are untouched.
        if (!GetNV("Enabled").empty()) {
            DelNV("Enabled");
        }

        // Parse load arguments. The only argument this module ever accepted
        // was enabled=0|1 (with off/on/false/true aliases). It no longer has
        // any effect, but we recognize it specifically to print a deprecation
        // notice rather than silently ignoring it and letting the user believe
        // their intent was honored.
        if (!sArgs.empty()) {
            VCString vs;
            sArgs.Split(" ", vs, false);
            for (const CString& tok : vs) {
                CString k = tok.Token(0, false, "=").AsLower();
                if (k == "enabled") {
                    PutModule("Note: the 'enabled=' load argument is deprecated in 1.2.0 "
                              "and has no effect. The module is always active while loaded; "
                              "use UnloadMod to stop it. Stored per-channel values are preserved.");
                }
            }
        }

        // One-time migration of legacy ASCII-lowercased keys to rfc1459-lowercased keys.
        // This is a no-op for channels that are purely ASCII (the overwhelming common case).
        MigrateLegacyKeys();

        // Apply remembered values to any channels that already exist. Useful
        // after module reloads or reconnect-related situations where channels
        // already exist at module load time.
        ApplyAllExistingChannels();

        return true;
    }

    // -------------------------------------------------------------------------
    // Client -> ZNC hooks
    // -------------------------------------------------------------------------

    // Raw hook: great for ultra-fast /cycle (captures PART before core can drop the channel object).
    EModRet OnUserRawMessage(CMessage& Message) override {
        if (!GetNetwork())
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
        if (!GetNetwork())
            return CONTINUE;

        RememberTargets(Message.GetTarget());
        return CONTINUE;
    }

    // Structured JOIN hook: early restore attempt (server JOIN still restores reliably).
    EModRet OnUserJoinMessage(CJoinMessage& Message) override {
        if (!GetNetwork())
            return CONTINUE;

        HandleUserJoinTargets(Message.GetTarget());
        return CONTINUE;
    }

    // -------------------------------------------------------------------------
    // Server -> ZNC hooks
    // -------------------------------------------------------------------------

    // Restore when *we* join (channel exists for sure).
    void OnJoinMessage(CJoinMessage& Message) override {
        if (!GetNetwork())
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
        if (!GetNetwork())
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

    // Remember on KICK fallback (if we're kicked and later rejoin).
    void OnKickMessage(CKickMessage& Message) override {
        if (!GetNetwork())
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
    // RFC 1459 lowercasing for IRC identifier comparison.
    // Treats '[', ']', '\\', '~' as the uppercase of '{', '}', '|', '^'.
    // This is the historical IRC default casemap; we hardcode it rather than
    // consulting the server's CASEMAPPING value because NV storage keys must be
    // stable across sessions (CASEMAPPING is not known until ISUPPORT arrives,
    // and can differ per network). For ASCII-only identifiers this produces the
    // exact same result as plain AsLower(), so it is fully backward compatible
    // with the most common case.
    static CString IRCLower(const CString& s) {
        CString r;
        r.reserve(s.size());
        for (char c : s) {
            if (c >= 'A' && c <= 'Z')        r += static_cast<char>(c + 32);
            else if (c == '[')               r += '{';
            else if (c == ']')               r += '}';
            else if (c == '\\')              r += '|';
            else if (c == '~')               r += '^';
            else                             r += c;
        }
        return r;
    }

    // Normalize channel for stable NV keys (rfc1459 casemap).
    static CString NormChan(const CString& sChan) {
        return IRCLower(sChan);
    }

    static CString KeyForChan(const CString& sChan) {
        return "buf:" + NormChan(sChan);
    }

    // Validate that a string looks like an IRC channel name. When connected,
    // we use the server-advertised CHANTYPES via CIRCNetwork::IsChan(); before
    // ISUPPORT arrives (or with no network), we fall back to the RFC defaults.
    bool IsValidChannelName(const CString& sChan) const {
        if (sChan.empty())
            return false;

        const CIRCNetwork* pNet = GetNetwork();
        if (pNet && !pNet->GetChanPrefixes().empty())
            return pNet->IsChan(sChan);

        const char c = sChan[0];
        return c == '#' || c == '&' || c == '+' || c == '!';
    }

    bool IsMe(const CNick& Nick) const {
        if (!GetNetwork())
            return false;
        return Nick.NickEquals(GetNetwork()->GetCurNick());
    }

    // CString overload (used by OnKickMessage via CKickMessage::GetKickedNick()).
    // Delegate to the CNick overload so NickEquals()'s IRC-casemap comparison is
    // what actually runs. A plain CString::Equals() would do ASCII-only
    // case-insensitive compare and silently mis-identify self on networks
    // where your nick contains casemap-corner characters
    // ([ ] \ ~ vs { } | ^).
    bool IsMe(const CString& sNick) const {
        if (!GetNetwork())
            return false;
        return IsMe(CNick(sNick));
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

        // Pass bForce=true: CChan::SetBufferCount() -> CBuffer::SetLineCount()
        // enforces CZNC::GetMaxBufferSize() only when bForce is false, and
        // silently returns false otherwise. Webadmin and the config loader set
        // per-channel buffers with force=true, so a restored value only needs
        // to honor that same ceiling. Without force=true, any channel whose
        // explicit buffer exceeds MaxBufferSize (which is legal via webadmin)
        // cannot be restored after teardown. We already validate against
        // MaxBufferSize at Set time, and any value we're restoring here was
        // previously accepted by ZNC, so forcing is safe.
        if (!Chan.SetBufferCount(uWanted, true)) {
            PutModule("Warning: failed to restore buffer size for " + Chan.GetName() +
                      " to " + sVal + ".");
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

    // One-time migration: rewrite any buf:* key whose stored form differs from
    // the rfc1459-lowered form. Intended to run once on load; re-running is a
    // no-op. We collect the key list first so we do not mutate NV while iterating.
    void MigrateLegacyKeys() {
        std::vector<std::pair<CString, CString>> vPairs;
        for (auto it = BeginNV(); it != EndNV(); ++it) {
            if (it->first.StartsWith("buf:"))
                vPairs.emplace_back(it->first, it->second);
        }

        for (const auto& kv : vPairs) {
            const CString& sOldKey = kv.first;
            const CString& sVal    = kv.second;
            const CString sChan    = sOldKey.substr(4); // after "buf:"
            const CString sNewKey  = "buf:" + IRCLower(sChan);

            if (sOldKey == sNewKey)
                continue;

            // If a new-form key already holds a value, prefer the existing one
            // rather than clobbering it, and drop the legacy duplicate.
            if (!GetNV(sNewKey).empty()) {
                DelNV(sOldKey);
                continue;
            }

            SetNV(sNewKey, sVal);
            DelNV(sOldKey);
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
        PutModule("Remembered channels: " + CString(CountRemembered()));
    }

    void CmdVersion(const CString&) {
        PutModule("keepchanbuffersize version " KEEPCHANBUFFERSIZE_VERSION);
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

        if (!IsValidChannelName(sChanArg)) {
            PutModule("Invalid channel name: " + sChanArg +
                      " (must begin with a channel prefix such as '#').");
            return;
        }

        // Reject non-digit input (so '-1' can't wrap into UINT_MAX via ToUInt()).
        if (sBufArg.find_first_not_of("0123456789") != CString::npos) {
            PutModule("Invalid buffer size: " + sBufArg +
                      " (must be a positive integer, digits only).");
            return;
        }

        const unsigned int uWanted = sBufArg.ToUInt();
        if (uWanted == 0) {
            PutModule("Invalid buffer size (must be greater than zero).");
            return;
        }

        // Enforce ZNC's global MaxBufferSize as the upper bound before
        // persisting, so we can't stash a value we know will be rejected on
        // every subsequent JOIN. A value of 0 for MaxBufferSize means
        // "unlimited" in the ZNC config grammar, so only enforce when it is
        // non-zero.
        const unsigned int uMax = CZNC::Get().GetMaxBufferSize();
        if (uMax > 0 && uWanted > uMax) {
            PutModule("Invalid buffer size: " + CString(uWanted) +
                      " exceeds ZNC's MaxBufferSize of " + CString(uMax) + ".");
            return;
        }

        SetNV(KeyForChan(sChanArg), CString(uWanted));
        PutModule("Remembered: " + NormChan(sChanArg) + " => " + CString(uWanted));

        if (GetNetwork()) {
            CChan* pChan = GetNetwork()->FindChan(sChanArg);
            if (pChan) {
                // Force: we have already validated uWanted against
                // MaxBufferSize above, and webadmin/config use the same
                // force=true path to set per-channel buffers.
                if (!pChan->SetBufferCount(uWanted, true)) {
                    PutModule("Note: stored, but failed to apply immediately.");
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

        if (!IsValidChannelName(sChanArg)) {
            PutModule("Invalid channel name: " + sChanArg +
                      " (must begin with a channel prefix such as '#').");
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
                  "Preserve per-channel BufferSize across quick PART/JOIN (incl. /cycle). "
                  "v" KEEPCHANBUFFERSIZE_VERSION);
