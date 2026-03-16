# keepnick_instant

See [CHANGELOG.md](./CHANGELOG.md) for version history and release notes.

`keepnick_instant` is a ZNC network module that quickly regains your preferred nickname on networks without nick registration services.

It is designed for the classic IRC case where there is no NickServ-style ownership gate keeping your preferred nick reserved for you, and where the right recovery action is simply to switch back the moment that nick becomes available.

As of version **1.6.4**, the module supports two reclaim backends:

- **`MONITOR` when the server advertises it via `005` / ISUPPORT**
- **`ISON` otherwise**

The default behavior is **`Backend Auto`**, which means the module will automatically use `MONITOR` where available and fall back to the original `ISON` logic everywhere else.

That makes it a good fit for mixed IRC environments:

- it keeps the original Undernet-style `ISON` behavior on networks that do not offer `MONITOR`,
- it gains push-style reclaim on daemons and networks that do support `MONITOR`,
- it still preserves the same safety model: idle-awareness, join-safe startup delay, optional jitter, and deduped `NICK` attempts.

This is especially useful on networks such as **Undernet**, which remains the primary environment this module was tuned around.

In other words: this module exists for the classic IRC case where your preferred nick may be temporarily occupied, you connect under an alternate nick, and you want ZNC to quietly and safely switch back the moment the preferred nick becomes available.

Compared with ZNC's built-in `keepnick` module, `keepnick_instant` is deliberately more cautious and more explicit about *when* it will try to reclaim. Stock `keepnick` periodically attempts a direct `NICK` back to the network's configured nick and also reacts to visible `NICK`/`QUIT` events for that nick, while `keepnick_instant` adds backend-aware availability checks, post-connect delay, idle-awareness, optional jitter, and deduped reclaim spacing. That makes the stock module a fine generic default, but makes `keepnick_instant` a better fit for Undernet-style networks where you want quick reclaim once the nick is truly free without adding unnecessary nick-change traffic.

---

## What this module is for

When ZNC connects to a network and your primary nick is already in use, the server will typically assign you an alternate nick. On networks without services-based nick ownership, the only thing preventing you from retaking your preferred nick is whether someone else is currently using it.

`keepnick_instant` automates that recovery path.

It does so with a deliberately cautious design:

- It uses **`MONITOR` when available**, and **`ISON` when not**.
- It only runs reclaim logic while you **do not** currently own the target nick.
- It waits after connect before it begins checking, so it does not interfere with initial connect/join phases.
- It avoids a backend tick immediately after you send commands, reducing the chance of clashing with your own activity.
- It spaces out actual `NICK` attempts so duplicate reclaims are not sent too aggressively.
- It can optionally add jitter so multiple clients or bouncers do not fall into the same timing rhythm.

The end result is an “instant-ish” nick reclaim module: responsive enough to recover your nick quickly, but careful enough to behave well on networks that are sensitive to bursts of activity.

---

## Intended environment

This module is intended for:

- IRC networks that **do not** have channel/nick services reserving nicks in the modern NickServ sense
- users who want ZNC to recover a preferred nick automatically
- environments where conservative, low-noise reclaim behavior is preferable to aggressive direct nick-change logic
- mixed environments where some networks advertise `MONITOR` and others do not

This module is **mainly designed to work for Undernet** and similar networks.

It is **not primarily aimed at** networks where nickname ownership is enforced by services and authentication workflows. On those networks, the correct solution is usually services identification and/or network-native account mechanisms rather than repeated nick reclaim attempts.

---

## How this differs from ZNC's built-in `keepnick`

ZNC already ships with a core module called `keepnick`, so it is reasonable to ask why this module exists at all. The short version is: `keepnick_instant` is not trying to reinvent the goal, but to change the *strategy* used to reach it.

### What stock `keepnick` does

ZNC's built-in `keepnick` is a small, generic reclaim module. In current ZNC source, it:

- tries to regain the network's configured nick with a repeating timer,
- also reacts quickly when it sees a visible `NICK` or `QUIT` for the target nick,
- disables itself in some cases where continuing would be counterproductive, such as specific nickname-change errors or when you intentionally change away from the configured nick.

That behavior is straightforward and useful, and it is one reason `keepnick` has been a long-standing core module.

### What `keepnick_instant` changes

`keepnick_instant` keeps the same overall objective, but changes the operational model in several important ways:

