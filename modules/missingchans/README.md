# missingchans

[`missingchans`](./src/missingchans.cpp) is a ZNC network module that verifies whether the channels you **expect** to be on are actually joined on the **IRC server**, and can optionally retry joins for anything that is still missing.

It is designed for reconnect scenarios where your configured channel list and your real server-side membership can drift apart for a while after connect — especially on networks where joining certain community channels depends on first authenticating to a **non-service bot**, obtaining a **community-specific cloak**, or waiting for some other post-connect condition to complete.

With the proper configuration, this helps make sure your channels are eventually joined even if there is a **temporary server issue**, a **community bot outage**, or a race where the first burst of joins happens before the network is ready to admit you everywhere.

In practical terms, this module is useful when:

- some channels only become joinable **after** a custom auth bot grants access;
- you rely on a bot-driven cloak, vhost, or account state before certain channels will accept you;
- the initial post-connect join burst can fail partially during outages, lag, or service instability;
- you want ZNC to **verify** the final joined state from the server side instead of assuming the first attempt worked.

Although the module is generic, it is particularly aimed at IRC environments where **community bots besides the core channel service bots** are part of the login and channel-access flow.

---

## What problem this module solves

ZNC already remembers channels and can auto-join them, and many users also run ZNC's built-in `perform` module to send post-connect commands. That is often enough.

The trouble starts when channel access depends on additional timing-sensitive steps such as:

- authenticating to a network-specific or community-specific helper bot;
- waiting for a cloak or mode change before joining protected channels;
- reconnecting during a temporary outage where your initial joins only partially succeed;
- racing your auth flow against the first join attempts after reconnect.

In those cases, you can end up in a state where:

- ZNC thinks the network is connected;
- some channels did join;
- some channels did **not** join;
- the original join opportunity has passed;
- and unless you notice manually, you stay detached from part of your normal channel set.

`missingchans` adds a delayed verification pass and optional rejoin logic on top of that flow.

---

## High-level behavior

After the IRC connection is established, the module:

1. waits for a configurable initial delay;
2. builds the set of **expected** channels from your ZNC network channel list;
3. performs a self-`WHOIS` on your current nick;
4. parses the server's channel list for your nick;
5. compares **expected** vs **actually joined**;
6. if anything is missing, optionally schedules one or more delayed join attempts;
7. re-checks again after each attempt.

It can also optionally trigger ZNC's built-in `perform` module before a retry attempt, which is useful if your recovery flow depends on re-sending authentication or helper-bot commands before retrying the missing joins.

---

## Why this is useful for bot-gated channels and cloaks

Some IRC communities use access bots, helper bots, cloak bots, or other non-standard automation that must recognize you before you can fully rejoin your normal channel set.

Examples include situations where you must:

- message a bot after connect to identify yourself;
- wait for a custom cloak/vhost before joining invite-restricted or community-restricted channels;
- rely on a network/community automation step that can occasionally lag or fail during outages.

If that step fails temporarily, or happens too slowly relative to the first auto-join burst, the result can be a partial join state.

With the proper setup:

- `perform` can re-send the auth or bot-contact commands;
- `missingchans` can delay and retry joins afterward;
- server-side verification can confirm which channels are still missing;
- and channel keys stored in ZNC are reused automatically when retrying joins.

That combination is what makes the module suitable for “make sure everything gets joined eventually” workflows during transient outages.

---

## Features

- Delayed post-connect verification instead of checking immediately.
- Server-side channel verification via self-`WHOIS`.
- Optional automatic rejoin of missing channels.
- Optional multi-attempt retry logic with increasing delay.
- Optional integration with ZNC's built-in `perform` module via `perform Execute`.
- Optional `StopPerformOn` sentinel channel to suppress further `perform` retries once a key channel is confirmed joined.
- Reuses channel keys from ZNC channel configuration when re-sending `JOIN`.
- Case-insensitive channel comparison.
- Extra fallback learning from numeric `443` (“already on channel”).
- Persistent configuration via ZNC module NV storage.

---

## Module identity

- **Module name:** `missingchans`
- **Source file:** `missingchans.cpp`
- **Current build marker in source:** `2026-02-11+r6 (robust 319 + case-insensitive chans + 443 fallback)`

The module advertises itself as:

> Verify/join missing channels by comparing expected list vs WHOIS (with retry + perform support).

---

