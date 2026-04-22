# highlightctx

Detached-only highlight context capture for ZNC, with its own live per-channel history, durable active-event journaling, replay into `*highlightctx`, and optional `ignore_drop` integration.

Current module version in source: **0.7.0**. See [CHANGELOG](./CHANGELOG.md) for per-release notes.

---

## What this module is for

`highlightctx` is designed for a very specific gap in the usual ZNC workflow:

when you are **detached** from a network, you may still want a clean, focused replay of the most important conversations that happened while you were away — not a full playback dump, and not a dependency on the normal channel buffer length.

This module watches **live incoming channel traffic only while the network is detached**, detects lines that highlight your current nick, captures surrounding context, stores active highlight events durably, and then replays those events into the module window when you attach.

The goal is to give you a **compact, high-signal review of missed highlights**, with enough surrounding context to understand what happened, while staying independent from the ordinary ZNC playback buffer.

In practice, that means:

- it only captures while the network has **no attached clients**
- it keeps its own per-channel in-memory history for pre-highlight context
- it does **not** inspect or depend on the normal ZNC channel/playback buffers
- it journals real highlight events to disk as they happen, so active captures survive an unexpected ZNC/VPS interruption
- it replays into `*highlightctx`, not into the channel windows
- it clears delivered events after replay

This makes it useful if you want **highlight-centric offline catch-up** rather than broad backlog replay.

---

## Design goals

The source is explicit about the intended behavior:

- **Network module** for modern ZNC environments, intended for **ZNC 1.9.1+**
- **Detached-only capture**
- **Live hooks only**
- **Independent of normal buffer length**
- **Durable storage only for real highlight events**, not for ordinary chatter
- **Replay into `*highlightctx` on attach**
- **Sort replay by channel, then event time**
- **Clear delivered events after replay**
- **Use native IRCv3 time/server-time replay when supported by the client**, otherwise fall back to inline UTC timestamps
- **Optional `ignore_drop` integration** with `off`, `on`, and `auto` modes, where `auto` is position-aware (see below)

---

## High-level behavior

At a high level, the module behaves like this:

1. The network becomes detached.
2. `highlightctx` starts observing live channel messages, notices, and actions.
3. For each channel, it keeps a rolling in-memory ring of recent lines.
4. If a line contains your current nick as a proper nick-like highlight boundary match, the module starts a highlight event.
5. It snapshots up to `before` earlier lines from that channel’s private ring.
6. It stores the trigger line.
7. It collects up to `after` later lines from that same channel.
8. When complete — or when interrupted by an attach/replay — the event is finalized.
9. On attach, finalized events are replayed into `*highlightctx` and then cleared.

If the event is interrupted before enough trailing lines arrive, it is replayed as **partial**.

---

## What it captures

The module captures these incoming channel message types:

- normal channel text messages
- channel notices
- channel actions (`/me` style messages)

Internally these are tagged as:

- `T` = text
- `N` = notice
- `A` = action

Replay formatting preserves their display style:

- text → `<nick> message`
- notice → `-nick- message`
- action → `* nick message`

The trigger line is marked with `>>>` in replay.

---

## What it does not do

This is just as important as what it does do.

`highlightctx` does **not**:

- inspect the normal ZNC playback buffer
- depend on ordinary channel buffer sizes
- retroactively search old channel history for highlights
- persist all detached chatter to disk
- capture while you are attached to the network
- replay into the original channel windows
- keep delivered events indefinitely after replay

This is intentionally a **focused context module**, not a general logging or backlog module.

---

## Detection model

A highlight event starts when:

- the network is detached
- the channel is not excluded
- capture is currently allowed under the `ignore_drop` mode rules
- an incoming channel line contains your **current network nick**
- the line is **not** from your own nick

Nick matching is case-insensitive and uses nick-style boundary logic, so it tries to detect proper nick mentions rather than arbitrary substring matches embedded in larger nick-like tokens.

---

## Replay model

Replay happens automatically in `OnClientAttached()`.

When you attach:

- any still-open events are finalized as **partial**
- all pending finalized events are sorted
- events are replayed into `*highlightctx`
- delivered events are marked as delivered and removed from persistent state
- the durable journal is compacted
- volatile in-memory channel rings are cleared

There is also a manual replay command:

```text
/msg *highlightctx ReplayNow
```

That does the same replay-and-clear flow immediately for the current client.

