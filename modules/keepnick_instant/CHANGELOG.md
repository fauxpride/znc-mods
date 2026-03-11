# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.5.1] - 2026-03-12

### Fixed
- Fixed the backend timer constructor so ZNC now receives the intended unique backend timer label as the label, not as the description. In `1.5.0`, the label/description arguments were reversed, which caused repeated `failed to arm backend timer` warnings and could stall the recurring reclaim loop.
- Fixed the reclaim dedupe timer constructor in the same way, so the dedupe timer now uses an empty label as intended instead of reusing a non-empty label across runs.
- Fixed the shared scheduler regression that made ordinary periodic reclaim unreliable on both `MONITOR` and `ISON` backends unless `Poke` was used to bypass the broken timer path.

### Changed
- Bumped version from `1.5.0` to `1.5.1`.
- Added explicit warning output if the reclaim dedupe timer itself cannot be armed.

### Documentation
- Updated the README timer-model section to document the corrected ZNC timer constructor argument order and the resulting fix for the stalled periodic backend loop.
- Updated README version references and source summary to `1.5.1`.

### Notes
- `Poke` remains a true force-run path, but ordinary scheduled reclaim should now work again without needing it as a workaround.
- This release is aimed at removing the `failed to arm backend timer` warnings and restoring reliable periodic reclaim on both `MONITOR` and fallback `ISON` networks.

## [1.5.0] - 2026-03-12

### Fixed
- Fixed a backend-flow issue where reclaim could fail to start promptly after the preferred nick was visibly lost, even though the module was otherwise loaded and enabled.
- Fixed `Poke` so it now acts as a real force-run path instead of depending on whether a backend timer was already pending.
- Fixed backend timer scheduling so immediate/replacement backend checks do not silently stall behind an already-armed timer.
- Hardened the one-shot backend timer model by using unique backend timer labels and explicit replacement logic when a forced backend check is needed.
- Fixed the shared scheduler path that could make both `MONITOR` and fallback `ISON` behavior appear inactive after hot-swaps, reconnects, or reactive reclaim attempts.

### Changed
- Bumped version from `1.4.0` to `1.5.0`.
- When the module sees the primary nick become available through visible `NICK` or `QUIT` traffic, it now immediately kicks the selected backend instead of waiting for the next ordinary scheduled backend tick.
- In `MONITOR` mode, once a confirmed subscription is active and reclaim is still needed, later backend ticks can send `MONITOR S` to refresh current status and preserve persistent retry behavior.
- `Poke` now ignores pending backend scheduling and forces an immediate backend check.
- Reactive reclaim follow-up now force-rearms the backend scheduler when needed, instead of only attempting a passive re-arm.

### Documentation
- Updated the README to document the new immediate backend-kick behavior when the nick is visibly lost.
- Updated the README `Poke` documentation to reflect that it is now a true force-run action.
- Updated the README timer-model section to describe unique backend timer labels, forced timer replacement, and the more robust re-arm behavior.
- Updated the README `MONITOR` implementation notes to cover periodic `MONITOR S` refreshes while reclaim is still needed.
- Updated version references and source-summary text throughout the README.

### Notes
- Join-safe startup behavior after connect remains intact.
- `MONITOR` detection still uses `005` / `ISUPPORT` first, with the existing hot-reload live-probe path preserved for already-connected sessions.
- This release is aimed at restoring reliable reclaim behavior on both `MONITOR`-capable and `ISON`-only networks after the scheduler regressions observed in `1.4.0`.

## [1.4.0] - 2026-03-12

### Fixed
- Fixed `CRearmTimer::RunJob()` which was previously empty and did nothing. It now re-arms the backend scheduler if a reactive reclaim attempt failed (e.g. due to a race where another client grabbed the nick first) and no timer is currently armed. Previously, a failed reactive reclaim would leave the module silent until the next coincidentally scheduled backend tick.
- Fixed `MaybeProbeMonitor()` setting `MonitorActive = true` before the server confirmed the `MONITOR` subscription. `MonitorActive` is now only set to `true` when the server responds with a `730` or `731` numeric, confirming the subscription. The previous behavior could leave the module believing `MONITOR` was active and working on servers that silently drop unknown commands, preventing fallback to `ISON` and stalling nick reclaim indefinitely.

### Changed
- Bumped version from `1.3.1` to `1.4.0`.

### Documentation
- Updated README timer model section to reflect the new `CRearmTimer` behavior.
- Updated README MONITOR state tracking section to document that `MonitorActive` is only set on server-confirmed response, and explain why.
- Updated version references throughout README.

## [1.3.1] - 2026-03-11

### Changed
- Bumped version from `1.3.0` to `1.3.1`.
- Refined backend detection so hot-swapped reloads on already-connected sessions can detect `MONITOR` without requiring a reconnect.
- Kept `005` / `ISUPPORT` as the primary detection path while adding a one-time live `MONITOR` probe path for already-live sessions that did not replay registration numerics.
- Preserved the existing `ISON` fallback behavior when `MONITOR` is unsupported or unusable on the current connection.

### Added
- Added one-time live `MONITOR` probing on already-connected sessions after hot-swapped reloads.
- Added fallback handling for unsupported live `MONITOR` probes so the module safely returns to `ISON` on that connection.
- Added status/help visibility for live probe state in module output.

### Documentation
- Updated the README to describe hot-swapped reload behavior, live `MONITOR` probing, and the backend-detection path for already-connected sessions.
- Updated troubleshooting guidance to cover the new hot-reload detection path.

### Notes
- Normal reconnect-based detection still works as before.
- This release specifically closes the gap where `MONITOR` auto-detection previously depended on seeing a fresh `005` after reconnect.

## [1.3.0] - 2026-03-11

### Added
- Added backend selection with `Auto`, `Ison`, and `Monitor` modes.
- Added `MONITOR` support for networks that advertise it via `005` / `RPL_ISUPPORT`.
- Added automatic backend selection logic:
  - `Auto` now prefers `MONITOR` when supported by the connected server.
  - Falls back to the existing `ISON` logic when `MONITOR` is unavailable.
- Added MONITOR state reporting to module status output.
- Added a `Backend <Auto|Ison|Monitor>` command to control backend selection.

### Changed
- Bumped module version from `1.2.0` to `1.3.0`.
- Updated runtime behavior so nickname recovery can use event-driven `MONITOR` tracking instead of polling where supported.
- Kept the original `ISON` reclaim path intact as the compatibility fallback.
- Preserved the existing reclaim model, including startup delay, idle-aware behavior, join-safe timing, jitter handling, and minimum spacing between reclaim attempts.
- Updated help/output text to reflect the new backend model and effective backend in use.

### Documentation
- Updated the README to document the new backend architecture and automatic `MONITOR`/`ISON` selection behavior.
- Updated usage documentation to cover the new `Backend` command and backend modes.
- Updated configuration/defaults documentation to explain how `Auto` behaves.
- Updated implementation details to describe:
  - `005` / `ISUPPORT` capability detection
  - `MONITOR` activation/deactivation
  - fallback to `ISON`
  - server-driven reclaim notifications vs polling
- Updated troubleshooting and behavior notes where relevant to account for `MONITOR` support.

### Notes
- No existing `ISON`-based behavior was removed.
- Networks without `MONITOR` support continue to behave as before.
- This release is intended to improve reclaim responsiveness on compatible networks while preserving the previous behavior everywhere else.
