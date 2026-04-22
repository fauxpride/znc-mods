# Changelog

All notable changes to the `keepchanbuffersize` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.2.0] — 2026-04-22

This release removes the `Enable`/`Disable` toggle and all related machinery. The toggle was a foot-gun: a loaded-but-disabled module is visually indistinguishable from a bug, and at least one real user tripped over it (setting a per-channel buffer, running `/cycle`, and reporting the module as broken when the actual cause was `Disable` having been invoked at some earlier point). The standard ZNC idiom for "I do not want this module running right now" is `UnloadMod`, which already preserved the module's NV data the same way `Disable` did, so the toggle was covering no case `UnloadMod` did not already cover. Built and runtime-tested against ZNC 1.9.1 with zero warnings.

### Added

* **Silent cleanup of the legacy `Enabled` NV key on load.** Installs upgrading from 1.0.x or 1.1.x will have a top-level `Enabled` entry in the module's `.registry` file. On the first 1.2.0 load, that entry is deleted via `DelNV("Enabled")`. The operation is idempotent, and per-channel `buf:*` entries are untouched. No user action required.
* **Deprecation notice for the `enabled=0|1` load argument.** Previously this argument (and its `off/on/false/true` aliases) controlled the initial state of the toggle. It no longer has any effect, but the module now specifically recognizes the token and prints `Note: the 'enabled=' load argument is deprecated in 1.2.0 and has no effect. The module is always active while loaded; use UnloadMod to stop it. Stored per-channel values are preserved.` to the module window. This is more honest than silently ignoring the argument and letting the user believe their intent was honored.
* **"Upgrading from 1.1.x or 1.0.x" section in the README,** covering the automatic behaviors above and the interface changes described below.

### Changed

* **The module is now always active while loaded.** The `m_bEnabled` field, the `!m_bEnabled` checks in every user and server hook, and the `Enabled` NV flag are all gone. On `LoadMod`, remember/restore behavior begins immediately; on `UnloadMod`, it stops. There is no intermediate state.
* **`Status` no longer reports an `ENABLED`/`DISABLED` line.** It now reports only the number of remembered channels. The enabled/disabled status line was the piece of UI most likely to give the misleading impression that a disabled module was "just configured off rather than broken" — removing the toggle removes the need to report on it.
* **`OnLoad` no longer reads or writes the `Enabled` NV key** beyond the one-time cleanup delete described above.

### Removed

* **`Enable` command.** Issuing it now returns `Unknown command!`. Use `LoadMod keepchanbuffersize` instead.
* **`Disable` command.** Issuing it now returns `Unknown command!`. Use `UnloadMod keepchanbuffersize` instead.
* **`m_bEnabled` member and `Enabled` NV key from the runtime state model.** Removing these eliminated roughly one branch per hook and the load-time argument parser for the `enabled=` key is now a single deprecation-notice branch rather than a set of NV writes.

### Compatibility

* **Per-channel NV storage format is unchanged.** Entries written by 1.0.0, 1.1.0, and 1.1.1 continue to load and restore correctly. The only NV modification this release performs automatically is deleting the top-level `Enabled` key, which no version of the module from 1.2.0 onward reads.
* **LoadMod arguments.** `LoadMod keepchanbuffersize` is the supported invocation. `LoadMod keepchanbuffersize enabled=0` (or `=1`, `=off`, `=on`, etc.) still succeeds but prints a deprecation notice and does not change behavior.
* **Migration semantics.** Per-channel `buf:*` keys are never touched by the 1.2.0 upgrade; only the global `Enabled` key is removed. The RFC 1459 casemap migration introduced in 1.1.0 continues to run on load and remains a no-op for ASCII-only channel names.
* **Interface break for scripts.** Any external tooling that expected to issue `/msg *keepchanbuffersize Enable` or `Disable` will now receive `Unknown command!`. Switch to `LoadMod` / `UnloadMod` via `*status`. This is the release's single intentional compatibility break and is the reason for bumping the minor version rather than the patch version.
* **ZNC compatibility.** Built and test-compiled against ZNC 1.9.1 with zero warnings. No new ZNC API surface is used relative to 1.1.1. Functional verification was performed by running the module inside a ZNC 1.9.1 instance, exercising every command, reloading with the deprecated argument (deprecation notice verified), and planting a legacy registry containing a top-level `Enabled 1` entry and an ASCII-lowered `buf:#foo[bar]` entry (both were correctly removed/rewritten on load).

---

## [1.1.1] — 2026-04-22

This release fixes a regression-class bug present in 1.0.0 and 1.1.0 that prevented the module's entire reason for existing — restoring an explicit per-channel buffer size across `PART`/`JOIN` — from working whenever the channel's stored buffer value exceeded `MaxBufferSize`. Built and runtime-compiled against ZNC 1.9.1 with zero warnings.

