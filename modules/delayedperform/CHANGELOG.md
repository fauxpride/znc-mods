# Changelog

All notable changes to the `delayedperform` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.2.0] — 2026-11-01

This release fixes a latent double free in timer scheduling and tightens input handling: numbers no longer wrap, incomplete shorthands and malformed delays are rejected instead of stored, unreadable stored entries are shown and can be deleted, and module output can no longer carry raw CR, LF or NUL. Built with `-Wall -Wextra -Werror` against ZNC 1.10.3 with GCC 13.3 and GCC 11.5, and runtime-tested against ZNC 1.10.3.

### Fixed
- **Double free when a timer could not be scheduled.** When `CModule::AddTimer()` refuses a timer it deletes it itself, but `OnIRCConnected()` then deleted it a second time, a use-after-free that crashes ZNC. The branch was only reachable if a timer with the same `dp#<index>` label still existed when the next connection came up. The module no longer deletes a refused timer, and timer labels now include a per-connection counter (`dp#<connection>.<index>`), so a label can no longer collide with one from an earlier connection.
- **Numbers above 4294967295 wrapped.** `CString::ToUInt()` truncates to 32 bits, so `Del 4294967296` deleted entry 0, `Add 4294967296 <cmd>` stored a delay of 0, and `SetDelay 4294967297` set 1. The same wrap applied to the registry: a key `cmd.4294967296` was read as entry 0. All numbers are now range-checked. Out-of-range input is rejected with a message, and out-of-range stored keys and delays are not misread as small numbers.
- **A signed or decimal delay became part of the command.** `Add -3 JOIN #x` stored the command `-3 JOIN #x` with the global delay. A first argument with a sign or a decimal point (`-3`, `+5`, `1.5`, `.5`) is now rejected: "Add: the delay must be a whole number of seconds from 0 to 4294967295."
- **Incomplete shorthands were stored as typed.** A supported shorthand missing a required argument, such as `/msg bob`, `/me waves` or `/kick #c`, was stored with its slash and later sent to the server, which rejected it. `/raw` and `/quote` with nothing after them stored an empty entry, which then showed as `[corrupt entry]`. These are now rejected with the shorthand's usage line, for example "Add: incomplete shorthand. Usage: /msg <target> <text>", and nothing is stored.
- **Control characters reached the client in module output.** `Skipped (contains control chars): ...` printed the stored line itself, so a lone CR reached the client inside a module message, and `List` printed stored CR, LF and NUL as they were. Both now print them as `\r`, `\n` and `\0`. The skip message shows the stored command rather than the expanded one.
- **Unreadable entries were hidden and could not be deleted.** An entry whose delay was not a number did not appear in `List` and could only be removed with `Clear`; the next `Del` silently discarded it, together with any other `cmd.*` key that was not numbered. Invalid base64 decoded to arbitrary bytes and was listed and sent as such.
- **`Add` reported the stored key instead of the `List` index.** With gaps in the stored keys, the index in `Added [N]` did not match the index `List` showed or `Del` accepted.

### Changed
- **`Del` removes only the deleted entry.** It no longer rewrites the whole list into contiguous keys. Other entries keep their keys, values and flags exactly as stored; `List` indices close up because `List` numbers entries by position, as before.
- **Unreadable entries are listed.** An entry whose delay is not a valid number, or whose payload is empty or not base64, is listed as `[corrupt entry]` (with `-` in the delay column when the delay is the problem), is never scheduled, and can be removed with `Del`. Unpadded base64 is accepted, as ZNC's decoder accepts it.
- **Connect message.** When the store holds unreadable entries, `Scheduled N delayed command(s).` is followed by `Skipped M unreadable entries; see List.`
- **`SetDelay` above 4294967295** answers "SetDelay: seconds must be between 0 and 4294967295." instead of storing a wrapped value. Non-numeric input still shows the usage line.
- **`Add` refuses when no key is free.** If the highest stored key is already `cmd.4294967295`, `Add` says so instead of wrapping to `cmd.0`.
- **README.** Documents the above, explains why loading the module on a live connection deliberately schedules nothing (otherwise every `/znc updatemod` would re-send the perform), and corrects a storage example whose base64 was invalid.

### Compatibility
- **Storage format is unchanged.** Entries in the 1.1 format and the 1.0 legacy format load as before, and 1.1.3 reads a store written by 1.2.0, including the gaps `Del` now leaves, so downgrading is safe.
- **Input that 1.1.3 accepted is now rejected.** A script that adds entries with a signed or decimal delay, an out-of-range number, or an incomplete shorthand gets an error and nothing is stored. Entries stored that way by earlier versions are kept and still sent as before; delete and re-add them.
- **Output changes.** Anything that parses the connect message or `Skipped (contains control chars)` should expect the additions above.
- **ZNC compatibility.** Built and runtime-tested against ZNC 1.10.3. No ZNC API changes were required.

---

## [1.1.3] — 2026-09-11

This release makes the module's own `Help` output reachable and fixes the argument order of two-argument `/whois` shorthands. Built with `-Wall -Wextra` without module warnings and runtime-tested against ZNC 1.9.1; also compiles with `znc-buildmod` against ZNC 1.9.0.