- it has an explicit backend mode: `Auto`, `Ison`, or `Monitor`,
- in `Auto`, it uses `MONITOR` if the server advertises it in `005`,
- otherwise it uses `ISON` to check whether the preferred nick is currently present before sending `NICK`,
- it only runs reclaim logic while you *do not* already own the preferred nick,
- it waits after connect before starting, so reclaim logic stays out of the initial connect/join burst,
- it can skip a backend tick if you were just active,
- it dedupes actual reclaim attempts with `MinGap`,
- it can add per-tick jitter to avoid perfectly regular timing.

In practice, that makes this module feel more deliberate, less spammy, and easier to tune for networks where being conservative matters.

### Why they do not fundamentally conflict

When both modules are aimed at the **same preferred nick**, they are not logically fighting each other: they both converge on the same end state, namely “you got your preferred nick back.” They do not share mutable state, do not patch or disable one another, and neither module depends on private behavior from the other.

The overlap is mostly limited to the possibility that both may send a `NICK` attempt around roughly the same time. That is usually just redundant, not adversarial. Once the nick is successfully reclaimed, `keepnick_instant` stops doing useful reclaim work because you already own the target nick, and the stock module has nothing useful left to do either.

That said, loading both at once is usually unnecessary. In most real setups, `keepnick_instant` should be treated as a more controlled **alternative** to stock `keepnick`, not as something that needs to be layered on top of it.

### Important caveat

The “no conflict” statement assumes both modules are trying to reclaim the **same** nick. If you intentionally configure ZNC's built-in `keepnick` to chase the network-configured nick while `keepnick_instant` is configured with a *different* `PrimaryNick`, then you have created a real policy conflict yourself: each module will be trying to converge on a different result.

For the intended use case, keep them aligned — or just run `keepnick_instant` by itself.

---

## Feature summary

- **Automatic backend selection**
  - `Backend Auto` uses `MONITOR` when the server advertises it and falls back to `ISON` otherwise.
- **Hot-reload MONITOR detection**
  - On an already-connected session, the module sends a one-time `MONITOR + <Primary>` probe at the first backend tick after a hot-swapped reload, regardless of whether you currently own your primary nick. This ensures MONITOR support is correctly established for the session even if you are already on your primary nick at reload time. The probe is deferred to the first tick after `IntervalSec` rather than fired synchronously on load.
- **Forceable backend mode**
  - You can explicitly set `Backend Auto`, `Backend Ison`, or `Backend Monitor`.
- **MONITOR support**
  - Subscribes to the primary nick, reacts to server online/offline notifications when available, remembers when the nick is known free, and retries locally on the normal scheduler cadence without periodic `MONITOR S` polling.
- **ISON fallback**
  - Preserves the original single-nick `ISON` reclaim path on networks that do not offer `MONITOR`.
- **Run only when needed**
  - If you already have the preferred nick, it does not send reclaim traffic.
- **Idle-aware behavior**
  - If you were just active, the next scheduled backend tick can be skipped.
- **Join-safe startup**
  - The first backend action is delayed after connect. On a fresh connect the full `StartDelay` applies. On a hot reload via `updatemod` into an already-live connection, a single `IntervalSec` tick is used instead — the connection is already stable so the full join-safe delay is unnecessary.
- **Deduped reclaim attempts**
  - Prevents repeated `NICK <primary>` attempts from being sent too close together.
- **Optional tick jitter**
  - Adds a random `0..J` second delay to each scheduled backend tick.
- **Persisted configuration**
  - Settings are stored with ZNC NV storage.
- **Reactive reclaim on visible nick loss**
  - When the module sees a visible `NICK` or `QUIT` freeing the primary nick, it immediately attempts `NICK <Primary>` and ensures the backend loop re-arms if that attempt is rejected.
- **Low-overhead raw line filtering**
  - `OnRaw` performs a cheap single-token extraction to pre-filter every incoming IRC line. A full vector split is only performed for the nine command types the module actually handles. All other traffic (PRIVMSG, NOTICE, JOIN, PART, MODE, etc.) is rejected before any heap allocation occurs.
