# Changelog

All notable changes to this project will be documented in this file.

The format is loosely based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.6.4] - 2026-03-12

### Fixed
- Fixed ISON poll responses (`303`) appearing in the IRC client status window. On every ISON poll the server's `303` reply was passed through ZNC to the client, causing visible noise in the mIRC status window (and any other IRC client connected to ZNC). A new `ISONQueryPending` flag is set in `RunBackendCheckNow()` immediately before each `ISON <Primary>` is sent. In the `303` handler, after all reclaim logic has run, the flag is checked: if set, the `303` is halted before reaching the client. Manual `/ison` commands typed by the user are unaffected since `ISONQueryPending` is not set for those, and their `303` responses continue to pass through normally.

### Changed
- Bumped version from `1.6.3` to `1.6.4`.
- Added `ISONQueryPending` state field (bool, default false). Set to true in `RunBackendCheckNow()` before each `ISON <Primary>`. Cleared on `303` swallow, in `OnIRCConnected`, and in `OnIRCDisconnected`.

### Notes
- The `303` swallow happens after reclaim logic has fully run. `TryReclaim()` is called before the swallow decision, so nick reclaim behavior is completely unchanged — the numeric is only suppressed from the client after it has already been acted on.
- This mirrors the design of the `433` swallow introduced in `1.6.3`: the flag is specific to module-generated queries, manual user commands are never affected.

## [1.6.3] - 2026-03-12

### Fixed
- Fixed client-side scripts (e.g. mIRC Peace & Protection) activating their own competing nick retake logic in response to `433 :Nickname is already in use` responses caused by the module's own `NICK <Primary>` attempts. When ZNC's built-in `keepnick` module was also loaded, it was incidentally swallowing those `433` numerics before they reached the client, masking the problem. With `keepnick` disabled and `keepnick_instant` as the sole reclaim mechanism, `433` responses flowed through to the client and triggered P&P's retake display and timer on every failed attempt. A `NickAttemptPending` flag is now set in `TryReclaim()` immediately before each `NICK <Primary>` is sent. In `OnRaw`, a new `433` handler checks this flag and, if set and the rejected nick matches `Primary`, halts the numeric before it reaches the client. All other `433` responses — from manual nick changes, other scripts, or any nick other than `Primary` — are passed through to the client unchanged.

### Changed
- Bumped version from `1.6.2` to `1.6.3`.
- Added `NickAttemptPending` state field (bool, default false). Set to true in `TryReclaim()` before each `NICK <Primary>`. Cleared on a matching `433` swallow, on any non-matching `433`, in `OnIRCConnected`, and in `OnIRCDisconnected`.
- Added `433` to the `OnRaw` pre-filter so the cheap command token check includes it before any vector allocation.

### Notes
- The reclaim logic itself is completely unchanged. `TryReclaim()`, `RunBackendCheckNow()`, ISON polling, MONITOR subscription, `730`/`731` handling, and all timers behave identically to `1.6.2`. The `433` is only intercepted after the attempt has already been made and its outcome recorded internally — the module does not use the `433` response for any decision-making.
- The swallow is conservative by design: it requires both `NickAttemptPending == true` and `v[3]` matching `Primary` before halting. A race condition where the flag is set but the `433` refers to a different nick will clear the flag and pass the numeric through rather than swallowing it.
- This fix restores the behavior that users of both `keepnick` and `keepnick_instant` experienced previously, where `keepnick` was incidentally preventing client scripts from seeing module-generated `433` responses.

## [1.6.2] - 2026-03-12

### Fixed
- Fixed MONITOR support not being detected after a hot reload via `updatemod` when the module loads while you already own your primary nick. `MaybeProbeMonitor()` is gated by `HavePrimary()` and silently skips the probe in that case, leaving `MonitorSupported` false for the entire session. If you later lost your primary nick, the module would fall back to ISON even on a MONITOR-capable network like EFNet and remain on ISON for the rest of that session. A new `HotReloadDetectPending` flag is set in `OnLoad` when the module loads into an already-live connection. At the first backend tick, this flag triggers a one-time `MONITOR + <Primary>` probe unconditionally (bypassing the `HavePrimary()` gate). The server responds with `730` (online) or `731` (offline), which sets `MonitorSupported = true` and `MonitorActive = true` via the existing numeric handler. From that point on, any future `731` correctly triggers reclaim via the MONITOR path.