### Changed
- **`Help` output.** Without an argument, `Help` now shows the module's usage help: the version, a summary of each command, the supported slash shorthands, and the `%nick%` variable. With an argument, such as `Help add`, it shows ZNC's standard filtered command table, exactly as before. The `Help` command is now registered with a `[filter]` argument and the description "Show detailed help, or list commands matching a filter.", and the usage help includes a matching `Help [filter]` line.

### Fixed
- **The module's help text was unreachable.** The constructor called `AddHelpCommand()`, which registers ZNC's generic `Help` command, before registering the module's own `Help`. Because command names are matched case-insensitively, the second `AddCommand("Help", ...)` failed silently and `CmdHelp()` never ran; 1.1.1 and 1.1.2 showed only ZNC's generated command table. The `AddHelpCommand()` call is removed, and `CmdHelp()` passes filtered requests to `HandleHelpCommand()`.
- **Two-argument `/whois` swapped its arguments.** The test for a server-like second argument, `(b.Find(".") >= 0) || (b.Find(":") >= 0)`, compared unsigned (`size_t`) results with zero and was always true, so every two-argument `/whois a b` was stored as `WHOIS b a`. For example, `/whois irc.server.net nick` became `WHOIS nick irc.server.net`, and `/whois nick1 nick2` became `WHOIS nick2 nick1`. The test now compares with `CString::npos`.
- **Version-check documentation.** Because `Help` shows the version again, the README lists it alongside `Version` and `ListAvailMods`. The 1.1.0 entry below states that the version is visible from `ListMods`; in ZNC 1.9.x, `ListMods` shows only module names and arguments, and the version appears in the module description shown by `ListAvailMods`.

### Compatibility
- **Existing `/whois` entries are not corrected automatically.** Slash shorthands are converted to raw IRC when an entry is added, and only the raw line is stored. Entries added in 1.1.2 or earlier with a two-argument `/whois` whose second argument contains neither `.` nor `:` keep the swapped order; delete and re-add them.
- **Help output.** Anything that parses the unfiltered `Help` output will now receive the module's usage help instead of ZNC's generated table. Filtered `Help <filter>` output is unchanged except for the row describing `Help` itself.
- **Storage format is unchanged.** Entries in the 1.1 format and the 1.0.0 legacy format load as before.
- **ZNC compatibility.** Runtime-tested against ZNC 1.9.1; compiles against ZNC 1.9.0. No ZNC API changes were required.

---

## [1.1.2] — 2026-09-11

This release fixes the `%nick%` short-circuit that 1.1.1 described but never performed, and withdraws the guidance that recommended `AddSecret` for credentials. Built with `-Wall -Wextra` and runtime-tested against ZNC 1.9.1; also compiles with `znc-buildmod` against ZNC 1.9.0.

### Changed
- **`AddSecret` help text.** The `AddSecret` description shown by `Help` now reads "Same as Add, but the command text is masked in module output. Not encryption; do not use for credentials." The corresponding lines in `CmdHelp()` were updated with equivalent wording.
- **README.** Added a *Credentials and secrets* section; removed examples that used `AddSecret` for `NickServ IDENTIFY` and `OPER`; documented that `%nick%` is case-sensitive and that commands without it are never subject to nick validation.

### Fixed
- **Nick validation blocked commands that do not use `%nick%`.** `ExpandVars()` was meant to return early when a command contains no `%nick%`, but the test `out.Find("%nick%") < 0` compared the unsigned (`size_t`) result with zero and was always false. Every command therefore fetched and validated the current nick, and when that nick failed `IsValidIRCNick()` (for example, a non-ASCII nick on a network that permits one), every entry was skipped with `Skipped (invalid nick for expansion)`, including entries with no `%nick%`. The test now compares with `CString::npos`. The 1.1.1 entry below states that non-`%nick%` commands no longer fetch or validate the nick; that behavior takes effect only as of this release.
- **`%nick%` detection now matches substitution.** The presence check is case-sensitive, like `CString::Replace()`. A command containing only another casing, such as `%NICK%`, is still sent literally and is no longer subject to nick validation.
- **README version-check instructions.** `Help` does not display the module version, and `ListMods` does not display module descriptions. The README now points to `Version` and `ListAvailMods`.

### Security
- **Credential guidance corrected.** `Help` and the README previously recommended `AddSecret` for `NickServ IDENTIFY` and `OPER`. They now state that `AddSecret` only masks the module's own output and advise against storing credentials in the module: the stored value is recoverable base64, the line typed to add an entry is shown and possibly logged by the IRC client and relayed by ZNC to other attached clients, and the command is sent as an ordinary message to a nickname after registration. This is a documentation and help-text change only; existing secret entries behave as before.

### Compatibility
- **Storage format is unchanged.** Entries in the 1.1 format (`<delay>|<flags>|<base64>`) and the 1.0.0 legacy format (`<delay>|<base64>`) load as before, and unknown flag characters are still preserved on rewrite.
- **Runtime behavior.** With a nick that passes `IsValidIRCNick()`, behavior is identical to 1.1.1. When the current nick fails validation, commands without `%nick%` are now sent instead of skipped; commands with `%nick%` are still skipped.
- **ZNC compatibility.** Runtime-tested against ZNC 1.9.1; compiles against ZNC 1.9.0. No ZNC API changes were required.

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
