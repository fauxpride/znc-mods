# Changelog

All notable changes to the `delayedperform` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.1] — 2026-04-20

This release properly addresses the `%nick%`-expansion hardening that was misidentified in 1.1.0, and reverts a behavior (delay caps) that had been introduced without being requested. Built and runtime-tested against ZNC 1.9.1.

### Added
- **Strict IRC nick-grammar validation before `%nick%` substitution.** New `IsValidIRCNick()` helper permits only the characters defined by RFC 2812 nick grammar — letters, digits, and the specials ``[ ] \ ` _ ^ { | }`` — plus hyphen (widely accepted in practice). Anything else (space, `:`, `,`, `@`, `!`, `.`, CR, LF, NUL, tab, `#`, non-ASCII, etc.) causes the substitution to fail.
- When `%nick%` substitution fails because the current nick is malformed, the module logs `Skipped (invalid nick for expansion): <command>` (or `[hidden]` for secret entries) and does not call `PutIRC()`. This sits upstream of the CR/LF/NUL filter from 1.1.0 and catches additional characters (like space and `:`) that could split or reframe an IRC line without tripping the CR/LF/NUL filter alone.

### Changed
- **`ExpandVars` signature is now `bool ExpandVars(const CString& in, CString& out)`.** It returns `false` when substitution is unsafe (e.g. a non-conforming nick) and leaves `out` untouched; callers must not transmit `out` in that case. `FireCommand`'s single call site was updated accordingly. This is an internal API change; the module's user-visible commands and storage format are unchanged.
- Non-`%nick%` commands no longer fetch or validate the current nick: if the stored command does not contain `%nick%`, `ExpandVars` short-circuits early. This means nick validation can never block a command that didn't ask for the substitution in the first place.

### Removed
- **Reverted the delay cap added in 1.1.0.** `kMaxDelaySecs`, the corresponding checks in `SetDelay` and `DoAdd`, the clamping in `LoadGlobalDelay` and `LoadAll`, and the "(max 3600s)" mention in `Help` are all gone. The module once again accepts arbitrary non-negative delays, matching 1.0.0 behavior. If you want a sanity cap, enforce it client-side before calling the module.
- **Confirmed absence of the unused `<climits>` include.** The header was noted as unused in the 1.0.0 audit and had already been dropped during the 1.1.0 rewrite; this release explicitly confirms it stays absent and records the removal here for the avoidance of doubt.

### Fixed
- **Bug #4 ("`%nick%` expansion isn't constrained") now properly addressed.** The CR/LF/NUL check added for bug #3 in 1.1.0 handled the narrow scenario of a malformed nick containing only CR/LF, but it did not cover nicks containing space, `:`, `,`, `@`, or other characters that would still split or reframe the outgoing line. The new grammar check closes that gap.

### Security
- Malformed `%nick%` substitution values can no longer flow into `PutIRC()`. Combined with the 1.1.0 CR/LF/NUL filtering, the module now has two independent layers of protection against stored or expanded commands being used to inject additional IRC lines, and the nick layer is specifically scoped to the %nick% code path so it is never bypassed by absence of CR/LF in the malformed value.

### Compatibility
- **Storage format is unchanged.** Entries written by 1.1.0 (`<delay>|<flags>|<base64>`) continue to load, and the 1.0.0 legacy format (`<delay>|<base64>`) is still read as non-secret entries — exactly as in 1.1.0.
- **ZNC compatibility.** Built and tested against ZNC 1.9.1 (CVE-2024-39844 security release). No ZNC API changes were required between 1.9.0 and 1.9.1 for this module; 1.9.0 continues to work as well.
- **Runtime behavior for existing users.** Users who had stored delays greater than 3600 seconds in 1.0.0 but had the value silently clamped by 1.1.0 will see the original value honored again in 1.1.1.

---

## [1.1.0] — 2026-04-20