- **Client-transparent 433 and 303 swallow**
  - `433 :Nickname is already in use` responses generated by the module's own `NICK <Primary>` attempts are intercepted before reaching the IRC client. This prevents client-side scripts (such as mIRC Peace & Protection) from seeing module-generated failures and activating their own competing retake logic. `433` responses from manual nick changes or any nick other than `Primary` are always passed through unchanged.
  - `303` ISON poll responses generated by the module's own `ISON <Primary>` queries are intercepted before reaching the client, preventing them from appearing as noise in the IRC client status window. Manual `/ison` commands typed by the user are always passed through unchanged. In both cases the swallow happens after reclaim logic has fully run.
- **Robust backend scheduling**
  - Backend timers are force-rearmable and use unique labels so hot-swaps, `Poke`, and reactive retries do not silently stall the reclaim loop.
- **Manual poke command**
  - Forces an immediate backend check now instead of merely requesting the next scheduled one.

---

## Default behavior

The module source currently identifies itself as version **1.6.4** and defaults to:

- `Backend = Auto`
- `Interval = 5s`
- `IdleGap = 2s`
- `StartDelay = 90s`
- `Jitter = 0s`
- `MinGap = 3s`
- `Enabled = true`

These defaults reflect a cautious, Undernet-friendly balance:

- fast enough to reclaim a nick quickly,
- conservative enough to avoid needless churn during connect and join activity,
- simple enough to reason about when debugging,
- backward-compatible with the original ISON behavior on networks that do not advertise `MONITOR`.

---

## Backend model

### `Backend Auto`

This is the default and recommended mode.

Behavior:

- if the server advertises `MONITOR` in `005` / ISUPPORT, the module uses `MONITOR`,
- if the module is hot-swapped onto an already-connected session and has not seen `005`, it can send a one-time live `MONITOR` probe to detect support immediately,
- otherwise it uses the original `ISON` path,
- if `MONITOR` is advertised or live-detected but cannot be used for this target on the current connection, the module falls back to `ISON`,
- once `MONITOR` is active and the server has reported the nick offline, the module remembers that state locally and uses its own scheduler cadence for retry attempts instead of issuing periodic `MONITOR S` refreshes.

This gives you the best compatibility without needing per-network hardcoding.

### `Backend Ison`

Forces the module to use the original `ISON` reclaim behavior even if the server advertises `MONITOR`.

This is mainly useful if:

- you want fully predictable legacy behavior,
- you are troubleshooting,
- you want to keep the exact original strategy on a specific network.

### `Backend Monitor`

Requests the `MONITOR` path when possible.

In practice, the module still only uses `MONITOR` when the current server connection actually advertises it or the module has successfully live-detected it on an already-connected session, and the subscription is usable. If not, the effective behavior falls back to `ISON` so the module still functions.

---

## How it works at a high level

1. ZNC connects to the IRC network, or the module is hot-swapped onto an already-connected network.
2. On a normal connection, the module waits `StartDelay` seconds before the first backend action.
3. On a hot-swapped already-live connection, it may send a one-time live `MONITOR` probe immediately so it can detect support without waiting for a reconnect.
4. If you already own the preferred nick, the module stays passive apart from its internal timer cycle.
5. If you lose the preferred nick through a visible `NICK` or `QUIT` event, the module immediately calls `TryReclaim()`. If that attempt is rejected, `CRearmTimer` ensures the backend scheduler re-arms so the loop continues.
6. If you do **not** own the preferred nick:
   - with `MONITOR`, it subscribes to that nick, reacts to server notifications, and if the server has reported the nick offline it retries locally on later backend ticks without `MONITOR S` polling,
   - with `ISON`, it periodically sends:

   ```irc
   ISON <PrimaryNick>
   ```

7. When the module determines the nick is no longer in use, it sends:

   ```irc
   NICK <PrimaryNick>
   ```

8. It continues its low-noise scheduler until you successfully recover the nick.

It also reacts opportunistically to visible `NICK` and `QUIT` messages for the target nick when those are visible to you through shared channels, which can let it attempt recovery sooner than the next scheduled backend action.

---

## Build and installation

### Build the module

From the directory containing the source file:

```bash
znc-buildmod keepnick_instant.cpp
```

This should produce a loadable ZNC module shared object for your environment.

### Install the built module

Copy or move the resulting module file into your ZNC modules directory, typically the per-user modules path or another path appropriate for your setup.

Example workflows vary by installation style, but commonly this means placing the built `.so` where ZNC can load user/network modules.

### Load the module

This is a **network module**, so load it on the specific IRC network where you want nickname reclaim behavior.

