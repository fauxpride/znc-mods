# keepnick_instant

[`keepnick_instant`](./src/keepnick_instant.cpp) is a lightweight ZNC network module that tries to reclaim your preferred nickname as soon as it becomes available again, while still being conservative enough to avoid noisy or risky behavior on IRC networks.

It is designed for networks where nicknames are **not reserved by channel services / nickname services**, so simply switching back to your preferred nick is enough when that nick becomes free. In practice, that makes it especially useful on networks such as **Undernet**, which is the primary target this module was tuned around.

In other words: this module exists for the classic IRC case where your preferred nick may be temporarily occupied, you connect under an alternate nick, and you want ZNC to quietly and safely switch back the moment the preferred nick becomes available.

Compared with ZNC's built-in `keepnick` module, `keepnick_instant` is deliberately more cautious and more explicit about *when* it will try to reclaim. Stock `keepnick` periodically attempts a direct `NICK` back to the network's configured nick and also reacts to visible `NICK`/`QUIT` events for that nick, while `keepnick_instant` adds `ISON`-based availability checks, post-connect delay, idle-awareness, optional jitter, and deduped reclaim spacing. That makes the stock module a fine generic default, but makes `keepnick_instant` a better fit for Undernet-style networks where you want quick reclaim once the nick is truly free without adding unnecessary nick-change traffic.

---

## What this module is for

When ZNC connects to a network and your primary nick is already in use, the server will typically assign you an alternate nick. On networks without services-based nick ownership, the only thing preventing you from retaking your preferred nick is whether someone else is currently using it.

`keepnick_instant` automates that recovery path.

It does so with a deliberately cautious design:

- It polls using `ISON` instead of hammering nick changes blindly.
- It only polls while you **do not** currently own the target nick.
- It waits after connect before it begins checking, so it does not interfere with initial connect/join phases.
- It avoids polling immediately after you send commands, reducing the chance of clashing with your own activity.
- It spaces out actual `NICK` attempts so duplicate reclaims are not sent too aggressively.
- It can optionally add jitter so multiple clients or bouncers do not fall into the same polling rhythm.

The end result is an “instant-ish” nick reclaim module: responsive enough to recover your nick quickly, but careful enough to behave well on networks that are sensitive to bursts of activity.

---

## Intended environment

This module is intended for:

- IRC networks that **do not** have channel/nick services reserving nicks in the modern NickServ sense
- users who want ZNC to recover a preferred nick automatically
- environments where conservative, low-noise polling is preferable to aggressive reclaim logic

This module is **mainly designed to work for Undernet** and similar networks.

It is **not primarily aimed at** networks where nickname ownership is enforced by services and authentication workflows. On those networks, the correct solution is usually services identification and/or network-native account mechanisms rather than repeated nick reclaim attempts.

---

## How this differs from ZNC's built-in `keepnick`

ZNC already ships with a core module called `keepnick`, so it is reasonable to ask why this module exists at all. The short version is: `keepnick_instant` is not trying to reinvent the goal, but to change the *strategy* used to reach it.

### What stock `keepnick` does

ZNC's built-in `keepnick` is a small, generic reclaim module. In current ZNC source, it:

- tries to regain the network's configured nick with a repeating 30-second timer,
- also reacts quickly when it sees a visible `NICK` or `QUIT` for the target nick,
- disables itself in some cases where continuing would be counterproductive, such as specific nickname-change errors or when you intentionally change away from the configured nick.

That behavior is straightforward and useful, and it is one reason `keepnick` has been a long-standing core module.

### What `keepnick_instant` changes

`keepnick_instant` keeps the same overall objective, but changes the operational model in several important ways:

- it uses `ISON` to check whether the preferred nick is currently present before sending `NICK`,
- it only polls while you *do not* already own the preferred nick,
- it waits after connect before starting, so reclaim logic stays out of the initial connect/join burst,
- it can skip a poll if you were just active,
- it dedupes actual reclaim attempts with `MinGap`,
- it can add per-poll jitter to avoid perfectly regular timing.

In practice, that makes this module feel more deliberate, less spammy, and easier to tune for networks where being conservative matters.

### Why they do not fundamentally conflict

When both modules are aimed at the **same preferred nick**, they are not logically fighting each other: they both converge on the same end state, namely “you got your preferred nick back.” They do not share mutable state, do not patch or disable one another, and neither module depends on private behavior from the other.

The overlap is mostly limited to the possibility that both may send a `NICK` attempt around roughly the same time. That is usually just redundant, not adversarial. Once the nick is successfully reclaimed, `keepnick_instant` stops polling because you already own the target nick, and the stock module has nothing useful left to do either.