## Installation

### Build

```bash
znc-buildmod missingchansv6.cpp
```

This should produce a module shared object suitable for your ZNC installation.

### Load

From a connected IRC client attached to the target network:

```text
/msg *status LoadMod missingchans
```

Or, if loading as a network module through the web interface or your usual module workflow, load it on the specific network where you want this behavior.

On load, the module restores its saved settings from NV storage.

---

## Default settings

The source defaults are:

- `Delay = 300` seconds
- `JoinMissing = off`
- `ExpectedMode = all`
- `RetryPerform = off`
- `Retries = 3`
- `RetryStep = 20` seconds
- `StopPerformOn = off`

These defaults are intentionally conservative:

- verification is delayed long enough for many post-connect flows to settle;
- auto-joining is disabled by default so you can observe behavior first;
- retrying `perform` is also disabled by default.

---

## Commands

All module commands are issued through the module query window or via `/msg` to the module.

Examples below assume the module window is `*missingchans`.

### Help

```text
/msg *missingchans HELP
```

Shows built-in help and the main setting names.

### Version

```text
/msg *missingchans VERSION
```

Prints the module build marker.

### Status

```text
/msg *missingchans STATUS
```

Shows the current configuration and some run-state details such as whether `perform` is currently suppressed for the active cycle.

### Show expected channels

```text
/msg *missingchans SHOW
```

Builds and displays the expected-channel set according to the current `ExpectedMode`.

### Run a verification immediately

```text
/msg *missingchans RUN
```

Forces an immediate verification cycle instead of waiting for the next automatic delayed run.

### Change settings

```text
/msg *missingchans SET <key> <value>
```

Supported keys:

- `delay`
- `joinmissing`
- `expectedmode`
- `retryperform`
- `retries`
- `retrystep`
- `stopperformon`

---

## Settings reference

### `SET delay <seconds>`

Controls how long the module waits **after IRC connection** before starting the verification cycle.

Example:

```text
/msg *missingchans SET delay 180
```

Use a longer delay when your post-connect flow depends on slow bot responses, cloaks, or other network-side processing.

---

### `SET joinmissing <on|off>`

Enables or disables automatic retry-join behavior for channels found to be missing.

Example:

```text
/msg *missingchans SET joinmissing on
```

When off, the module will still detect and report missing channels, but it will not attempt to fix them.

---

### `SET expectedmode <all|config|enabled>`

Controls how the module builds its **expected** channel set from ZNC's configured channel list.

#### `all`
Includes every channel known to the network object.

#### `config`
Includes only channels that are present in ZNC's saved config.

#### `enabled`
Includes only channels that are both:

- in config, and
- not disabled.

Example:

```text
/msg *missingchans SET expectedmode enabled
```

For most real-world use, `enabled` is the safest and most intuitive setting if you keep disabled channels in your config.

---

### `SET retryperform <on|off>`

If enabled, the module will try to locate ZNC's built-in `perform` module and call:

```text
Execute
```

just before a retry join attempt.

Example:

```text
/msg *missingchans SET retryperform on
```

This is especially useful when your reconnect workflow requires re-sending:

- auth messages to a helper bot;
- cloak requests;
- timing-sensitive setup commands;
- or any post-connect commands that should happen again before retrying missing joins.

The module looks for `perform` first at the **network** level, then at the **user** level.

If no `perform` module is loaded, it reports that and continues without it.

---

### `SET retries <N>`

Sets the maximum number of join-attempt rounds.

Example:

```text
/msg *missingchans SET retries 5
```

Each attempt can optionally re-run `perform` and then sends `JOIN` for all channels still considered missing.

---

### `SET retrystep <seconds>`

Controls the base retry spacing.

Attempt `i` waits:

```text
i * retrystep
```

seconds.

So with `retrystep = 20`:

- attempt 1 runs after 20s
- attempt 2 runs after 40s
- attempt 3 runs after 60s

Example:

```text
/msg *missingchans SET retrystep 30
```

This increasing wait pattern is useful for giving services, bots, or network state time to recover.

---

### `SET stopperformon <#channel|off>`

Sets a sentinel channel that, once confirmed joined on the server, suppresses future `perform Execute` calls for the current run.

Example:

```text
/msg *missingchans SET stopperformon #communityhub
```

Why this matters:

- maybe your `perform` script contacts a helper bot;
- that helper bot only needs to succeed once;
- once a key channel proves that access is working, there is no need to keep re-running `perform` on later retry rounds.

The module checks this against **server-truth** membership, using either:

- `WHOIS` channel visibility, or
- `443` fallback confirmation.

To disable the sentinel:

```text
/msg *missingchans SET stopperformon off
```

Accepted “off” values in the source are `off`, `none`, or `-`.

---

## Typical usage patterns

### 1. Report only, no automatic repair

This is the safest first-step deployment.

```text
/msg *missingchans SET delay 300
/msg *missingchans SET expectedmode enabled
/msg *missingchans SET joinmissing off
```

What you get:

- the module waits 5 minutes after connect;
- checks server-side membership;
- reports anything missing;
- does not send any corrective joins.

---

### 2. Rejoin missing channels after a slow auth/cloak flow

```text
/msg *missingchans SET delay 180
/msg *missingchans SET expectedmode enabled
/msg *missingchans SET joinmissing on
/msg *missingchans SET retries 4
/msg *missingchans SET retrystep 30
```

This is a straightforward recovery configuration when your initial connect sequence may be too early for some channels.

---

### 3. Re-run `perform` before join retries

```text
/msg *missingchans SET delay 180
/msg *missingchans SET joinmissing on
/msg *missingchans SET retryperform on
/msg *missingchans SET retries 4
/msg *missingchans SET retrystep 30
```

Use this when your built-in ZNC `perform` module contains the commands needed to authenticate to a helper bot or trigger a cloak before the retry joins happen.

---

### 4. Use a sentinel channel to stop repeated `perform` retries

```text
/msg *missingchans SET delay 180
/msg *missingchans SET joinmissing on
/msg *missingchans SET retryperform on
/msg *missingchans SET stopperformon #communityhub
/msg *missingchans SET retries 5
/msg *missingchans SET retrystep 30
```

In this design:

- `perform` may re-contact the helper bot on early retries;
- once `#communityhub` is confirmed joined, repeated `perform Execute` calls are suppressed;
- plain join retries can still continue for any remaining channels.

This is a sensible pattern when one “gateway” channel is a good proxy for “the auth/cloak flow is working now.”

---

## Example scenario: community bot outage or lag

Suppose a network/community setup works like this:

1. you connect;
2. ZNC's built-in `perform` messages a community auth bot;
3. that bot normally grants the state needed to enter a set of channels;
4. a temporary outage or delay prevents the first auth/join window from succeeding cleanly.

Without extra recovery logic, you might end up joined to only part of your channel list.

With `missingchans` configured appropriately:

- the module waits for the initial connection storm to settle;
- it checks what the server says you are actually on;
- it identifies which expected channels are still missing;
- it can re-run `perform Execute` if needed;
- it retries missing joins using any stored keys from ZNC;
- and it re-checks again afterward.

That is the main reason this module is valuable in environments with helper bots or other non-standard channel-access prerequisites.

---

## How the module determines “expected” channels

The expected set comes from ZNC's channel objects for the current network.

Internally, the module supports three modes:

- `all`
- `config`
- `enabled`

The code path is:

- `all`: include every channel returned by `GetNetwork()->GetChans()`;
- `config`: include only channels where `InConfig()` is true;
- `enabled`: include only channels where `InConfig()` is true and `IsDisabled()` is false.

If you maintain channels in config that you do not always want joined, `enabled` is typically the best fit.

---

## How server-side verification works

The module does **not** trust only the local ZNC channel list. Instead, it asks the server.

### Primary mechanism: self-WHOIS

It sends:

```text
WHOIS <your-current-nick>
```

and parses numeric replies:

- `319` — channel list
- `318` — end of WHOIS
- `401` — WHOIS target failure

The final missing set is computed as:

```text
expected - (actual_from_whois ∪ verified_from_443)
```

### Fallback mechanism: numeric 443

If a retry `JOIN` gets:

```text
443 ... :is already on channel
```

the module treats that as server-truth that you are already joined there and adds that channel to a verified set.

This is helpful if WHOIS visibility is incomplete for some reason but the server explicitly confirms channel membership during a retry.

---

## Implementation details

### Delayed automatic run on connect

When `OnIRCConnected()` fires, the module:

- increments an internal generation counter;
- logs that verification is scheduled;
- installs a one-shot timer for `Delay` seconds.