Typical examples from an IRC client connected to ZNC:

```irc
/msg *status LoadMod keepnick_instant
```

Or with an explicit primary nick argument:

```irc
/msg *status LoadMod keepnick_instant MyPrimaryNick
```

If no argument is provided, the module determines the initial primary nick as follows:

1. stored `PrimaryNick` from NV storage, if present
2. the current network nick
3. the ZNC user nick
4. fallback string `ZNCUser`

---

## Usage

Once loaded, talk to the module in the usual ZNC module-command style.

Depending on your client and setup, that typically means one of these forms:

```irc
/msg *keepnick_instant help
```

or:

```irc
/msg *keepnick_instant show
```

If your client/setup uses a different module command format, adapt accordingly.

---

## Command reference

### `Help` / `Show`

Displays current configuration, state, defaults, and available commands.

Example:

```irc
/msg *keepnick_instant Show
```

`Show` now includes backend-related state such as:

- configured backend mode,
- effective backend currently in use,
- whether `MONITOR` was advertised by the server,
- whether a `MONITOR` subscription is currently active,
- optional server-advertised `MONITOR` limit if present.

### `Enable`

Enables scheduled backend checks and reclaim behavior.

```irc
/msg *keepnick_instant Enable
```

### `Disable`

Disables backend checks and reclaim behavior.

```irc
/msg *keepnick_instant Disable
```

If the module is actively using `MONITOR`, it removes its subscription when disabled.

### `SetNick <nick>`

Sets and persists the primary nickname the module should try to reclaim.

```irc
/msg *keepnick_instant SetNick MyNick
```

If the active backend is `MONITOR`, the module updates the monitored target accordingly.

### `Backend <Auto|Ison|Monitor>`

Sets the backend mode.

```irc
/msg *keepnick_instant Backend Auto
/msg *keepnick_instant Backend Ison
/msg *keepnick_instant Backend Monitor
```

Recommended setting:

```irc
/msg *keepnick_instant Backend Auto
```

### `Interval <5-300>`

Sets the poll interval, in seconds, used while the **effective backend is `ISON`** and you do **not** own the primary nick.

```irc
/msg *keepnick_instant Interval 5
```

With the `MONITOR` backend, `Interval` still governs the module's lightweight internal scheduler, but it is no longer the thing that discovers nick availability.

### `IdleGap <0-30>`

If you sent any outbound line within the last `IdleGap` seconds, the next scheduled backend tick is skipped and rescheduled.

```irc
/msg *keepnick_instant IdleGap 2
```

### `StartDelay <0-600>`

Sets the startup delay before the first backend action after connect.

```irc
/msg *keepnick_instant StartDelay 90
```

Note: the new value becomes relevant on the next connect cycle.

### `Jitter <0-10>`

Adds a random `0..J` second delay to each subsequent backend tick schedule.

```irc
/msg *keepnick_instant Jitter 0
```

### `MinGap <0-30>`

Sets the minimum spacing between actual `NICK` reclaim attempts.

```irc
/msg *keepnick_instant MinGap 3
```

### `Poke`

Forces an immediate backend check now.

```irc
/msg *keepnick_instant Poke
```

This is useful when you want to force a fresh backend action without waiting for the normal schedule, and in 1.5.1+ it is designed to bypass a pending backend timer rather than silently deferring to it.

---

## Recommended settings

### Recommended baseline for Undernet

The source defaults are already a sensible baseline for Undernet-style usage:

- `Backend Auto`
- `Interval 5`
- `IdleGap 2`
- `StartDelay 90`
- `Jitter 0`
- `MinGap 3`

On Undernet-like networks, `Backend Auto` will typically behave the same as the original module because the server does not normally advertise `MONITOR`.

### Recommended baseline for mixed-network use

If you use the same module design on different IRC networks or daemon families, the recommended baseline is still:

- `Backend Auto`
- `Interval 5`
- `IdleGap 2`
- `StartDelay 90`
- `Jitter 0`
- `MinGap 3`

That lets the module use `MONITOR` where available without sacrificing compatibility elsewhere.

### When to increase `StartDelay`

Increase it if:

- your connect burst is already busy,
- you auto-join many channels,
- the network is sensitive to early post-connect traffic,
- you want the reclaim logic to stay completely out of the way during startup.

### When to increase `Interval`