### Replay ordering

Events are sorted by:

1. channel name (case-normalized)
2. event start timestamp
3. event ID

### Replay destination

Replay is sent to the module window, not to the source channels:

```text
*highlightctx
```

### Timestamp behavior

If the attached client supports native IRCv3 time replay:

- replay is emitted as synthetic raw `PRIVMSG` lines
- `@time=` tags are attached with the original UTC timestamp
- the client can display the historical timestamps natively

If the client does **not** support that capability:

- replay falls back to module output with inline UTC timestamps prefixed into the text

This behavior is one of the main implementation features of the module.

---

## Partial vs complete events

An event is **complete** when the module successfully gathers the full configured number of trailing `after` lines.

An event is **partial** when replay occurs before enough trailing lines arrived, or when the module finalizes open events during attach/replay recovery.

This is expected behavior, not an error.

The source explicitly treats `before` and `after` as **caps, not guarantees**.

---

## Persistence model

One of the core design choices in `highlightctx` is that it does **not** persist ordinary chatter.

Instead:

- the rolling pre-highlight per-channel context ring is kept **only in RAM**
- once a real highlight event starts, that event is durably journaled to disk
- as new `after` lines arrive for the event, they are appended durably
- when the event is finalized, finalization is journaled durably
- when the event is delivered, delivery is journaled too

This means the hot path stays lighter than a full detached log, while still protecting real highlight captures from being lost if ZNC or the VPS dies mid-capture.

### Journal file

The module uses a journal file named:

```text
highlightctx.journal
```

It lives under the module save path returned by ZNC.

### Durability behavior

The implementation uses append/replace patterns with `fsync()` and parent-directory syncing so that journal mutations are intended to be durable on disk, not just buffered in userspace.

### Journal compaction

The journal is compacted when needed, including:

- after replay/clear
- when it grows beyond the internal threshold
- when `Compact` is called manually

Internal threshold in the source:

- `kCompactThresholdLines = 512`

---

## `ignore_drop` integration

### What `ignore_drop` detection does

`highlightctx` captures channel messages while you are detached. If someone on your ignore list is spamming or harassing you, you do not want their messages appearing in your replay — you want them filtered out before `highlightctx` ever sees them.

[`ignore_drop`](../ignore_drop/README.md) is a separate module whose job is to drop messages from users on your ignore list. When a message comes in from an ignored user, `ignore_drop` tells ZNC "halt this — do not pass it to any other module." That halt only works if `ignore_drop` runs *before* `highlightctx` in the chain of modules that process the message. If it runs after, the damage is done: `highlightctx` has already captured the ignored user's message.

ZNC dispatches each incoming message to modules in the order they appear in the module list. So detection is really asking one question: **"is `ignore_drop` positioned ahead of `highlightctx` in the module list, so its filtering runs first?"**

Three possible answers:

- **`ignore_drop` is ahead of `highlightctx`** — filtering works, protection is active. "Armed."
- **`ignore_drop` is loaded but positioned after `highlightctx`** — `highlightctx` sees messages before the filter gets a chance. Not armed.
- **`ignore_drop` is not loaded at all** — nothing to filter with. Not armed.

The three integration modes (`off`, `on`, `auto`) described below decide what `highlightctx` does with that answer.

For `auto` mode specifically, detection runs at three moments: when `highlightctx` first loads, when ZNC finishes booting up from `znc.conf` (the important one for `/znc restart`), and whenever you run `Rearm` manually. The most recent result is stored in a flag called "armed," visible in `Status` output.

Separately from detection, every incoming channel message goes through a live check: "is `ignore_drop` currently loaded right now?" If the answer is no and strict mode is effective, capture pauses for that message. The actual capture behavior is therefore always correct based on what is true when a message arrives, even if the armed flag from some earlier check is stale.

In practical terms: if you have `ignore_drop` set up and correctly ordered (`ignore_drop` before `highlightctx` in `znc.conf`), you do not have to think about any of this. `highlightctx` detects the good setup at startup, goes into strict mode, and filters ignored users out of your replay automatically. If you do not have `ignore_drop`, `highlightctx` falls back to unprotected capture and you get everything that comes in.

### What `Rearm` is for