This generation system helps ignore stale timer callbacks from older connection cycles.

### State reset on disconnect

When the IRC connection drops, the module:

- advances generation;
- clears volatile state;
- resets retry/run-specific bookkeeping.

### Retry scheduling model

Retries are not all scheduled at once.

Instead, after each verification cycle that still finds missing channels, the module schedules the **next** join attempt with:

```text
wait = attempt_number * RetryStep
```

That means later retries back off naturally.

### Recheck after every join burst

After sending the `JOIN` commands for the current missing set, the module schedules a short recheck timer 3 seconds later.

This keeps the feedback loop tight:

- join attempt;
- short pause;
- WHOIS again;
- recompute missing set.

### Channel keys are preserved

For each missing channel, the module looks up the configured ZNC channel object.

If the channel has a stored key, it sends:

```text
JOIN <channel> <key>
```

otherwise it sends:

```text
JOIN <channel>
```

This is important for recovering keyed channels automatically.

### Case-insensitive channel comparisons

The source uses a case-insensitive comparator for `CString` sets. This reduces false mismatches such as:

- `#Chan`
- `#chan`

being treated as different entries during expected/actual comparison.

### Robust WHOIS 319 parsing

The module concatenates WHOIS parameters from index 2 onward before splitting the channel list. This is more tolerant of parser/layout differences in the `319` reply.

### Client-attach notice

If a retry join attempt happens while no client is attached to ZNC, the module stores a short notice and prints it when a client later attaches.

---

## Interaction with ZNC's built-in `perform` module

`missingchans` does **not** replace `perform`. Instead, it can optionally use it as a recovery helper.

The integration flow is:

1. `missingchans` detects missing channels;
2. before a retry join round, it optionally finds `perform`;
3. if found, it sends `Execute` to that module;
4. it then sends `JOIN` for the currently missing channels.

Lookup order for `perform` is:

1. network module `perform`
2. user module `perform`

This makes `missingchans` especially useful when your auth workflow is already encoded in `perform` and you simply need a verification/retry layer on top of it.

---

## Recommended configuration strategy

For bot-gated or cloak-gated channels:

1. put your prerequisite bot/auth commands in ZNC's built-in `perform` module;
2. set a `Delay` long enough for the normal happy-path flow to complete;
3. enable `JoinMissing` so failed joins can be retried;
4. enable `RetryPerform` if re-triggering the auth flow is useful;
5. optionally set `StopPerformOn` to a reliable “gateway” channel.

A practical starting point might be:

```text
/msg *missingchans SET delay 180
/msg *missingchans SET expectedmode enabled
/msg *missingchans SET joinmissing on
/msg *missingchans SET retryperform on
/msg *missingchans SET retries 4
/msg *missingchans SET retrystep 30
/msg *missingchans SET stopperformon #communityhub
```

Tune from there based on how quickly the network, helper bot, or cloak system normally settles after reconnect.

---

## Limitations and caveats

- The module's server-side truth is based primarily on self-`WHOIS`, so behavior depends on what the network exposes there.
- `443` improves correctness when the server says you are already on a retried channel, but it is only a fallback, not a complete substitute for WHOIS visibility.
- If your network hides some channel memberships from WHOIS and never emits a useful `443` for them, they may continue to appear missing.
- The module only retries channels that are part of the expected set built from ZNC's configured channel objects.
- `RetryPerform` only helps if your `perform` contents are actually suitable for safe re-execution.
- A poorly chosen `Delay` that is too short can make retries start before your normal auth flow has had time to succeed.

---

## Safe rollout advice

A good way to deploy this module is:

1. load it with `JoinMissing` off;
2. watch what `SHOW`, `STATUS`, and `RUN` report after real reconnects;
3. switch to `ExpectedMode enabled` if needed;
4. only then enable `JoinMissing`;
5. add `RetryPerform` after confirming that re-running `perform` is safe in your environment.

That gives you confidence in the verification logic before enabling automated repair actions.

---

## Summary

`missingchans` is best thought of as a **verification and recovery layer** for ZNC reconnect behavior.

It does not assume that the first connect-time join sequence succeeded. Instead, it asks the server what actually happened, identifies what is missing, and can retry in a controlled way.

That makes it particularly well suited to IRC environments where channel access depends on additional moving parts such as custom auth bots, cloaks, delayed permissions, or intermittent outages.