### Changed
- Bumped version from `1.6.1` to `1.6.2`.
- Added `HotReloadDetectPending` state field (bool, default false). Set to true in `OnLoad` when `IsConnected()` and `BackendMode != BACKEND_ISON`. Cleared at the first backend tick, in `OnIRCConnected`, and in `OnIRCDisconnected`.

### Notes
- The `HotReloadDetectPending` check is the first thing evaluated in `RunBackendCheckNow()`, before the `HavePrimary()` early-exit, specifically so the probe fires regardless of nick ownership. It is cleared immediately on first evaluation so it only fires once per hot reload.
- `MonitorActive` is still not set by the probe itself. It is only set to true when the server confirms the subscription via `730` or `731`, consistent with the policy established in `1.4.0` and `1.6.0`.
- On a hot reload where `Backend` is set to `Ison`, `HotReloadDetectPending` is not set and no probe is sent. The flag is only set when the module could meaningfully use MONITOR.
- This fix is specific to the hot-reload case. On a normal connect, `005` / ISUPPORT delivers MONITOR capability before the first backend tick fires, so the existing detection path is sufficient.

## [1.6.1] - 2026-03-12

### Fixed
- Fixed `OnLoad()` applying `StartDelaySec` (default 90s) unconditionally on hot reloads via `updatemod`. The 90-second join-safe delay exists to avoid reclaim traffic during the initial connect burst — CAP negotiation, auto-joins, channel floods — none of which are present on an already-live connection. Using the same delay on a hot reload meant MONITOR detection and reclaim were unnecessarily suppressed for up to 90 seconds on a session that was already stable. `OnLoad()` now checks `IsConnected()`: if the connection is already live (hot reload), it arms the backend with a single `IntervalSec` tick (default 5s) instead, which is long enough to avoid firing synchronously into the event loop but short enough to be effectively immediate. If the connection is not yet live, `StartDelaySec` is used as before, and `OnIRCConnected()` will re-arm with `StartDelaySec` when the connection comes up regardless.

### Changed
- Bumped version from `1.6.0` to `1.6.1`.

### Notes
- `OnIRCConnected()` continues to arm with `StartDelaySec` unconditionally, so the join-safe behavior on fresh connects is unchanged.
- The `IntervalSec` delay on hot reload is deliberately not zero. `OnLoad` fires synchronously in ZNC's event loop and a zero-delay timer would run in the same iteration, which is the same problem as calling `PutIRC()` directly. One `IntervalSec` tick defers the first action to the next loop iteration while keeping the delay functionally invisible to the user.

## [1.6.0] - 2026-03-12

### Fixed
- Fixed `OnRaw()` allocating a `VCString` and performing a full `Split()` on every incoming IRC line regardless of type. `OnRaw` fires for every PRIVMSG, NOTICE, JOIN, PART, MODE, and so on. The prior behavior caused unnecessary heap allocation and CPU overhead on every line in every channel. A cheap token extraction now filters the command field first; the full split is only performed for the eight command types the handler actually cares about (`005`, `303`, `730`, `731`, `734`, `421`, `NICK`, `QUIT`). This is the most likely contributor to ZNC send-queue buildup and observed high lag on busy connections.
- Fixed `EnsureMonitor()` prematurely setting `MonitorActive = true` before the server confirmed the MONITOR subscription. This was inconsistent with the same intentional guard already present in `MaybeProbeMonitor()` since `1.4.0`. The premature flag could cause the module to treat an unconfirmed subscription as active, preventing the ISON fallback on servers that silently drop unknown commands. `EnsureMonitor()` now uses `MonitorProbeSent` as the sent-but-unconfirmed guard, matching `MaybeProbeMonitor()` behavior. `MonitorActive` is only set to `true` when the server responds with a `730` or `731` numeric.
- Fixed `StopMonitor()` not resetting `MonitorProbeSent`, which would have prevented `EnsureMonitor()` from re-subscribing after an explicit stop if the probe had previously been sent via `EnsureMonitor()`.
- Fixed `LoadPrimary()` fallback using `GetNetwork()->GetNick()` — the current IRC nick — when no stored `PrimaryNick` is available. On a hot reload via `updatemod` while connected under an alternate nick (e.g. `fauxpride-`), this caused the module to poll for the wrong nick for the duration of that session. The fallback now uses `GetUser()->GetNick()`, which reflects the nick the user has configured in ZNC and is the correct authoritative source. `GetNV("PrimaryNick")` remains the first preference; the user-configured nick is only reached if no stored value exists.
- Fixed `OnLoad()` calling `MaybeProbeMonitor()` immediately on load. On a hot reload via `updatemod`, this fires synchronously into an already-live connection that may already have queue pressure, bypassing the `StartDelay` guard entirely. `MaybeProbeMonitor()` is already invoked from within `RunBackendCheckNow()`, so the MONITOR live-probe for already-connected sessions now happens naturally at the first backend tick after `StartDelaySec`. Hot reloads are treated the same as fresh connects in terms of how eagerly they interact with the server.