That said, loading both at once is usually unnecessary. In most real setups, `keepnick_instant` should be treated as a more controlled **alternative** to stock `keepnick`, not as something that needs to be layered on top of it.

### Important caveat

The “no conflict” statement assumes both modules are trying to reclaim the **same** nick. If you intentionally configure ZNC's built-in `keepnick` to chase the network-configured nick while `keepnick_instant` is configured with a *different* `PrimaryNick`, then you have created a real policy conflict yourself: each module will be trying to converge on a different result.

For the intended use case, keep them aligned — or just run `keepnick_instant` by itself.

---

## Feature summary

- **ISON-only polling**
  - Uses `ISON <nick>` to check whether the preferred nick is currently online.
- **Poll only when needed**
  - If you already have the preferred nick, it does not keep polling to no purpose.
- **Idle-aware behavior**
  - If you were just active, the next poll can be skipped.
- **Join-safe startup**
  - The first poll is delayed after connect.
- **Deduped reclaim attempts**
  - Prevents repeated `NICK <primary>` attempts from being sent too close together.
- **Optional poll jitter**
  - Adds a random `0..J` second delay to each scheduled poll.
- **Persisted configuration**
  - Settings are stored with ZNC NV storage.
- **Manual poke command**
  - Lets you trigger an immediate single `ISON` check.

---

## Default behavior

The module source currently identifies itself as version **1.2.0** and defaults to:

- `Interval = 5s`
- `IdleGap = 2s`
- `StartDelay = 90s`
- `Jitter = 0s`
- `MinGap = 3s`
- `Enabled = true`

These defaults reflect a cautious, Undernet-friendly balance:

- fast enough to reclaim a nick quickly,
- conservative enough to avoid needless churn during connect and join activity,
- simple enough to reason about when debugging.

---

## How it works at a high level

1. ZNC connects to the IRC network.
2. The module waits `StartDelay` seconds before the first check.
3. If you already own the preferred nick, the module stays passive except for its timer cycle.
4. If you do **not** own the preferred nick, it periodically sends:

   ```irc
   ISON <PrimaryNick>
   ```

5. If the nick is not present in the `303` reply, the module sends:

   ```irc
   NICK <PrimaryNick>
   ```

6. It continues polling on its timer schedule until you successfully recover the nick.

It also reacts opportunistically to visible `NICK` and `QUIT` messages for the target nick when those are visible to you through shared channels, which can let it attempt recovery sooner than the next poll.

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

### `Enable`

Enables polling and reclaim behavior.

```irc
/msg *keepnick_instant Enable
```

### `Disable`

Disables polling and reclaim behavior.

```irc
/msg *keepnick_instant Disable
```

### `SetNick <nick>`

Sets and persists the primary nickname the module should try to reclaim.

```irc
/msg *keepnick_instant SetNick MyNick
```

### `Interval <5-300>`

Sets the poll interval, in seconds, used while you do **not** own the primary nick.

```irc
/msg *keepnick_instant Interval 5
```

### `IdleGap <0-30>`

If you sent any outbound line within the last `IdleGap` seconds, the next poll is skipped and rescheduled.

```irc
/msg *keepnick_instant IdleGap 2
```

### `StartDelay <0-600>`

Sets the startup delay before the first `ISON` after connect.

```irc
/msg *keepnick_instant StartDelay 90
```

Note: the new value becomes relevant on the next connect cycle.

### `Jitter <0-10>`

Adds a random `0..J` second delay to each subsequent poll schedule.

```irc
/msg *keepnick_instant Jitter 0
```

### `MinGap <0-30>`

Sets the minimum spacing between actual `NICK` reclaim attempts.

```irc
/msg *keepnick_instant MinGap 3
```

### `Poke`

Schedules an immediate one-shot `ISON` check.

```irc
/msg *keepnick_instant Poke
```

This is useful when you want to force a fresh availability check without waiting for the normal timer.

---

## Recommended settings

### Recommended baseline for Undernet

The source defaults are already a sensible baseline for Undernet-style usage:

- `Interval 5`
- `IdleGap 2`
- `StartDelay 90`
- `Jitter 0`
- `MinGap 3`

These settings keep the module responsive without making it too eager around connect time.

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
- you are tuning for especially strict network etiquette.

### When to use `Jitter`

Enable a small jitter if:

- you run multiple clients/bouncers with similar behavior,
- you want to avoid perfectly regular polling cadence,
- you want timing to look less mechanically synchronized.

---