### Fixed

* **Restore silently failed when the stored buffer exceeded `MaxBufferSize`.** `ApplyRememberedToChan()` called `CChan::SetBufferCount(uWanted, false)`, and `CBuffer::SetLineCount()` enforces `CZNC::Get().GetMaxBufferSize()` only when `bForce` is `false`, silently returning `false` otherwise. Webadmin and the config loader set per-channel buffers with `bForce = true`, so any explicit channel buffer that exceeds `MaxBufferSize` is both configurable and persistable through ZNC's own interfaces, but was **not** restorable by this module after the channel object was torn down on PART. The user-visible symptom was: set a channel buffer of e.g. 450 via webadmin, `/cycle`, watch the buffer come back at the user-level `ChanBufferSize` default. Restore now passes `bForce = true`, matching the path webadmin and config use. The up-front validation added in 1.1.0 (`CmdSet` rejects values above `MaxBufferSize`) ensures forcing on restore only ever applies values that were previously accepted by ZNC through some legitimate channel.
* **Immediate-apply in `CmdSet` had the same silent-failure mode.** When `Set <#chan> <lines>` was issued while the channel existed, the immediate apply used `bForce = false` and would return the "failed to apply immediately (may exceed limits)" note for values above `MaxBufferSize` even though 1.1.0's front-end validation had already confirmed the value was within limits. Now that the front-end already rejects values above `MaxBufferSize`, the immediate-apply uses `bForce = true` for consistency with the restore path.

### Changed

* **Warning message on failed restore is shorter.** The old "may exceed limits" hint was misleading once restore began forcing, since the front-end already gates out values above `MaxBufferSize` at `Set` time and legitimately-configured values cannot fail the force path for that reason. The warning is now simply "failed to restore buffer size for `<chan>` to `<n>`."

### Compatibility

* **NV storage format is unchanged.** Entries written by 1.0.0 or 1.1.0 continue to load and restore correctly; the fix does not touch the data shape at all.
* **Command interface is unchanged.** `Set`, `Forget`, `List`, `Status`, `Version`, `Enable`, `Disable` all behave identically to 1.1.0 for well-formed input.
* **ZNC compatibility.** Built and test-compiled against ZNC 1.9.1 with zero warnings. No new ZNC API surface is used relative to 1.1.0.

---

## [1.1.0] — 2026-04-22

This release addresses findings from a security and correctness review of 1.0.0. It tightens input validation on the manual `Set` command, adds channel-name shape validation to both `Set` and `Forget`, fixes a latent self-identification bug on the `KICK` code path, and switches stored-key normalization from plain ASCII lowercasing to RFC 1459 casemapping. Built and runtime-compiled against ZNC 1.9.1 with zero warnings.

### Added