Runtime load or unload of [`ignore_drop`](../ignore_drop/README.md) is not automatically detected by `highlightctx`. ZNC 1.9.x dispatches the relevant lifecycle hooks only to global-scope modules, so a network-scope module like `highlightctx` gets no notification when other modules come and go. The armed flag in `Status` only refreshes at the three moments described above — so after a manual `loadmod ignore_drop` or `unloadmod ignore_drop`, the flag can go stale until something prompts a re-check.

This does not affect capture correctness. The live `HasIgnoreDropLoaded()` check inside `ShouldCaptureNow()` runs on every incoming message and always gets an accurate answer from ZNC. Protection works in real time; only the *display* of whether protection is active can go stale.

`Rearm` exists to bridge that gap. It does three things nothing else does:

1. **Refresh the armed flag shown in `Status`.** After a runtime load or unload of `ignore_drop`, `Status` can show a stale value; `Rearm` re-checks the module list and updates it.

2. **Explicitly disarm after sticky armed.** If `ignore_drop` was unloaded at runtime, the strict requirement stays in force and capture stays paused by design — so capture does not silently resume unprotected. To actively step out of that state and resume unprotected capture, `Rearm` does it cleanly. The alternative is `SetRequireIgnoreDrop off`.

3. **Verify hook-order position after a reorder.** After an `UpdateMod highlightctx` or a manual `loadmod` / `unloadmod` sequence, `Rearm` inspects the new list and tells you whether things are now positioned correctly. Its output is more diagnostic than just re-reading `Status`: it explicitly says "armed" or "not armed because positioned at or after highlightctx," with remediation steps.

If you set up `znc.conf` once with the correct order and do not touch module loading at runtime, you will almost never need `Rearm`. It is there for cases where runtime state has changed and you want the module's status display (or sticky-armed state) to reflect reality.

---

The module supports three integration modes for `ignore_drop`:

- `off`
- `on`
- `auto`

### `off`

`ignore_drop` is never required.

Capture depends only on the normal detached-state logic.

### `on`

`ignore_drop` must already be loaded on the network when `highlightctx` loads.

If it is not already loaded, module load fails.

At runtime, if the requirement is effective and `ignore_drop` is absent, capture is paused.

### `auto`

This is the default.

In `auto` mode, strict ignore-aware behavior is **armed** only when `ignore_drop` is positioned **ahead of** `highlightctx` in the network's module list, so that its hooks dispatch before ours. That position-aware check is what `auto` means as of 0.7.0.

The armed state is re-evaluated at these points:

- **`OnLoad`** — initial check using `HasIgnoreDropLoaded()`. If `ignore_drop` is already in the module list when we load, we infer that it is ahead of us. This is correct in the common case, but is just a heuristic and can be wrong at `/znc restart` if ZNC loads `highlightctx` before `ignore_drop` (alphabetical order will do this, since `h` precedes `i`).
- **`OnBoot`** — fires after all `znc.conf` modules have loaded. At that point the module inspects actual module-list positions and sets the armed state accurately. This is the primary fix for the `/znc restart` case.
- **`Rearm` command** — on-demand manual re-check.

#### What about runtime load/unload of `ignore_drop`?

Runtime load or unload of `ignore_drop` does **not** automatically refresh the armed flag. ZNC 1.9.x dispatches the `OnModuleLoading` and `OnModuleUnloading` lifecycle hooks only to global-scope modules, so a network-scope module like `highlightctx` does not receive these callbacks for other modules in the same scope. After any runtime load, unload, or `UpdateMod` affecting `ignore_drop` or `highlightctx`, run `Rearm` to refresh the armed flag for display and to trigger any needed arm/disarm transition.

Capture correctness does not depend on the armed flag being up to date: `ShouldCaptureNow()` calls `HasIgnoreDropLoaded()` live on every incoming message, so when the strict requirement is effective but `ignore_drop` is absent, capture pauses regardless.

#### Sticky armed semantics

Once `auto` has armed, the strict `ignore_drop` requirement stays effective across a subsequent unload of `ignore_drop`. That is deliberate: if you asked for strict protection by wiring `ignore_drop` ahead of `highlightctx`, unloading `ignore_drop` should cause capture to **pause** rather than silently resume without the protection. Explicit disarm is available via `Rearm` (once `ignore_drop` has left the list, `Rearm` reports the transition and clears the armed flag) or by changing the mode with `SetRequireIgnoreDrop off`.

#### Important caveat on hook order