Increase it if:

- you want even more conservative behavior,
- you do not care about reclaiming the nick within a few seconds,
- you are tuning for especially strict network etiquette,
- you are mostly using the `ISON` backend and want fewer checks.

### When to use `Jitter`

Enable a small jitter if:

- you run multiple clients/bouncers with similar behavior,
- you want to avoid perfectly regular scheduler cadence,
- you want timing to look less mechanically synchronized.

---

## Example sessions

### Example A: ISON path

Suppose your preferred nick is `Alice`, but on connect you end up as `Alice_` because `Alice` is in use.

1. The module waits `StartDelay` seconds.
2. The server does not advertise `MONITOR`, so `Backend Auto` resolves to `ISON`.
3. The module begins sending one-shot checks like:

   ```irc
   ISON Alice
   ```

4. As long as the server reports `Alice` is present, nothing happens beyond rescheduling.
5. Once `Alice` disappears from `ISON`, the module sends:

   ```irc
   NICK Alice
   ```

6. You recover the nick, and future reclaim behavior becomes effectively dormant because you now own the primary nick.

### Example B: MONITOR path

Suppose the same nick is in use, but the server advertises `MONITOR`.

1. The module waits `StartDelay` seconds.
2. `Backend Auto` resolves to `MONITOR`.
3. The module sends:

   ```irc
   MONITOR + Alice
   ```

4. While `Alice` is still online, the server can report that state using `MONITOR` numerics.
5. Once the server reports `Alice` offline, the module sends:

   ```irc
   NICK Alice
   ```

6. After you have the nick back, the module removes its `MONITOR` subscription on a later scheduler pass because it no longer needs to watch the nick.

---

## Implementation details

This section documents the code-level behavior in more detail for maintainers and reviewers.

### Module type

The source declares:

```cpp
NETWORKMODULEDEFS(CKeepNickInstant, "Auto-backend keepnick (MONITOR/ISON instant-ish)")
```

So this is a **network module**, not a global or user module.

### Persistent configuration

The module stores configuration through ZNC NV storage using these keys:

- `Enabled`
- `BackendMode`
- `IntervalSec`
- `IdleGapSec`
- `StartDelaySec`
- `JitterSec`
- `MinGapSec`
- `PrimaryNick`

On load, it reads stored values if present and validates them against hard-coded bounds.

### Primary nick resolution

The target nick is established by `LoadPrimary()`:

- explicit module argument wins,
- otherwise stored `PrimaryNick`,
- otherwise the network nick,
- otherwise the user nick,
- otherwise `ZNCUser`.

This gives the module a safe startup path even if no explicit nick was configured.

### Timer model

The module uses two single-shot timer classes:

- `CISONOnceTimer`
- `CRearmTimer`

`CISONOnceTimer` now acts as the general **backend tick** timer.

- If the effective backend is `ISON`, it sends the single-nick `ISON` check.
- If the effective backend is `MONITOR`, it either establishes the subscription or, if a confirmed subscription is already active and the nick is locally known free, retries `NICK <PrimaryNick>` on the normal scheduler cadence.

Backend timers are armed with unique labels, and forced actions such as `Poke` or reactive recovery can replace a pending backend timer cleanly. This avoids the earlier failure mode where a pending timer could keep an immediate backend check from actually being scheduled.

`CRearmTimer` fires after a reactive reclaim attempt (`TryReclaim()` triggered by a visible `NICK` or `QUIT` event). If the reclaim was rejected — for example because someone else grabbed the nick in the same moment — it force-rearms the backend scheduler so the module does not go silent waiting for the next coincidental tick.

The module does **not** keep a permanently repeating timer object. Instead, it re-arms one-shot backend ticks, which makes scheduling behavior explicit and easier to control.

### Backend detection

The module does **not** try to identify networks or daemons by name.

Instead, it watches `005` / ISUPPORT for the `MONITOR` token.

If the module is loaded or hot-swapped onto an already-connected session and has not seen `005`, it can also send a one-time live `MONITOR + <PrimaryNick>` probe. A working `MONITOR` response effectively confirms support on that live connection; an unsupported or unusable response falls back to `ISON`.

That means behavior is selected by what the current server connection actually advertises or demonstrably supports, not by hardcoded assumptions.

### MONITOR path

When the effective backend is `MONITOR`, the module uses:

```irc
MONITOR + <PrimaryNick>
```

The module tracks four pieces of `MONITOR` state:

- whether `MONITOR` has been advertised or otherwise detected for the current connection,
- whether the current connection can use it for this target,
- whether the module currently has an active, **server-confirmed** subscription,
- whether a one-time live probe has already been sent on a hot-swapped already-live session.

`MonitorActive` is only set to `true` when the server has confirmed the subscription via a `730` or `731` response. It is deliberately **not** set at probe-send time, which prevents other code paths from treating an unconfirmed probe as an active subscription. This matters on servers that silently drop unknown commands — without this guard, the module could get stuck believing `MONITOR` was working when no subscription was ever established.

When it receives the offline numeric for the primary nick, it immediately attempts reclaim and remembers that the nick is locally known free. If reclaim is still needed later and the `MONITOR` subscription remains active, scheduled backend ticks use that remembered offline state to retry `NICK <PrimaryNick>` locally without sending periodic `MONITOR S` refreshes.

If the server reports that the monitor list is full or unusable for the target, the module marks `MONITOR` unusable for the current connection and falls back to `ISON`.

If the module probes `MONITOR` on a hot-swapped already-live session and the server answers with an unknown-command style failure, it marks `MONITOR` unusable for that connection and continues with `ISON`.

### ISON path

When the effective backend is `ISON`, the module uses a **single-nick `ISON`** request:

```irc
ISON <PrimaryNick>
```

That keeps the logic straightforward:

- if the nick appears in `303`, it is still in use,
- if it does not appear, attempt reclaim.

This is the original behavior of the module and remains the fallback path.

### Idle-aware behavior

The module tracks outbound client activity via:

```cpp
EModRet OnUserRaw(CString& /*line*/) override
```

Every user-sent line updates `LastUserActivity`.

When the backend timer fires, the module checks whether the last user activity was within `IdleGapSec`. If yes, it skips this backend tick and reschedules.

The `Poke` command intentionally bypasses this idle suppression so it behaves immediately regardless of recent activity.

This is a subtle but important design decision: it helps keep ordinary reclaim checks from interleaving too tightly with your own manual commands.

### Join-safe startup

On both initial load and `OnIRCConnected()`, the module arms the first backend action with `StartDelaySec`.

The one exception is hot-swapped already-live reloads: in that case, the module may send a one-time live `MONITOR` probe immediately so backend detection can become accurate without waiting for the server to replay registration numerics. That probe is for backend discovery, not because the normal join-safe scheduler changed. Outside that hot-reload discovery case, the join-safe post-connect delay remains intact.

That means the module intentionally stays quiet during the early phase after connection, which is typically the busiest period for:

- CAP negotiation aftermath,
- network settle-in,
- auto-joins,
- channel bursts,
- client restore behavior.

This makes the module friendlier to conservative networks and reduces the chance that nick recovery logic contributes to startup noise.

### Reclaim dedupe / rate spacing

Before sending `NICK <Primary>`, the module checks `LastAttempt` against `MinGapSec`.

If a reclaim was attempted too recently, it does nothing.

This avoids duplicate back-to-back nick changes in edge cases such as:

- repeated timer firings,
- multiple visible events implying availability,
- clustered server responses,
- closely spaced backend notifications.

### Opportunistic event handling

In addition to `303` processing and `MONITOR` numerics, `OnRaw()` watches visible raw traffic for (after a cheap pre-filter that skips all other line types without allocating):

- `NICK`
- `QUIT`

If the nick currently held by someone else changes away from the primary nick, or if that nick quits and the event is visible to you, the module attempts reclaim immediately via `TryReclaim()`. The existing `CRearmTimer` then ensures the backend scheduler re-arms if that reclaim attempt was rejected, so the loop continues without waiting for the next coincidental tick.

This can reduce the time-to-reclaim compared with waiting for the next scheduled backend action.

### Case-insensitive nick comparisons

Nick comparisons use `Equals(..., false)`, so behavior is case-insensitive in the ZNC/CString sense used by the module.

### Random jitter

The module seeds `std::rand()` on load and computes an added delay in the range `0..JitterSec` for re-armed backend ticks.

This is intentionally simple. It is not trying to provide cryptographic randomness; it only needs enough variation to avoid rigid timer alignment.

---

## Safety and behavioral philosophy

This module aims to be **conservative rather than aggressive**.

