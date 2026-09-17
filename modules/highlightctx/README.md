# highlightctx

Detached-only highlight context capture for ZNC, with its own live per-channel history, durable active-event journaling, replay into `*highlightctx`, and optional `ignore_drop` integration.

Current module version in source: **0.11.1**. See [CHANGELOG](./CHANGELOG.md) for per-release notes and [TESTING](./TESTING.md) for the test suite.

---

## What this module is for

[`highlightctx`](./src/highlightctx.cpp) is designed for a very specific gap in the usual ZNC workflow:

when you are **detached** from a network, you may still want a clean, focused replay of the most important conversations that happened while you were away — not a full playback dump, and not a dependency on the normal channel buffer length.

This module watches **live incoming channel traffic only while the network is detached**, detects lines that highlight your current nick, captures surrounding context, stores active highlight events durably, and then replays those events into the module window when you attach.

The goal is to give you a **compact, high-signal review of missed highlights**, with enough surrounding context to understand what happened, while staying independent from the ordinary ZNC playback buffer.

In practice, that means:

- it only captures while the network has **no attached clients**
- it keeps its own per-channel in-memory history for pre-highlight context
- it does **not** inspect or depend on the normal ZNC channel/playback buffers
- it journals real highlight events to disk as they happen, so active captures survive an unexpected ZNC/VPS interruption — unless you turn journaling off with [`journal=off`](#journaloffon), in which case nothing is written to disk at all
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
- **Exclusions for channels, nicknames, and `nick!ident@host` hostmasks**, with different semantics per kind
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
7. It collects up to `after` later lines from that same channel. If another qualifying highlight arrives in that channel during this step, it does **not** start a second, overlapping event: it is marked as an additional trigger inside the open event and extends the collection window (see [Overlapping highlights (extension)](#overlapping-highlights-extension)).
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
- the sender is not excluded by a nick or hostmask exclusion
- capture is currently allowed under the `ignore_drop` mode rules
- an incoming channel line contains your **current network nick**
- the line is **not** from your own nick

Nick matching is case-insensitive and uses nick-style boundary logic, so it tries to detect proper nick mentions rather than arbitrary substring matches embedded in larger nick-like tokens.

---

## Exclusions

`highlightctx` supports three kinds of exclusion, all managed through the same `AddExclude` / `DelExclude` / `ListExcludes` commands.

### Channel exclusions

A channel exclusion drops the channel entirely. No triggers, no context, no ring buffering. Added as `AddExclude #channel` (any token starting with `#`, `&`, `+`, or `!` is classified as a channel).

Use this when you never want a given channel to contribute to highlight capture — for example, a very noisy bot channel where mentions of your nick are likely to be bot-driven rather than human.

### Nick exclusions

A nick exclusion is narrower. Messages from the excluded nick **still appear** as before/after context for other users' triggers, but the excluded nick **cannot start a new event** of their own. Added as `AddExclude BadNick` (any token without a channel prefix and without `!` or `@` is classified as a nick).

This is the right choice when someone is noisy enough to pollute your replay if they triggered on their own, but you still want their messages visible when someone else triggers around them.

### Hostmask exclusions

A hostmask exclusion works the same way as a nick exclusion, but matches against the full `nick!ident@host` of the sender instead of just the nickname. Added as `AddExclude *!*@evil.example` or `AddExclude *!~bot@host.example` (any token containing `!` or `@` is classified as a full mask).

Use this when you need to suppress a whole group of senders (everyone from a known bridge host, a shared script bot, etc.) as a trigger source.

### Mask syntax

Nick and hostmask exclusions use:

- `*` for any sequence of characters (including empty)
- `?` for exactly one character
- any other character matched literally
- **RFC 1459 case folding** before comparison

RFC 1459 folding matters because most IRC daemons treat the pairs `A-Z`/`a-z`, `[`/`{`, `]`/`}`, `\`/`|`, and `~`/`^` as equivalent. The folding used here matches the folding used by [`ignore_drop`](../ignore_drop/README.md), so `[bot]` and `{bot}` fold to the same stored form and cannot be used to defeat each other as exclusion masks.

### Mask validation

Masks are rejected at add-time if:

- they are empty, or
- they contain embedded CR, LF, or NUL bytes (these would corrupt the NV storage format)

Duplicate entries (compared on folded form) are rejected with a message rather than silently added twice.

### Listing and removal

`ListExcludes` shows all exclusions in a single numbered list, channels first (alphabetically sorted), then nick and hostmask exclusions in insertion order. Each entry is tagged `[channel]`, `[nick]`, or `[mask]` to make its kind obvious.

`DelExclude` accepts any of:

- a channel name (`DelExclude #noise`)
- an exact mask, compared after RFC 1459 folding (`DelExclude BadNick`, `DelExclude *!*@evil.example`)
- a numeric index from the `ListExcludes` output (`DelExclude 3`)

### Why the difference between channel and nick exclusions matters

A channel exclusion is coarse: the channel contributes nothing. A nick exclusion is surgical: the sender's messages are still in your context, they just cannot be the reason an event starts.

A useful way to think about it: channel exclusion is "I don't care what happens here," while nick exclusion is "this person's spam isn't worth a highlight on its own, but I still want to see their messages around real highlights from others."

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

An event is **complete** when the module successfully gathers the full number of trailing `after` lines. For an event that was extended by a later highlight, that number is the extended target (a full `after` window past the latest trigger), not the original cap.

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

Journaling is on by default and can be turned off entirely with the [`journal=off`](#journaloffon) load argument.

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
- when it grows beyond the current trigger point
- when `Compact` is called manually

Internal constants in the source:

- `kCompactThresholdLines = 512` — the floor for the trigger point
- the trigger point itself is `max(512, 2 x lines written by the last compaction)`, recomputed after every successful compaction

See [Compaction and large pending queues](#compaction-and-large-pending-queues) for why the trigger point is derived rather than fixed.

### Disabling the journal

Since 0.10.0, the load argument `journal=off` turns the durable journal off completely.

With journaling disabled:

- the journal file is **not read** at load, so nothing is recovered from a previous session
- **nothing is written** to disk: no event records, no compaction, no temporary files
- captured context — channel names, nicks, and message text — exists only in the module's memory until it is replayed to you

The trade-off is durability. Open and pending events are lost on unload, restart, crash, or VPS shutdown, which is exactly the protection the journal exists to provide. Within a single session, capture and replay behave identically either way.

Other properties worth knowing:

- **The setting persists** in NV storage like the other settings, so reloading the module without arguments keeps it. Re-enable with `journal=on`.
- **`Reset` never re-enables it.** Resetting settings to defaults deliberately leaves the journal setting alone, so it cannot silently resume writing highlight context to disk. `Reset` says which mode is in effect.
- **An existing journal file is left alone**, not read and not deleted. `Status` reports that a leftover file exists, and `Compact` removes it on an explicit request.
- **`Status` shows the active mode** and the journal path in both modes.

This is a privacy and disk-hygiene control rather than a performance one. The most common reason to want it is that the journal persists real message text under the ZNC data directory, where it can reach backups and VPS snapshots; see [What it captures](#what-it-captures).

---

## Growth controls

Everything the module keeps is a function of live state: the journal after compaction holds exactly the records needed to rebuild open and pending events, and nothing else. Bounding what is in memory therefore bounds what is on disk.

Three settings do that, and each addresses a different way state grows.

### `max_events` — how many events wait for replay

Finalized events accumulate for as long as you stay detached. This is the dominant growth term: one event per highlight burst, with no natural end.

The default is **100** since 0.11.0. Past that, the oldest pending event is dropped to make room, so a long absence leaves you with the 100 most recent highlights rather than the first 100.

The binding constraint on this number is not memory or disk. A default-sized event is about 17 lines, which is roughly 7 KB in the journal and under 5 KB in memory, so even 1000 events would be about 7 MB on disk. The real limit is replay volume: 100 events is already around 1700 lines delivered to your client in one burst when you attach. That is a lot to read, and larger values get unwieldy fast.

### `max_event_lines` — how large one event can grow

Since 0.9.0, a highlight arriving inside an open event's `after` window [extends](#overlapping-highlights-extension) that event instead of starting a new one. Without a limit, a channel that keeps highlighting you keeps one event growing indefinitely.

The default is **100** total lines, counting `before` + trigger + `after`.

The cap limits extension only. An event always collects the full `before`/`after` window it started with, so no value of this setting can reduce the context you configured; setting it below the natural event size simply disables extension. When the cap stops an extension:

- the trigger is still recorded and still marked `>>>`, and still counts in `triggers=`
- the event's target does not grow
- the header gains `capped`
- once the event finalizes, the next highlight starts a fresh event

So a sustained flood produces several bounded events instead of one unbounded one, and no highlight is lost. 100 lines allows roughly eleven extensions past a default window, which covers any realistic conversation in which someone keeps addressing you; beyond that, splitting into separate events is easier to read anyway.

### `max_event_age` — how long events are kept

Pending events are otherwise kept until you replay them, however old they are.

The default is **off**, because the module exists to tell you what you missed, and discarding that by default would defeat it. If you are away for months, nothing expires unless you ask for it.

Set it when the concern is retention rather than size: with journaling on, pending events are message text sitting on disk. `max_event_age=30d` drops anything older than 30 days without replaying it. Expiry runs at load, as traffic arrives, and immediately before replay.

### Drops are always reported

Whenever events are shed, for either reason, the count is reported at the top of your next replay:

```text
note: 12 older highlight event(s) were dropped before this replay (10 to stay within max_events=100, 2 expired after max_event_age=30d).
```

`Status` shows the same counters between replays, and they reset once reported. Before 0.11.0, `max_events` dropped events silently.

### Resulting bound

```text
journal_bytes  ≈  (open_events + max_events) × max_event_lines × ~0.4 KB
```

`open_events` is bounded by your channel count: at most one event per channel is ever open at a time, because a highlight arriving while one is open extends it rather than starting another. With the defaults that is a worst case of roughly 4 MB, and typical use is far below it.

Setting `journal=off` removes the on-disk term entirely; the in-memory bound still applies.

### Compaction and large pending queues

A compacted event occupies about 10 journal lines at default settings — the `before` lines live inside the `B` record rather than as separate lines — so a queue of 100 pending events is roughly 1000 journal lines.

Up to 0.11.0, compaction was attempted whenever the journal exceeded a fixed 512 lines. Once live state alone exceeded that, a rewrite could not get below it, so the next appended line triggered another rewrite, and every eligible channel line cost a full journal rewrite with two `fsync` calls. Measured at stock defaults with 70 pending events: 10 rewrites per 10 channel lines.

Since 0.11.1 the trigger point is derived from what the last compaction actually achieved:

```text
next compaction at  max(512, 2 x lines written by the last compaction)
```

The journal must therefore at least double before being rewritten again, so a compaction costing *L* line-writes is always preceded by at least *L* appends. Total rewrite work is proportional to the number of appends rather than to the queue size, at every scale. The same measurement on 0.11.1 gives 0 rewrites per 10 lines, and appending 160 events across about 1000 journal lines triggers a single compaction.

The trade is that the file sits at up to twice the size of live state between compactions. With the default caps that peak is under 1 MB.

Below 512 lines nothing changes: small journals compact exactly as before.

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
/msg *status LoadMod --type=network highlightctx [before=8 after=8 require_ignore_drop=auto excludes=#chan1,#chan2,BadNick,*!*@evil.example]
```

### Supported load arguments

The module accepts space-separated `key=value` pairs.

#### `before=<count>`

Maximum number of earlier lines to snapshot from the per-channel ring when a highlight trigger happens.

#### `after=<count>`

Maximum number of later lines to collect after the trigger.

#### `journal=<off|on>`

Controls whether highlight events are journaled to disk.

- default is `on`
- `off` keeps everything in memory only; nothing is written to disk
- accepts the usual spellings: `on`/`off`, `1`/`0`, `yes`/`no`, `true`/`false`, `enable`/`disable`, `enabled`/`disabled`
- any other value fails the module load with an explicit error
- the value is stored in NV, so it survives a reload without arguments
- see [Disabling the journal](#disabling-the-journal) for the trade-offs

#### `require_ignore_drop=<off|on|auto>`

Controls `ignore_drop` integration mode.

#### `max_events=<count|0|off>`

Maximum number of finalized pending events to keep at once.

- `0` or `off` disables the cap
- **default is 100** (was disabled before 0.11.0)
- when the cap is reached, the oldest pending event is dropped to make room, and the count is reported at your next replay
- see [Growth controls](#growth-controls) for why 100

#### `max_event_lines=<count|0|off>`

Maximum total captured lines in a single event, counting `before` + trigger + `after`.

- `0` or `off` disables the limit
- default is `100`
- limits [extension](#overlapping-highlights-extension) only; an event always collects the full `before`/`after` window it started with, whatever the cap is set to
- a capped event shows `capped` in its replay header
- see [Growth controls](#growth-controls)

#### `max_event_age=<duration|off>`

Drops pending events older than the given age without replaying them.

- accepts `30d`, `12h`, `90m`, `3600s`, `2w`; a bare number is seconds
- `off` or `0` disables expiry
- **default is off**, so nothing is ever discarded by age unless you ask for it
- this is a retention control, not a size control; see [Growth controls](#growth-controls)

#### `excludes=<list>`

Comma-separated list of mixed exclusions. Each token is classified automatically:

- tokens starting with `#`, `&`, `+`, or `!` are treated as channels
- tokens containing `!` or `@` are treated as full `nick!ident@host` masks
- everything else is treated as a bare nickname mask

Wildcards `*` and `?` are supported in nick and hostmask tokens. RFC 1459 case folding is applied before storage and comparison.

### Example load commands

Minimal:

```text
/msg *status LoadMod --type=network highlightctx
```

With larger context windows:

```text
/msg *status LoadMod --type=network highlightctx before=12 after=20
```

Memory-only, with nothing written to disk:

```text
/msg *status LoadMod --type=network highlightctx before=12 after=20 journal=off
```

With channel exclusions:

```text
/msg *status LoadMod --type=network highlightctx excludes=#ops,#noise,#bots
```

With mixed channel + nick + hostmask exclusions:

```text
/msg *status LoadMod --type=network highlightctx excludes=#noise,BadNick,*!*@evil.example
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
/msg *highlightctx AddExclude <#channel|nick|mask>
/msg *highlightctx DelExclude <#channel|nick|mask|index>
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

Resets settings to defaults, but **does not discard pending/open events**, and **does not change the journal setting**.

---

## Command reference

### `Overview`

Prints a detailed built-in summary of how capture, replay, persistence, exclusions, and `ignore_drop` behavior work.

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
- number of excluded nicks/masks
- number of open events
- number of pending finalized events
- `max_event_lines` and `max_event_age`
- events dropped since the last replay, split by cause
- whether journaling is enabled or disabled, and — when disabled and a leftover journal file exists — a note naming its size
- journal path

When `auto` mode is unarmed but `ignore_drop` is loaded, `Status` explains that the cause is hook-order position and points you to `Rearm` and the module-reload / `znc.conf` reorder workarounds.

### `ReplayNow`

Finalizes open events as partial if needed, replays everything pending into `*highlightctx`, then clears delivered events.

### `SetBefore <count>`

Sets the pre-highlight context cap.

This also trims existing in-memory rings to the new size.

### `SetAfter <count>`

Sets the post-trigger context cap for **new events**.

Already-open events keep the cap they started with. This also applies to extensions: when a later highlight extends an open event, the extra window uses the cap that event started with, not the current `SetAfter` value.

### `SetMaxEvents <count|0|off>`

Controls the pending finalized-event cap.

If you lower the cap below the current number of pending events, the oldest pending events are dropped immediately.

### `SetMaxEventLines <count|0|off>`

Sets the maximum total captured lines per event (`before` + trigger + `after`). `0` or `off` disables the limit.

Unlike `SetAfter`, the value is evaluated live rather than captured per event, so it also governs events rebuilt from the journal. Note that while a client is attached there are never any open events — attaching finalizes and replays them — so in practice the change takes effect on the capture that follows your next detach.

If the value is below `before + 1 + after`, the module says so: extension is then effectively disabled, though events still collect their full configured window.

### `SetMaxEventAge <duration|off>`

Drops pending events older than the given duration without replaying them. Accepts `30d`, `12h`, `90m`, `3600s`, `2w`, or `off`.

Expiry runs at load, as traffic arrives, and immediately before replay. Dropped counts are reported at your next replay.

### `Reset`

Resets settings to compiled defaults:

- `before=8`
- `after=8`
- `max_events=100`
- `max_event_lines=100`
- `max_event_age=disabled`
- `require_ignore_drop=auto`
- all exclusions (channel and nick/mask) cleared

Pending and open events remain intact.

`max_events`, `max_event_lines`, and `max_event_age` are restored to their defaults (100, 100, and disabled).

The journal setting is deliberately **not** reset, so `Reset` can never silently resume writing highlight context to disk. The reply states the mode currently in effect.

### `AddExclude <#channel|nick|mask>`

Adds an exclusion. The token is classified as a channel, a nickname, or a full `nick!ident@host` mask based on its first character and the presence of `!` or `@`. See [Exclusions](#exclusions) for semantic details on the three kinds.

Masks may contain `*` and `?` wildcards and are matched after RFC 1459 case folding. Duplicate entries (compared on folded form) are reported rather than silently added. Empty masks and masks with embedded CR/LF/NUL are rejected with a reason.

### `DelExclude <#channel|nick|mask|index>`

Removes an exclusion. Accepts any of:

- a channel name
- an exact nick mask or hostmask (case-insensitive via RFC 1459 folding)
- a numeric index from the `ListExcludes` output

### `ListExcludes`

Lists all exclusions in a single numbered list. Channels appear first (alphabetically sorted), then nick and hostmask exclusions in insertion order. Each entry is tagged `[channel]`, `[nick]`, or `[mask]`. The numbering is the same one that `DelExclude <index>` uses.

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

With `journal=off`, there is nothing to compact, so `Compact` instead removes a journal file left over from an earlier session and reports how many bytes it deleted. If no such file exists, it says so and does nothing.

### `ClearPending`

Clears all open and pending events, clears volatile state, and compacts the journal. With `journal=off`, nothing is written to disk and the reply says so.

This is useful for cleanup and testing.

---

## Defaults

Compiled/runtime defaults in the source:

- `before = 8`
- `after = 8`
- `max_events = 100`
- `max_event_lines = 100`
- `max_event_age = 0` → disabled
- `require_ignore_drop = auto`
- excluded channels = empty
- excluded nicks/masks = empty

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
/msg *status LoadMod --type=network highlightctx before=6 after=12 excludes=#noise,BadNick,*!*@bridge.example require_ignore_drop=auto
```

This means:

- keep up to 6 lines before the trigger
- keep up to 12 lines after the trigger
- ignore `#noise` entirely
- still show `BadNick`'s and the bridge host's messages around real triggers, but do not let them start a new event on their own
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
4. the captured `after` lines, where any additional trigger that extended the event is also marked with `>>>`
5. a separator line between events

Conceptually it looks like this:

```text
[#channel] highlight event #42 (complete, before=3, after=4/4)
[#channel] <nick1> previous context
[#channel] <nick2> more context
[#channel] >>> <nick3> fauxpride: are you around?
[#channel] <nick4> follow-up line 1
[#channel] <nick5> follow-up line 2
```

The header's `after=<collected>/<target>` shows how many trailing lines were collected against how many the event needed. For an event with a single trigger the target is the `after` cap, and the header is unchanged from earlier versions.

An event that was extended by later highlights adds a `triggers=<n>` field, and every trigger line is marked. With `before=2 after=3`, a second highlight arriving as the second trailing line looks like this:

```text
[#channel] highlight event #43 (complete, before=2, after=5/5, triggers=2)
[#channel] <nick1> previous context
[#channel] <nick2> more context
[#channel] >>> <nick3> fauxpride: are you around?
[#channel] <nick4> follow-up line 1
[#channel] >>> <nick5> fauxpride, also this
[#channel] <nick6> follow-up line 3
[#channel] <nick7> follow-up line 4
[#channel] <nick8> follow-up line 5
```

If native server-time replay is available, the client can show those lines with their original timestamps. Otherwise the module prefixes UTC timestamps inline.

---

## Build and install

### Tests

The module ships with a live-ZNC test suite under [`tests/`](./tests/), which runs a real ZNC process against a fake IRC server and asserts on actual replay output. See [TESTING.md](./TESTING.md) for what it covers and how to run it.

### Build

Typical build command from the source header:

```text
znc-buildmod highlightctx8.cpp
```

This should produce `highlightctx8.so` for ZNC module loading. If you prefer a shorter module name (`highlightctx` instead of `highlightctx8`), rename the source file to `highlightctx.cpp` before building.

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
- `m_excluded` → set of lowercased channel names with channel-level exclusion
- `m_excluded_nicks` → vector of nick/mask exclusion rules with a folded mask and a `nick_only` flag

Each captured line stores:

- timestamp (`ts_sec`)
- kind (`T`, `N`, `A`)
- nick
- text
- an `extends_event` flag, set only on the copy stored in an event's `after` list when that line extended the event

Each event stores:

- unique event ID
- channel and lowercase channel key
- start timestamp
- `after` cap for that event
- `after` target (starts at the cap; grows when a later trigger extends the event)
- captured `before` lines
- trigger line
- captured `after` lines
- finalized flag
- partial flag

### Exclusion logic

The three exclusion kinds are checked at different points in the capture pipeline:

1. **Channel exclusion** is checked first thing in `HandleIncoming`. An excluded channel returns immediately — no context, no ring, nothing.
2. **Nick/mask exclusion** is checked only on the trigger path, when classifying a line that highlights your nick. That classification happens *before* `FeedOpenEvents`, because a qualifying trigger extends an open event instead of starting a new one. An excluded sender's line is classified as ordinary context: it is still fed into any already-open events on this channel, but it can neither extend an open event nor reach `StartEvent`. This is what produces the "excluded nick still contributes context but cannot start an event" semantic.
3. The channel ring buffer always receives every eligible (non-channel-excluded) line, including from excluded senders, so their messages remain available as `before` context for any future trigger.

Nick/mask matching uses an iterative star-backtracking wildcard engine (`wildmatch_folded`), operating on RFC 1459-folded strings. Both the stored masks and the per-message sender samples are folded once; subsequent comparisons are byte-level. Nick-only masks match the sender's nickname; masks containing `!` or `@` match the full `nick!ident@host`.

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

### Overlapping highlights (extension)

Since 0.9.0, a highlight that lands inside an event that is still collecting its `after` lines extends that event rather than starting a second one.

Before 0.9.0, every qualifying highlight started its own event. When a second highlight arrived inside the first event's `after` window, both events stayed open side by side, and every line inside both windows was replayed twice. With `before=8 after=8` and a second highlight four lines after the first, that meant two events sharing 13 lines. A burst of highlights produced one overlapping event per highlight.

The current behavior, for each incoming eligible line in a channel:

1. The line is classified as a **trigger** if it highlights your current nick, is not from your own nick, and the sender is not on the nick/mask exclusion list. These are exactly the lines that could start an event.
2. The line is fed into every open event on that channel as an `after` line, as before.
3. If the line is a trigger and the channel has an open event, the most recently started open event is **extended**:
   - the line is marked as an additional trigger, so it replays with `>>>`
   - the event's target becomes the trigger's position in `after` plus a full `after` window, using the cap the event started with
   - a durable `X` journal record is appended
4. Each open event whose `after` list has reached its target is finalized as complete.
5. If the line is a trigger and no open event was extended, a new event starts as described under [Event start behavior](#event-start-behavior).

Worked example with `before=8 after=8`, trigger `L0`, and a second trigger `L4`, the fourth line after it:

- `L0` starts the event with `before` = the 8 ring lines and a target of 8.
- `L4` is `after` line 4, so the target becomes 4 + 8 = 12.
- The event completes at `L12` as `after=12/12, triggers=2`, with no duplicated lines.

A third trigger at `L10` would push the target to 10 + 8 = 18.

Details worth knowing:

- **Extension is evaluated before the completion check.** A trigger arriving as the very last line of the window (for example the 8th `after` line with `after=8`) still extends the event instead of letting it finish and starting an overlapping one.
- **A highlight after completion starts a new event.** It takes a normal `before` snapshot, which can include lines from the event that just finished.
- **Excluded and self lines never extend.** Self lines and lines from nick/mask-excluded senders are ordinary context, just as they cannot start an event. Messages dropped by [`ignore_drop`](../ignore_drop/README.md) never reach `highlightctx` at all.
- **Channels are independent.** A highlight in one channel never extends an event in another.
- **`after=0` is unchanged.** Events finalize immediately and are never open, so each highlight is its own event.
- **Extensions are bounded by `max_event_lines`** (default 100 total lines) since 0.11.0. Before that they were unbounded. When the cap stops an extension the trigger is still marked, the event finishes its current window, and the next highlight after it finalizes starts a fresh event. A single extended event counts as one event towards `max_events`. See [Growth controls](#growth-controls).
- **Legacy journals converge to one event.** A journal written by 0.8.0 or earlier can contain several overlapping open events on the same channel. After upgrading, only the newest of them is extended by later triggers, and the older ones finish their original windows.

### Event start behavior

When a non-self line highlights your nick, the sender is not on the nick/mask exclusion list, AND the line did not extend an already-open event on that channel:

- a new event ID is allocated
- channel metadata is stored
- the current channel ring is copied into the event’s `before`
- the trigger line is stored
- a durable journal `begin` record is written
- if `after=0`, the event is finalized immediately
- otherwise the event is added to the open-event list for that channel

### Event feed behavior

For every later eligible line in the same channel (regardless of sender — excluded senders still feed existing open events):

- each open event for that channel receives the line in its `after` vector
- a durable journal `after` record is appended
- if the line is a qualifying trigger, the newest open event is extended and a durable journal `X` record is appended (see [Overlapping highlights (extension)](#overlapping-highlights-extension))
- once `after.size() >= after_target`, the event is finalized

### Finalization behavior

Finalization:

- marks the event finalized
- marks whether it is partial or complete
- moves it into the pending list
- appends a durable finalize record

If `max_events` is enabled and the pending count exceeds the cap, the oldest pending events are marked delivered and dropped, and a counter is incremented so the drop can be reported at the next replay. Expiry by `max_event_age` runs at the same point, plus at load and as traffic arrives.

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
- `X` → extend event: the `after` line at a given zero-based index was an additional trigger (added in 0.9.0)
- `F` → finalize event
- `D` → delivered event

An `X` record is always written immediately after the `A` record it refers to, and compaction preserves that order. It stores only the event ID and the index; the extended target is recomputed on load as index + 1 + the event's `after` cap from its `B` record. `X` records whose event is unknown, whose index does not refer to an already-loaded `after` line, or that are malformed are ignored.

Compatibility:

- Journals written by 0.8.0 and earlier contain no `X` records and load unchanged.
- 0.8.0 ignores unknown record types, so a 0.9.0 journal still loads after a downgrade. The extension marks and extended targets are lost, and open events finish at their original cap.
- With `journal=off` (0.10.0+) no records are written or read at all, and the format is unchanged. A journal written while enabled stays valid and is loaded again if you re-enable it — as long as `Compact` has not removed it in the meantime.

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
- `max_event_lines`
- `max_event_age_secs`
- `dropped_by_cap` and `dropped_by_age` — drop counters, cleared once reported at replay
- `require_ignore_drop_mode`
- legacy compatibility key `require_ignore_drop`
- `excluded_channels` — newline-separated lowercased channel names
- `excluded_nicks` — newline-separated RFC 1459-folded nick/host masks
- `version_marker`

Excluded channels and excluded nicks are stored in separate NV keys so 0.7.0 / 0.6.0 datadirs continue to load unchanged — only the channel key is read by older versions, and the new nick key is simply absent.

---

## Operational notes and caveats

- `before` and `after` are **caps**, not guarantees.
- If you attach quickly after a trigger, the event may replay as partial.
- Capture does not happen while attached.
- Excluded channels contribute neither triggers nor context.
- Excluded nicks/masks can still contribute context but cannot start events. This is the intentional semantic difference from channel exclusion.
- `SetAfter` affects only new events, not ones already open. Extensions of an open event also use the cap that event started with.
- A highlight inside an open event's `after` window extends that event instead of starting an overlapping one; see [Overlapping highlights (extension)](#overlapping-highlights-extension).
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
- `Reset` clears both channel and nick/mask exclusions, but leaves pending/open events intact.

### Known limitation: highlights at the moment of disconnect

There is a narrow timing window in which a highlight can arrive at ZNC effectively at the same moment your client disconnects. In that window, `highlightctx` may not capture the highlight even though you experienced it as having arrived "while I was detached." The most visible symptom is exactly what you'd expect: on reconnect, the highlight is sitting at the top of the channel's normal buffer playback, but no corresponding entry was ever created in `*highlightctx`.

The cause is a property of ZNC's event-handling, not of the module. `ShouldCaptureNow()` gates capture on `IsUserAttached()`, which is a snapshot of "is your client currently in the network's client list?" When a client disconnects, ZNC has to process the socket-close event before it removes the client from that list. If an IRC message from the server arrives during the brief interval between the socket actually closing and ZNC processing the close, `IsUserAttached()` still returns `true`, so `highlightctx` treats the message as having arrived while you were attached and skips it. The message still flows through ZNC's normal channel-buffer storage, which is why you still see it on reconnect.

The window is:

- typically very short (milliseconds to a few hundred milliseconds) for a clean client disconnect that delivers a TCP FIN/RST to ZNC
- potentially much longer (until TCP keepalives time out, which can be minutes) for a half-open connection caused by a hard process kill, a sudden network drop, or anything else that doesn't deliver a clean close to ZNC

In both cases the information is not lost. The highlight remains in ZNC's regular channel buffer and is visible to you as part of the usual buffer playback the next time you reattach. What is missing is only the focused replay entry that would otherwise appear in the `*highlightctx` window.

This is treated as an accepted limitation rather than a bug to be fixed. A fix would require `highlightctx` to read from ZNC's normal channel buffer at the detach transition and retroactively synthesize events for recent highlights — feasible, but a meaningful departure from the module's "do not inspect ZNC's normal buffer" design principle for what is, in practice, a fairly narrow operational window.

---

## Summary

`highlightctx` is a specialized detached-only ZNC network module for **capturing and replaying highlight-centered conversation context**.

Its main strengths are:

- independence from normal playback buffers
- focus on real missed highlights instead of general backlog
- durable journaling of active events (unless `journal=off`)
- replay into a dedicated module window
- native timestamp replay when the client supports it
- configurable `ignore_drop` integration with position-aware auto mode and on-demand re-check via `Rearm`
- exclusion control per channel, per nickname, and per `nick!ident@host` hostmask, with semantics matched to what each kind is actually for

If your goal is to preserve the context around important missed mentions while staying detached, without turning the module into a general-purpose logger, this design is aimed exactly at that use case.