No hook can fix hook order on its own. If `ignore_drop` ends up positioned after `highlightctx` in the module list, the fix is operator-driven:

- **At startup / across `/znc restart`:** reorder `LoadModule` lines in `znc.conf` so `ignore_drop` precedes `highlightctx`.
- **At runtime:** `UnloadMod highlightctx` followed by `LoadMod --type=network highlightctx`, so `highlightctx` is re-added to the tail of the module list, after `ignore_drop`.

The `Rearm` command re-checks and reports the state accurately, but will honestly tell you when it cannot arm.

---

## Load syntax

Build comment in the source:

```text
znc-buildmod highlightctx.cpp
```

Load example from the source:

```text
/msg *status LoadMod --type=network highlightctx [before=8 after=8 require_ignore_drop=auto excludes=#chan1,#chan2]
```

### Supported load arguments

The module accepts space-separated `key=value` pairs.

#### `before=<count>`

Maximum number of earlier lines to snapshot from the per-channel ring when a highlight trigger happens.

#### `after=<count>`

Maximum number of later lines to collect after the trigger.

#### `require_ignore_drop=<off|on|auto>`

Controls `ignore_drop` integration mode.

#### `max_events=<count|0|off>`

Maximum number of finalized pending events to keep at once.

- `0` or `off` disables the cap
- default is disabled
- when the cap is reached, the oldest pending event is silently dropped to make room

#### `excludes=#chan1,#chan2,...`

Comma-separated list of excluded channels.

### Example load commands

Minimal:

```text
/msg *status LoadMod --type=network highlightctx
```

With larger context windows:

```text
/msg *status LoadMod --type=network highlightctx before=12 after=20
```

With channel exclusions:

```text
/msg *status LoadMod --type=network highlightctx excludes=#ops,#noise,#bots
```

With explicit `ignore_drop` requirement:

```text
/msg *status LoadMod --type=network highlightctx require_ignore_drop=on
```

With a pending-event cap:

```text
/msg *status LoadMod --type=network highlightctx before=8 after=8 max_events=50
```

---

## Runtime commands

The module registers a fairly complete command set.

### Help / overview

```text
/msg *highlightctx Help
/msg *highlightctx Overview
/msg *highlightctx Version
/msg *highlightctx Status
```

### Replay and maintenance

```text
/msg *highlightctx ReplayNow
/msg *highlightctx Compact
/msg *highlightctx ClearPending
```

### Context sizing

```text
/msg *highlightctx SetBefore <count>
/msg *highlightctx SetAfter <count>
/msg *highlightctx SetMaxEvents <count|0|off>
```

### Exclusion management

```text
/msg *highlightctx AddExclude <#channel>
/msg *highlightctx DelExclude <#channel>
/msg *highlightctx ListExcludes
```

### `ignore_drop` mode

```text
/msg *highlightctx SetRequireIgnoreDrop <off|on|auto>
/msg *highlightctx Rearm
```

### Reset

```text
/msg *highlightctx Reset
```

Resets settings to defaults, but **does not discard pending/open events**.

---

## Command reference

### `Overview`

Prints a detailed built-in summary of how capture, replay, persistence, and `ignore_drop` behavior work.

### `Version`

Shows the version marker embedded in the source.

### `Status`

Shows current state, including:

- version marker
- whether detached-only capture is active right now
- whether the network is attached right now
- current `before` cap
- current `after` cap
- current `max_events` cap
- `require_ignore_drop` mode
- whether `ignore_drop` is ahead of `highlightctx` in hook order
- whether `auto` is armed
- whether the `ignore_drop` requirement is effectively active
- whether `ignore_drop` is currently loaded
- whether the current client supports native server-time replay
- number of excluded channels
- number of open events
- number of pending finalized events
- journal path

When `auto` mode is unarmed but `ignore_drop` is loaded, `Status` explains that the cause is hook-order position and points you to `Rearm` and the module-reload / `znc.conf` reorder workarounds.

### `ReplayNow`

Finalizes open events as partial if needed, replays everything pending into `*highlightctx`, then clears delivered events.

### `SetBefore <count>`

Sets the pre-highlight context cap.

This also trims existing in-memory rings to the new size.

### `SetAfter <count>`

Sets the post-trigger context cap for **new events**.

Already-open events keep the cap they started with.

### `SetMaxEvents <count|0|off>`

Controls the pending finalized-event cap.

