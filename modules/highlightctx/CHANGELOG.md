# Changelog

All notable changes to the `highlightctx` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.7.0] — 2026-04-21

This release fixes a long-standing usability footgun where `require_ignore_drop=auto` could stay unarmed after `/znc restart` even when `ignore_drop` was loaded, because the original arming logic only looked at `ignore_drop`'s presence at `OnLoad` time. If the ZNC config happened to load `highlightctx` first (e.g. alphabetical ordering), the armed flag was locked to `false` for the rest of the process's lifetime. The primary fix is a new `OnBoot` hook that re-checks `ignore_drop`'s actual position in the module list after all `znc.conf` modules have loaded, plus a manual `Rearm` command for runtime re-checks without reloading the module. Built against ZNC 1.9.0 headers in the development container and verified end-to-end through an automated test harness with fifty-five assertions covering module load, command surface, load-order scenarios, detached capture with replay, and 0.6.0→0.7.0 upgrade of on-disk state.

### Added

* **`OnBoot()` override.** Fires after all `znc.conf` modules have been loaded. At this point the module inspects actual module-list positions to determine whether `ignore_drop`'s hooks will dispatch before ours. This is the primary fix for the `/znc restart` case: `OnLoad`'s `HasIgnoreDropLoaded()` can return `false` even when `ignore_drop` is about to load right after us; `OnBoot` re-checks once everything is in the list. For dynamically loaded modules not present in `znc.conf`, `OnBoot` is not called by ZNC; the `Rearm` command below covers that case.
* **`Rearm` command.** Manual re-check of `ignore_drop` presence and hook-order position without reloading the module. Reports a clear diagnosis — not armed because not loaded, not armed because positioned at or after us in the module list, or armed because positioned ahead — and flags arm/disarm transitions relative to the previous state. Outside of `auto` mode, `Rearm` updates the diagnostic state but does not change behavior.
* **`IsIgnoreDropAheadOfUs()` helper.** Iterates the network's module list once and returns `true` only when `ignore_drop` is present AND positioned at a lower index than `highlightctx`, so its hooks dispatch before ours.
* **`RecheckIgnoreDropHookOrder()` helper.** Single re-check routine shared by `OnBoot` and `Rearm`.

### Changed

* **Semantics of the auto-mode armed flag.** The internal arming heuristic was previously "was `ignore_drop` loaded at my `OnLoad` time" (captured as `m_ignore_drop_present_on_module_load`). The updated semantic is "was `ignore_drop` found ahead of us in hook order at last check," where "last check" is one of `OnLoad`, `OnBoot`, or a `Rearm` invocation. The variable name is preserved for minimal diff; the meaning is now the stricter, position-aware version.
* **Status output label updated.** `ignore_drop was present when highlightctx loaded: yes/no` is replaced with `ignore_drop ahead of highlightctx in hook order: yes/no` to reflect the new semantic. Other `Status` lines are unchanged.
* **Status/setmode warning text updated.** The warning shown when auto mode is unarmed but `ignore_drop` is loaded now correctly attributes the cause to "positioned at or after highlightctx in the module list" rather than "was not present when highlightctx loaded," and directs the user to `Rearm` and the module-list reordering workarounds.
* **`SetRequireIgnoreDrop` help text updated** to reference the new arming semantics and mention `Rearm`.
* **`Overview` help text updated** to describe the actual re-evaluation points (`OnLoad`, `OnBoot`, `Rearm`), to note that runtime load/unload of `ignore_drop` does not auto-refresh the armed flag, and to explain the independent runtime safety net.
* **Primary-commands summary line** in `Overview` now includes `Rearm`.
* **Version marker bumped** from `highlightctx 0.6.0` to `highlightctx 0.7.0`. Stored in the `version_marker` NV entry on successful load.
* **Header comment block updated** with the new arming semantics, the complete list of re-evaluation points, and the ZNC 1.9.x dispatch limitation on module-lifecycle hooks.

### Fixed

* **Auto mode staying unarmed after `/znc restart` despite `ignore_drop` being loaded.** When the ZNC config load order placed `highlightctx` ahead of `ignore_drop` (the common alphabetical case, since `h` precedes `i`), `OnLoad`'s `HasIgnoreDropLoaded()` returned `false`, `m_ignore_drop_present_on_module_load` was set to `false`, and no subsequent event re-evaluated it. The new `OnBoot` hook re-checks the actual module-list positions after all `znc.conf` modules have loaded; the `Rearm` command provides a manual fallback for dynamically loaded configurations. Note: neither `OnBoot` nor `Rearm` can arm auto mode if `highlightctx` is still positioned ahead of `ignore_drop` in the module list; the user must also reorder `znc.conf` or reload the module to move `highlightctx` to the tail of the list. This is now clearly surfaced by `Status` and `Rearm`.
* **Stale "reload highlightctx after ignore_drop" guidance.** Previous wording implied that simply loading `ignore_drop` later and then reloading `highlightctx` would arm auto mode. That guidance was incomplete — it only worked because it happened to re-run `OnLoad` after `ignore_drop` was already present. The updated guidance now makes the actual requirement explicit: `ignore_drop` must sit ahead of `highlightctx` in the module list so its hooks dispatch first.

### Security