### Added
- `Version` command: replies with `delayedperform version <x.y.z>` for quick identification of the running build.
- `AddSecret` command: same semantics as `Add`, but the command's text is never echoed in the module's `Added`, `List`, `Ran:`, or `Skipped` output. Intended for entries containing credentials (e.g. `/ns IDENTIFY`, `/oper`).
- Per-entry flags in the NV storage format (`<delay>|<flags>|<base64>`), currently supporting the `s` (secret) flag. The parser still accepts the legacy `<delay>|<base64>` format, so existing configurations are preserved without migration.
- Module version string is now embedded in `Help` output, in `TModInfo`'s description, and in `MODULEDEFS`. Visible from `/msg *status ListMods` as well.
- Upper bound of 3600 seconds on any configurable delay (`SetDelay`, per-command delays on `Add`/`AddSecret`). Values over the cap are rejected at input time, and stored entries are clamped on load. *(Reverted in 1.1.1.)*
- Defense-in-depth check that rejects command text containing CR, LF, or NUL — both at `Add`/`AddSecret` time (before persisting) and just before the timer sends the line to IRC.

### Changed
- `CmdList` now surfaces corrupt entries as `[corrupt entry]` instead of printing an empty row, making broken NV values visible to the user.
- `OnIRCConnected` now reports the number of *successfully scheduled* commands (previously the loop index was reported regardless of scheduling success).
- `CleanupTimers` now snapshots the timer vector before iterating, so reentrant list modifications cannot observe stale state.

### Fixed
- **Dangling pointers in the timer-tracking vector.** One-shot timers are destroyed by ZNC after they fire, but the module retained the raw pointers until the next `CleanupTimers`, leading to undefined behavior when those pointers were later passed back to `RemTimer`. `CCmdTimer::RunJob` now calls `ForgetTimer(this)` right after firing, so the module never holds a pointer to a freed timer.
- **`AddTimer` return value was ignored.** If the call failed, the new `CCmdTimer` was leaked and an unowned pointer was recorded in the module's timer vector. The return is now checked; on failure the pointer is deleted, a warning is printed, and nothing is added to the tracking vector.
- **`DecodeCommand` return was ignored in `CmdList`.** Corrupt entries previously rendered as silent blanks; they now render as `[corrupt entry]`.

### Security
- **Credential exposure through the module window.** Commands stored with `AddSecret` no longer appear in clear text in any `PutModule()` output (`Added`, `List`, `Ran:`, `Skipped`). Because `PutModule()` broadcasts to every IRC client currently attached to the ZNC user, and because most IRC clients log module-window traffic by default, this closes the most common way credentials in a delayed-perform queue end up on disk or on a secondary device.
- **CR/LF/NUL injection hardening.** Stored and expanded commands are now validated against embedded control characters before they are handed to `PutIRC()`. This is primarily defense in depth — the authenticated ZNC user has always been able to send arbitrary IRC via other channels — but it removes a plausible injection primitive if a future code path ever lets another source influence stored values.

### Compatibility
- Existing `cmd.<index>` NV entries written by 1.0.0 (`<delay>|<base64>` format) continue to load and behave as non-secret entries.
- When the module rewrites storage (e.g. on `Del` or `Clear`+re-`Add`), surviving entries are re-serialised in the new 1.1 format.
- No ZNC API dependencies changed. The module continues to build with `znc-buildmod` against current ZNC releases.

---

## [1.0.0] — Initial release

### Added
- Run multiple user-configured IRC commands automatically after connect.
- Global default delay (in seconds) with optional per-command delay override.
- `Add`, `List`, `Del`, `Clear`, `SetDelay`, `Help` commands via `/msg *delayedperform`.
- Per-network persistence of the command list and global delay via ZNC NV storage.
- Slash-style command shorthands: `/msg`, `/notice`, `/join`, `/part`, `/quit`, `/nick`, `/topic`, `/mode`, `/kick`, `/invite`, `/ctcp`, `/me`, `/whois`, `/away`, `/oper`, `/raw`, `/quote`, and service aliases `/ns /cs /hs /ms /os /bs`. Unknown slash verbs fall back to a generic `VERB args` conversion.
- Send-time expansion of the `%nick%` variable using the current IRC nick (after reconnects or fallback-nick scenarios).
- Cleanup of scheduled timers on IRC disconnect, so reconnects start from a fresh schedule.