If you lower the cap below the current number of pending events, the oldest pending events are dropped immediately.

### `Reset`

Resets settings to compiled defaults:

- `before=8`
- `after=8`
- `max_events=disabled`
- `require_ignore_drop=auto`
- excludes cleared

Pending and open events remain intact.

### `AddExclude <#channel>`

Excludes a channel completely from detached highlight capture.

### `DelExclude <#channel>`

Removes a channel exclusion.

### `ListExcludes`

Lists all excluded channels.

### `SetRequireIgnoreDrop <off|on|auto>`

Changes the integration mode at runtime.

Important behavior:

- setting `on` fails if `ignore_drop` is not currently loaded
- setting `auto` recomputes whether auto mode is armed based on whether `ignore_drop` is currently ahead of `highlightctx` in hook order
- if you want the position-aware check to run right now without changing the mode, use `Rearm`

### `Rearm`

Re-checks `ignore_drop` presence and hook-order position without reloading the module. Reports:

- whether `ignore_drop` is loaded
- whether `ignore_drop` is ahead of `highlightctx` in hook order
- current `require_ignore_drop` mode
- whether `auto` mode is now armed
- whether the `ignore_drop` requirement is effectively active
- a plain-language diagnosis (not armed because not loaded, not armed because positioned at or after us, or armed because positioned ahead)
- any armed ↔ unarmed transition relative to the previous state

`Rearm` cannot fix hook order on its own — that requires either unloading and reloading `highlightctx` so it ends up after `ignore_drop` in the list, or reordering `LoadModule` lines in `znc.conf` for the next restart. Outside of `auto` mode, `Rearm` updates the diagnostic state but does not change behavior.

`Rearm` is also the canonical way to refresh the armed flag after any runtime change to `ignore_drop` (load, unload, or `UpdateMod`), because ZNC does not notify network-scope modules about lifecycle events of other modules.

### `Compact`

Rewrites the durable journal to the minimum current representation.

### `ClearPending`

Clears all open and pending events, clears volatile state, and compacts the journal.

This is useful for cleanup and testing.

---

## Defaults

Compiled/runtime defaults in the source:

- `before = 8`
- `after = 8`
- `max_events = 0` → disabled
- `require_ignore_drop = auto`
- excluded channels = empty

---

## Example usage workflow

### Basic usage

1. Load the module as a network module.
2. Detach from the network.
3. Let traffic happen naturally while away.
4. If your nick is mentioned, `highlightctx` begins capturing context.
5. Reattach later.
6. Review the replay in `*highlightctx`.

### Example configuration

```text
/msg *status LoadMod --type=network highlightctx before=6 after=12 excludes=#noise,#flood require_ignore_drop=auto
```

This means:

- keep up to 6 lines before the trigger
- keep up to 12 lines after the trigger
- ignore `#noise` and `#flood`
- use strict `ignore_drop` integration when `ignore_drop` is ahead of `highlightctx` in hook order

### Recommended `znc.conf` ordering for `auto` mode

If you want `auto` mode to arm reliably across `/znc restart`, make sure `ignore_drop` is listed before `highlightctx` in the network's `LoadModule` lines:

```text
LoadModule = ignore_drop ...
LoadModule = highlightctx ...
```

ZNC loads modules in the order they appear in the config, so this guarantees `ignore_drop`'s hooks dispatch before ours regardless of alphabetical considerations.

---

## Replay shape

A replayed event contains:

1. an event header line
2. the captured `before` lines
3. the trigger line marked with `>>>`
4. the captured `after` lines
5. a separator line between events

Conceptually it looks like this:

```text
[#channel] highlight event #42 (complete, before=3, after=4/4)
[#channel] <nick1> previous context
[#channel] <nick2> more context
[#channel] >>> <nick3> Tim: are you around?
[#channel] <nick4> follow-up line 1
[#channel] <nick5> follow-up line 2
```

If native server-time replay is available, the client can show those lines with their original timestamps. Otherwise the module prefixes UTC timestamps inline.

---

## Build and install

### Build

Typical build command from the source header:

```text
znc-buildmod highlightctx7.cpp
```

This should produce `highlightctx7.so` for ZNC module loading. If you prefer a shorter module name (`highlightctx` instead of `highlightctx7`), rename the source file to `highlightctx.cpp` before building.

### Install

Install the built module into the appropriate ZNC module path for your environment, then load it as a **network module**.