* **No new attack surface.** The additions are purely diagnostic and state-tracking. They do not read untrusted data, do not touch the journal, and emit only module-output messages whose content is derived from module names already known to the module loader. The capture/replay hot path is unchanged.
* **Capture correctness is still runtime-checked.** `ShouldCaptureNow()` calls `HasIgnoreDropLoaded()` live on every incoming message, so capture fails closed the moment `ignore_drop` is unloaded, independent of the armed flag. The armed flag is a setup-time guarantee about hook order; the runtime presence check is the safety net.
* **Armed state is deliberately sticky.** Once `auto` is armed, unloading `ignore_drop` at runtime does not disarm automatically — the strict requirement stays effective so capture pauses (rather than silently resuming without the protection the user asked for). Explicit disarm is available via `Rearm` or by changing the mode with `SetRequireIgnoreDrop`.

### Compatibility

* **Storage format unchanged.** NV entries (`before_max`, `after_max`, `max_events`, `require_ignore_drop_mode`, legacy-compat `require_ignore_drop`, `excluded_channels`, `version_marker`) use the same on-disk representation as 0.6.0. Journal format (`B`/`A`/`F`/`D` records, double-hex encoded fields) unchanged. 0.6.0 journals load cleanly into 0.7.0 with no migration step — verified by the test harness, which seeds a 0.6.0-format journal and confirms the event replays on next attach with the correct channel, nick, trigger marker, and compaction post-replay.
* **ZNC compatibility.** Built against ZNC 1.9.0 headers (`znc-dev 1.9.0-2build3`) in the development container and run end-to-end against the ZNC 1.9.0 binary. The hook signatures used (`OnBoot`, plus the pre-existing capture hooks) have been stable across ZNC 1.9.x, so this is expected to build and run unchanged against ZNC 1.9.1. A local `znc-buildmod` against 1.9.1 on the target host is still recommended before production deployment.
* **User-visible commands and command arguments unchanged** except for the addition of `Rearm`. All 0.6.0 commands take the same arguments and behave the same way.
* **Load-arg syntax unchanged.** `before`, `after`, `require_ignore_drop`, `max_events`, `excludes` are all parsed identically to 0.6.0.
* **No operator action required** when upgrading a running 0.6.0 deployment. On the next load of 0.7.0, the armed flag is re-evaluated with the improved logic. If `znc.conf` currently has `LoadModule = highlightctx` before `LoadModule = ignore_drop`, auto mode will remain unarmed until the order is fixed or until the module is reloaded manually — the same remediation that already applied under 0.6.0, just now clearly surfaced by `Status` and `Rearm`.

### Testing

An automated test harness was written for this release (not shipped with the module). It runs against ZNC 1.9.0 as a non-root user via `runuser` and a fake IRC server for the upstream connection. Coverage:

- module load + version + default settings + all expected `Status` keys (including the new label) — 8 assertions
- `znc.conf` load order with `ignore_drop` first: correct armed state, effective requirement active — 4 assertions
- `znc.conf` load order with `highlightctx` first: correctly unarmed, `Rearm` reports position problem with remediation — 5 assertions
- full command surface: `SetBefore`/`SetAfter`/`SetMaxEvents` (including numeric and `off`), `AddExclude`/`DelExclude`/`ListExcludes`, `Reset`, `SetRequireIgnoreDrop on` rejected when absent, mode preservation on rejection, `Rearm` diagnostic output — 11 assertions
- runtime `LoadMod ignore_drop` after highlightctx: `Rearm` correctly reports wrong order, subsequent `UpdateMod highlightctx` moves it to the tail and `Rearm` arms — 5 assertions
- `UnloadMod ignore_drop` at runtime: sticky armed remains `yes`, effective requirement remains `yes`, subsequent `Rearm` explicitly disarms and reports the transition — 7 assertions
- detached-capture + on-attach replay: disconnect client, inject channel traffic with a highlight, reconnect, verify replay contains the channel, the event header, the `>>>` trigger marker, the triggering speaker, `pending=0`, and journal compaction — 7 assertions
- 0.6.0 → 0.7.0 upgrade: seed a 0.6.0-format journal, attach, verify on-attach replay delivers the event correctly, `pending=0` afterward, and journal is compacted — 6 assertions

Result: **55/55 passing** against ZNC 1.9.0.

---

## [0.6.0] — Initial release

### Added

* Detached-only highlight context capture as a ZNC network module for ZNC 1.9.1+.
* Independent per-channel in-memory ring buffer for pre-highlight context, decoupled from normal ZNC playback buffer length.
* Durable on-disk journal (`highlightctx.journal`) recording only real highlight events (`B` begin, `A` after-line, `F` finalize, `D` delivered) with `fsync` + parent-directory fsync for write durability, and atomic tmp+rename compaction.
* Automatic replay into `*highlightctx` on `OnClientAttached`, sorted by channel then event time/ID, followed by compaction and clearing of delivered events.
* Native IRCv3 `server-time` / `@time=` replay for clients that advertise support, with an inline UTC-prefixed fallback for clients that do not.
* Nick-boundary-aware highlight detection (case-insensitive) against the current network nick.
* `ignore_drop` integration with `off` / `on` / `auto` modes, gated on presence at `OnLoad` time.
* Configurable `before` and `after` context caps, per-channel exclusion list, and optional `max_events` cap on the pending-event list.
* Runtime commands: `Help`, `Overview`, `Version`, `Status`, `ReplayNow`, `SetBefore`, `SetAfter`, `SetMaxEvents`, `AddExclude`, `DelExclude`, `ListExcludes`, `SetRequireIgnoreDrop`, `Reset`, `Compact`, `ClearPending`.
* Persistent settings via module NV entries, with a legacy-compatibility alias for the old `require_ignore_drop` boolean.