### Changed
- Bumped version from `1.5.3` to `1.6.0`.
- `EnsureMonitor()` now sets `MonitorProbeSent = true` (rather than `MonitorActive = true`) to guard against duplicate subscription sends while awaiting server confirmation, consistent with `MaybeProbeMonitor()`.

### Notes
- The `MonitorActive` flag continues to serve as the server-confirmed subscription indicator. The newly consistent use of `MonitorProbeSent` as the sent-but-unconfirmed guard applies to both the `MaybeProbeMonitor()` path (unconfirmed MONITOR support) and the `EnsureMonitor()` path (confirmed MONITOR support but subscription not yet acknowledged).
- The `OnRaw` pre-filter does not change any observable behavior. The same eight command types are still processed; all others are now rejected before a split is performed.
- Hot reload via `updatemod` no longer sends any IRC traffic synchronously during `OnLoad`. The first backend tick on a hot reload fires after `IntervalSec` (see 1.6.1 for the distinction between hot-reload and fresh-connect delay). The `MaybeProbeMonitor()` call that was previously in `OnLoad` was removed; it now happens naturally from within `RunBackendCheckNow()` at the first backend tick.

## [1.5.3] - 2026-03-12

### Changed
- Bumped version from `1.5.2` to `1.5.3`.
- Reworked the `MONITOR` backend so scheduled retry behavior now uses locally remembered offline state instead of sending `MONITOR S` on every backend tick.

### Fixed
- Fixed the `MONITOR` backend design so it no longer recreates polling-like traffic with periodic `MONITOR S` status refreshes while reclaim is still needed.
- Fixed the `MONITOR` retry path to keep trying on the module's own scheduler cadence after a confirmed offline (`731`) state, without needing to re-query the server every tick.

### Documentation
- Updated the README backend-model, feature-summary, high-level-behavior, implementation, troubleshooting, and source-summary sections to describe the remembered-offline MONITOR design and the removal of periodic `MONITOR S` refreshes.

### Notes
- `MONITOR` remains event-driven for state discovery: the module still relies on server `730`/`731` notifications to learn whether the nick is online or offline.
- Once the nick is known free, the module's own scheduler handles retry cadence locally until the nick is recovered or a later `730` marks it taken again.

## [1.5.2] - 2026-03-12

### Fixed
- Fixed excessive IRC traffic generated by visible `NICK` and `QUIT` events for the primary nick. In `1.5.0` and `1.5.1`, each such event called both `TryReclaim()` and `KickBackendNow(true)`, which sent a `NICK` attempt followed immediately by an additional ISON or MONITOR command and a forced timer reset. On busy channels with frequent nick changes or quits, this compounded into a rapid burst of outgoing commands that built up ZNC's send queue and caused noticeable client lag. The `KickBackendNow(true)` call has been removed from the `NICK` and `QUIT` handlers. Reactive reclaim now calls only `TryReclaim()`, and the existing `CRearmTimer` mechanism (introduced in `1.4.0`) handles re-arming the backend scheduler if the reclaim attempt was rejected.

### Changed
- Bumped version from `1.5.1` to `1.5.2`.

### Documentation
- Updated README opportunistic event handling section to reflect that reactive reclaim no longer kicks the backend immediately.
- Updated README high-level behavior description (step 5) to match.
- Updated README idle-aware behavior section to remove the reference to reactive kicks bypassing idle suppression.
- Updated README feature summary to replace the immediate backend kick bullet with a more accurate reactive reclaim description.
- Updated README source summary version and description.
- Updated README troubleshooting MONITOR Active version reference.

### Notes
- `KickBackendNow` is still used by the `Enable`, `SetNick`, `Backend`, and `Poke` command handlers, where an immediate forced backend action is the deliberate and expected behavior.
- The fix restores the conservative traffic model of earlier versions while preserving correct reclaim behavior after visible nick-loss events.

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