## Example session

Suppose your preferred nick is `Alice`, but on connect you end up as `Alice_` because `Alice` is in use.

1. The module waits `StartDelay` seconds.
2. It begins sending one-shot checks like:

   ```irc
   ISON Alice
   ```

3. As long as the server reports `Alice` is present, nothing happens beyond rescheduling.
4. Once `Alice` disappears from `ISON`, the module sends:

   ```irc
   NICK Alice
   ```

5. You recover the nick, and future polling becomes effectively dormant because you now own the primary nick.

---

## Implementation details

This section documents the code-level behavior in more detail for maintainers and reviewers.

### Module type

The source declares:

```cpp
NETWORKMODULEDEFS(CKeepNickInstant, "ISON-only keepnick (instant-ish)")
```

So this is a **network module**, not a global or user module.

### Persistent configuration

The module stores configuration through ZNC NV storage using these keys:

- `Enabled`
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

`CISONOnceTimer` performs the actual scheduled `ISON` logic.

`CRearmTimer` acts as a lightweight spacing guard after a reclaim attempt. It does not itself send anything; it exists as a harmless dedupe spacing mechanism around `MinGap` handling.

The module does **not** keep a permanently repeating timer object. Instead, it re-arms one-shot checks, which makes scheduling behavior explicit and easier to control.

### Why `ISON`

`ISON` is a simple and low-cost IRC primitive for checking whether a nick is online.

This module uses a **single-nick `ISON`** request:

```irc
ISON <PrimaryNick>
```

That keeps the logic straightforward:

- if the nick appears in `303`, it is still in use,
- if it does not appear, attempt reclaim.

### Idle-aware behavior

The module tracks outbound client activity via:

```cpp
EModRet OnUserRaw(CString& /*line*/) override
```

Every user-sent line updates `LastUserActivity`.

When the `ISON` timer fires, the module checks whether the last user activity was within `IdleGapSec`. If yes, it skips this poll and reschedules.

This is a subtle but important design decision: it helps keep reclaim checks from interleaving too tightly with your own manual commands.

### Join-safe startup

On both initial load and `OnIRCConnected()`, the module arms the first `ISON` with `StartDelaySec`.

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
- clustered server responses.

### Opportunistic event handling

In addition to `303` processing, `OnRaw()` watches visible raw traffic for:

- `NICK`
- `QUIT`

If the nick currently held by someone else changes away from the primary nick, or if that nick quits and the event is visible to you, the module may attempt reclaim immediately.

This can reduce the time-to-reclaim compared with waiting for the next `ISON` poll.

### Case-insensitive nick comparisons

Nick comparisons use `Equals(..., false)`, so behavior is case-insensitive in the ZNC/CString sense used by the module.

### Random jitter

The module seeds `std::rand()` on load and computes an added delay in the range `0..JitterSec` for re-armed polls.

This is intentionally simple. It is not trying to provide cryptographic randomness; it only needs enough variation to avoid rigid timer alignment.

---

## Safety and behavioral philosophy

This module aims to be **conservative rather than aggressive**.

That philosophy shows up in several places:

- it does not spam `NICK` attempts blindly,
- it defers its first poll after connect,
- it backs off around your own activity,
- it only polls while reclaim is actually needed,
- it rate-limits actual nick attempts.

This makes it suitable for users who want reliable reclaim behavior without a noisy “fight for the nick” pattern.

---

## Limitations and scope

- This module does **not** authenticate to nickname services.
- It is not a replacement for NickServ-style account workflows.
- It assumes reclaiming the nick is valid on the target network once that nick is free.
- `NICK`/`QUIT` opportunism only works when those events are visible to you.
- The “instant” part is bounded by poll interval, idle suppression, and network visibility.

Those trade-offs are intentional.

---

## Why this is a good fit for Undernet-style use

Undernet and similar networks are exactly the sort of environment where a module like this makes sense:

- there may be no modern services-backed nickname reservation workflow to solve the problem for you,
- simply switching back to the nick is often the correct action once it becomes available,
- users may still want that reclaim behavior to be measured and network-friendly.

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
- your current timing values are not too conservative for your expectations.

### It feels too slow

Try:

- lowering `StartDelay`,
- keeping `Interval` at `5`,
- ensuring `IdleGap` is not suppressing polls during heavy manual use.

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
- **Version:** `1.2.0`
- **Description:** ISON-only, idle-aware, join-safe nick reclaim for ZNC

---

## License

Add the license appropriate for your repository here.

If you intend to publish this module publicly, it is a good idea to include an explicit license file in the repository root.