That philosophy shows up in several places:

- it does not spam `NICK` attempts blindly,
- it defers its first backend action after connect,
- it backs off around your own activity,
- it only runs reclaim logic while reclaim is actually needed,
- it rate-limits actual nick attempts,
- it uses `MONITOR` only when the server explicitly advertises it, or when a one-time live probe confirms it on an already-connected hot-swapped session (deferred to the first backend tick, not fired synchronously on load).

This makes it suitable for users who want reliable reclaim behavior without a noisy “fight for the nick” pattern.

---

## Limitations and scope

- This module does **not** authenticate to nickname services.
- It is not a replacement for NickServ-style account workflows.
- It assumes reclaiming the nick is valid on the target network once that nick is free.
- `NICK`/`QUIT` opportunism only works when those events are visible to you.
- `MONITOR` is only used when the current server connection advertises it or when a hot-swapped already-live session confirms it with a one-time probe. On a hot reload, that probe happens at the first backend tick after `IntervalSec` and fires unconditionally — even if you already own your primary nick — so MONITOR support is established before it is ever needed.
- The “instant” part is still bounded by startup delay, idle suppression, rate spacing, server behavior, and network visibility.

Those trade-offs are intentional.

One deliberate side effect of the `433` and `303` swallows: client-side scripts that monitor for `433` to display a "nick in use" notification will not see failures caused by the module's own reclaim attempts, and `303` ISON replies from the module's own polls will not appear in the client status window. Both are intentional — those responses are noise when ZNC is handling reclaim automatically. Manual nick changes and manual `/ison` commands are always unaffected.

---

## Why this is a good fit for Undernet-style use

Undernet and similar networks are exactly the sort of environment where a module like this makes sense:

- there may be no modern services-backed nickname reservation workflow to solve the problem for you,
- simply switching back to the nick is often the correct action once it becomes available,
- users may still want that reclaim behavior to be measured and network-friendly,
- `Backend Auto` naturally preserves the original `ISON` path where `MONITOR` is not offered, while still being able to detect `MONITOR` correctly after a hot-swapped reload on networks that do support it.

That is why this module was primarily shaped around **Undernet-friendly** behavior.

---

## Troubleshooting

### The module does not reclaim my nick

Check:

- the module is loaded on the correct network,
- `Show` reports the expected `Primary` nick,
- the module is `ENABLED`,
- you are not already on the primary nick,
- the target nick is actually becoming free,
- your current timing values are not too conservative for your expectations,
- `Show` reports the backend state you expect.

### It is using ISON when I expected MONITOR

Check:

- whether `Show` says `MONITOR advertised/detected: yes`,
- whether the server actually advertised `MONITOR` in `005` or the module live-detected it after load,
- whether `Backend` is set to `Ison`,
- whether the current connection marked `MONITOR` unusable and fell back to `ISON`,
- whether the session was hot-swapped and the live probe has already run.

### MONITOR is detected, but `Active` stays `no`

In 1.6.2 and later, on a hot reload into an already-live session, `Active` should flip to `yes` shortly after reload once the server responds to the one-time detection probe. On a normal connect, `Active` flips to `yes` once the server confirms the subscription via a `730` or `731` numeric after `StartDelaySec`. It will not flip immediately when the command is sent in either case.

If it does not, try `Poke` once and check again. `Poke` is now a true force-run path and should not be blocked by a pending backend timer.

### It feels too slow

Try:

- lowering `StartDelay`,
- keeping `Interval` at `5`,
- ensuring `IdleGap` is not suppressing backend ticks during heavy manual use.

### It feels too aggressive

Try:

- increasing `StartDelay`,
- increasing `Interval`,
- increasing `MinGap`,
- adding a little `Jitter`.

### I use a services-heavy network

This module may not be the right solution model for that network. A services/account-based approach is usually more appropriate there.

---

## Source summary

From the source header:

- **Name:** `keepnick_instant`
- **Version:** `1.6.4`
- **Description:** Auto-backend, idle-aware, join-safe nick reclaim for ZNC (MONITOR when available, otherwise ISON; OnRaw pre-filter, consistent MONITOR subscription guards, hot-reload MONITOR detect, 433/303 swallow)

---

## License

Add the license appropriate for your repository here.

If you intend to publish this module publicly, it is a good idea to include an explicit license file in the repository root.