Typical load command:

```text
/msg *status LoadMod --type=network highlightctx
```

### Upgrade / hot-update note

If you update a live-loaded ZNC module, avoid overwriting the existing `.so` in place with a direct `cp` onto the live path.

Safer workflow:

1. build the new module to a temporary filename
2. move it into place atomically with `mv` on the same filesystem
3. then run `UpdateMod`

A weaker fallback that may also avoid the immediate overwrite problem is:

1. remove the old `.so`
2. copy the new `.so` into place
3. run `UpdateMod`

That is an operational safety note for live ZNC updates, not a behavior of `highlightctx` itself.

---

## Implementation details

This section is for people reading or maintaining the module.

### Module type

`highlightctx` is implemented as a **network module**:

- `Info.AddType(CModInfo::NetworkModule)`

So configuration and state are per-network, not global per-user.

### Hooks used

The module hooks:

- `OnLoad`
- `OnBoot` — re-check `ignore_drop` hook order after all `znc.conf` modules are loaded (fires only for modules loaded from `znc.conf`)
- `OnChanTextMessage`
- `OnChanNoticeMessage`
- `OnChanActionMessage`
- `OnClientAttached`
- `OnClientDetached`

That means capture happens from the real live message stream as it passes through the three `OnChan*` hooks.

Not hooked: `OnModuleLoading` and `OnModuleUnloading`. ZNC 1.9.x dispatches those to global-scope modules only, so a network-scope override would be dead code. Refreshing the armed flag after a runtime load/unload of `ignore_drop` is therefore handled by the `Rearm` command rather than a hook.

### Detached-only gate

Capture is allowed only if `ShouldCaptureNow()` returns true.

That currently requires:

- a valid network
- no attached client on that network
- `ignore_drop` requirement not blocking capture

The third condition is checked live on every incoming message against `HasIgnoreDropLoaded()`, so capture fails closed the moment `ignore_drop` is unloaded, independent of the cached armed flag.

### In-memory structures

The main internal structures are:

- `m_ring_by_chan` → per-channel ring buffer for pre-trigger context
- `m_open_by_chan` → open events still collecting trailing `after` lines
- `m_pending` → finalized events waiting to be replayed

Each captured line stores:

- timestamp (`ts_sec`)
- kind (`T`, `N`, `A`)
- nick
- text

Each event stores:

- unique event ID
- channel and lowercase channel key
- start timestamp
- `after` cap for that event
- captured `before` lines
- trigger line
- captured `after` lines
- finalized flag
- partial flag

### Arming logic

The armed flag (`m_auto_ignore_drop_armed`) is derived from a single underlying bool (`m_ignore_drop_present_on_module_load`) whose semantic as of 0.7.0 is "was `ignore_drop` found ahead of `highlightctx` in hook order at the last check." That bool is updated at:

- `OnLoad` — via `HasIgnoreDropLoaded()` (heuristic; accurate in the common case).
- `OnBoot` — via `IsIgnoreDropAheadOfUs()` (position-aware check after all `znc.conf` modules are loaded).
- `Rearm` — via `IsIgnoreDropAheadOfUs()`.

`RecomputeIgnoreDropRuntimeState()` maps the underlying bool to `m_auto_ignore_drop_armed` according to the current `require_ignore_drop` mode.

The armed flag is deliberately sticky across runtime unloads of `ignore_drop`, because `ShouldCaptureNow()` will still pause capture when the strict requirement is effective but `ignore_drop` is absent. This preserves the protection the operator asked for, instead of silently lifting it on every transient unload.

### Ring behavior

The per-channel ring stores only the most recent `before` lines.

Every incoming eligible channel line is appended to the channel ring after open-event feeding and highlight detection.

If the ring exceeds `before`, the oldest line is dropped.

### Event start behavior

When a non-self line highlights your nick:

- a new event ID is allocated
- channel metadata is stored
- the current channel ring is copied into the event’s `before`
- the trigger line is stored
- a durable journal `begin` record is written
- if `after=0`, the event is finalized immediately
- otherwise the event is added to the open-event list for that channel

### Event feed behavior

For every later eligible line in the same channel:

- each open event for that channel receives the line in its `after` vector
- a durable journal `after` record is appended
- once `after.size() >= after_cap`, the event is finalized

### Finalization behavior

Finalization:

- marks the event finalized
- marks whether it is partial or complete
- moves it into the pending list
- appends a durable finalize record

If `max_events` is enabled and the pending count exceeds the cap, the oldest pending events are marked delivered and dropped.

### Replay output path

Replay uses two possible output modes.

#### Native time-tag replay

If the attached client supports time tags/server-time:

- the module synthesizes raw `PRIVMSG` lines
- source prefix is built as `*highlightctx!znc@znc.in`
- target is the current client nick
- `@time=` is attached using ISO-8601 UTC

#### Fallback replay

If native support is unavailable:

- the module emits normal module output
- each line is prefixed with `[YYYY-MM-DDTHH:MM:SS.000Z]`

### Formatting helpers

Formatting helpers include:

- UTC ISO-8601 timestamp formatting
- UTC `HH:MM:SS` formatting
- inline replay body formatting by message type
- IRC text sanitization for raw replay
- IRC message-tag value escaping

### Journal encoding

The journal format uses a compact line-oriented record model with operations including:

- `B` → begin event
- `A` → append after-line
- `F` → finalize event
- `D` → delivered event

Nick/text and some fields are hex-encoded so the journal can safely represent arbitrary IRC text without relying on raw delimiters being absent.

On load, the module replays the journal into reconstructed in-memory state:

- unfinished events go back into the open-event map
- finalized-undelivered events go into pending
- delivered events are removed

### Recovery behavior

If ZNC or the host dies mid-capture:

- RAM-only ordinary chatter is lost
- active highlight events survive if already journaled
- on the next attach, unfinished recovered events can be replayed as partial

This is one of the main reasons the module journals active events but not general chatter.

---

## Storage and config details

The module stores persistent settings via module NV entries, including:

- `before_max`
- `after_max`
- `max_events`
- `require_ignore_drop_mode`
- legacy compatibility key `require_ignore_drop`
- `excluded_channels`
- `version_marker`

Excluded channels are stored one per line internally.

---

## Operational notes and caveats

- `before` and `after` are **caps**, not guarantees.
- If you attach quickly after a trigger, the event may replay as partial.
- Capture does not happen while attached.
- Excluded channels contribute neither triggers nor context.
- `SetAfter` affects only new events, not ones already open.
- In `auto` mode, arming now follows hook-order position rather than load-time presence:
  - if `ignore_drop` is ahead of `highlightctx` in the module list, `auto` arms automatically at the next re-check point
  - if `ignore_drop` is at or after `highlightctx` in the module list, `auto` does not arm
  - reloading `highlightctx` after `ignore_drop` is loaded will put `highlightctx` at the end of the list, so ahead-of-us becomes true, and the next `Rearm` (or auto-re-check point) will arm
  - reordering `znc.conf` so `LoadModule = ignore_drop` precedes `LoadModule = highlightctx` makes this stable across `/znc restart`
- Runtime load or unload of `ignore_drop` does not automatically update the armed flag, because ZNC 1.9.x dispatches the relevant lifecycle hooks only to global-scope modules. Run `Rearm` after any such runtime change to refresh the flag. Capture safety does not depend on this: `ShouldCaptureNow()` always checks `HasIgnoreDropLoaded()` live.
- The armed flag is **sticky** across `ignore_drop` unload — capture pauses rather than silently resuming unprotected. Use `Rearm` to explicitly disarm once `ignore_drop` is gone, or change mode with `SetRequireIgnoreDrop`.
- `OnBoot` re-checks arming automatically for modules loaded from `znc.conf`. For modules loaded dynamically via `LoadMod`, `OnBoot` is not called by ZNC, so `Rearm` (or an explicit reload) is the path to reassert the armed state.
- `ReplayNow` and attach both finalize open events as partial before replaying.
- `ClearPending` is destructive for current event state and should be used intentionally.

---

## Summary

`highlightctx` is a specialized detached-only ZNC network module for **capturing and replaying highlight-centered conversation context**.

Its main strengths are:

- independence from normal playback buffers
- focus on real missed highlights instead of general backlog
- durable journaling of active events
- replay into a dedicated module window
- native timestamp replay when the client supports it
- configurable `ignore_drop` integration with position-aware auto mode and on-demand re-check via `Rearm`
- exclusion control per channel

If your goal is to preserve the context around important missed mentions while staying detached, without turning the module into a general-purpose logger, this design is aimed exactly at that use case.