* **`Version` command.** `/msg *keepchanbuffersize Version` replies with `keepchanbuffersize version <x.y.z>` for quick identification of the running build. The version string is also embedded in the module description so it surfaces in `/msg *status ListMods`.
* **Channel-name shape validation in `Set` and `Forget`.** Both commands now reject arguments that do not begin with a valid channel prefix. When connected, the server-advertised `CHANTYPES` list is consulted via `CIRCNetwork::IsChan()`; before `ISUPPORT` arrives (or when the module has no network), the check falls back to the RFC defaults `#`, `&`, `+`, `!`. Previously the commands accepted any string and would happily write a `buf:<garbage>` entry to NV storage.
* **Strict numeric validation in `Set`.** A new digits-only check runs before `ToUInt()`, so inputs like `-1`, `10abc`, or empty values are rejected up front rather than being silently coerced (a negative like `-1` would previously wrap through `ToUInt()` into `UINT_MAX`).
* **Upper-bound check in `Set` against `CZNC::Get().GetMaxBufferSize()`.** Values that exceed ZNC's configured global maximum are rejected before being written to NV storage, so a bogus value can never be persisted and re-tried on every subsequent `JOIN`. When `MaxBufferSize` is `0` (ZNC's "unlimited" sentinel), no upper bound is enforced.
* **RFC 1459 casemapping for channel-key normalization.** A new `IRCLower()` helper treats `[`, `]`, `\`, `~` as the uppercase of `{`, `}`, `|`, `^` — the IRC default casemap — rather than the plain ASCII lowercasing previously done via `CString::AsLower()`. For ASCII-only channel names this produces the same output, so the change is invisible in the common case.
* **One-time legacy-key migration in `OnLoad`.** Any `buf:*` entry whose stored key differs from its RFC 1459 form is transparently rewritten to the new form, preserving its value. The migration collects keys before mutating NV (to avoid iterator invalidation), runs at most once per key, and skips rewrites where the new-form key already holds a value to avoid clobbering live data.

### Changed

* **Module description now includes the version string.** `NETWORKMODULEDEFS` embeds `v1.1.0` so `ListMods` shows which build is loaded without having to run a command.
* **`Set` now validates before persisting.** The previous order of operations was "write to NV, then attempt to apply"; it is now "validate, then write to NV, then attempt to apply". This closes the case where a value that ZNC would always reject at apply time (e.g. above `MaxBufferSize`) could still be kept in storage and retried on every `JOIN`.

### Fixed

* **Bug: `IsMe(const CString&)` used case-insensitive ASCII comparison instead of IRC casemap comparison.** This overload is reached via `OnKickMessage` through `CKickMessage::GetKickedNick()` (which returns `CString`, not `CNick`). On networks where your nick contains characters that IRC casemaps treat as equivalent (`[ ] \ ~` vs `{ } | ^`), the old implementation could fail to recognize that *you* were the one kicked, which silently skipped the fallback remember on the `KICK` path. The `CString` overload now constructs a `CNick` and delegates to the `CNick` overload, so `NickEquals()`'s casemap-aware comparison is what actually runs.

### Security

* **Eliminates a "garbage key" foot-gun in NV storage.** With `Set`/`Forget` now rejecting inputs that don't look like channel names, an authenticated user (or a misbehaving script attached to the module window) can no longer cause arbitrary `buf:<junk>` entries to accumulate in the module's NV file. This was ranked low severity in the 1.0.0 review — only someone already authenticated to your ZNC could trigger it and the worst case was self-inflicted — but the validation makes the bug class unreachable rather than merely uncomfortable.
* **Eliminates a "stored-then-rejected" persistence pattern.** Values that exceed `MaxBufferSize` are no longer written to NV. Previously, a value you could observe being rejected at apply time would still live in storage and generate a warning on every subsequent `JOIN`. Not a security vulnerability in the classical sense, but the fix removes a small class of persistent noise that a hostile or careless script could seed.

### Compatibility

* **NV storage format is unchanged.** Entries written by 1.0.0 (keys of the form `buf:<ascii-lowered-channel>` with a decimal integer value) continue to load. The only automatic modification is the one-time RFC 1459 migration, which is a no-op for the overwhelming majority of channel names and never drops data it could preserve.
* **Command interface is unchanged for existing callers.** `Set #foo 200` and `Forget #foo` behave identically to 1.0.0 when given well-formed input; only previously undefined inputs (non-digit buffer values, non-channel-looking arguments) now return an explicit error message.
* **ZNC compatibility.** Built and test-compiled against ZNC 1.9.1 with zero warnings. The new ZNC APIs used — `CZNC::Get().GetMaxBufferSize()`, `CIRCNetwork::GetChanPrefixes()`, `CIRCNetwork::IsChan()` — are present in 1.9.0 as well, so the module should continue to build against 1.9.0 without modification, though 1.9.1 is the declared target because it is the current CVE-patched release.
* **Downgrade behavior.** A ZNC instance running 1.0.0 of this module against an NV file written by 1.1.0 will continue to function: RFC 1459 keys for ASCII-only channels are identical to the ASCII-lowercased keys 1.0.0 writes, and the handful of non-ASCII corner cases would be invisible to 1.0.0 because `AsLower()` would miss them in either direction.

---

## [1.0.0] — Initial release

### Added

* Preserve per-channel `BufferSize` across quick `PART`/`JOIN` cycles, including `/cycle`.
* Early-capture remember logic on user raw `PART` (`OnUserRawMessage`) so the channel's explicit buffer size is captured before ZNC can tear down the channel object.
* Structured `OnUserPartMessage` and `OnUserJoinMessage` hooks as secondary safety nets for clients/scripts whose timing differs from the raw path.
* Restore on server `JOIN` (`OnJoinMessage`) when the channel object is guaranteed to exist, plus an opportunistic early restore during user-side join if the channel already exists.
* Fallback remember on server `PART` (`OnPartMessage`) and on `KICK` (`OnKickMessage`) for non-client-initiated leave flows.
* `JOIN 0` snapshot: captures all current channels before they may be torn down.
* `Status`, `Enable`, `Disable`, `List`, `Set`, `Forget` commands via `/msg *keepchanbuffersize`.
* `enabled=0|1` load argument (also accepts `off/on/false/true`), with per-network NV persistence of the enabled flag and the remembered per-channel values.
* "Explicit setting only" policy: values are persisted only when `CChan::HasBufferCountSet()` is true, so default-behavior channels are not turned into unnecessary persistent overrides.
* On-load application: when the module loads and is enabled, it walks existing channel objects and reapplies any remembered values, covering module-reload and post-reconnect scenarios.
