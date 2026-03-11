# ignore_drop

[`ignore_drop`](./src/ignore_drop.cpp) is a **network-scoped ZNC module** that performs **server-side ignore filtering inside ZNC itself**, before matching traffic reaches your IRC client and, where applicable, before it is replayed from ZNC buffers.

In practical terms, this module is aimed at users who want a cleaner, lower-noise ZNC experience than a client-side ignore can provide. Instead of merely hiding messages after they arrive in the client, `ignore_drop` can prevent matching traffic from ever being delivered to the client in the first place. That makes it especially useful for persistent ZNC setups where you stay connected through the bouncer, detach and reattach across devices, and want tighter control over both **live traffic** and **buffer playback**.

The module is intentionally designed around two different ignore semantics:

- **ALWAYS**: drop matching live traffic whether the network is attached or detached, and hide matching playback when buffers are replayed.
- **DETACHED**: drop matching live traffic **only while the network is detached**; hiding replayed playback for detached rules is optional and controlled separately.

This makes `ignore_drop` well suited to use cases such as:

- permanently suppressing noise from specific nicknames or hostmasks
- suppressing spam or nuisance traffic only while you are away/detached
- keeping playback cleaner when reattaching to a network
- centralizing ignore behavior in ZNC rather than reproducing it across multiple IRC clients
- applying ignores on a **per-network** basis instead of globally

---

## What the module does

At a high level, `ignore_drop` watches inbound IRC traffic coming from the server to ZNC, checks the sender against your configured masks, and decides whether the event should be passed through or halted.

It covers:

- channel messages
- private messages
- channel notices
- private notices
- channel actions (`/me`)
- private actions (`/me`)
- channel buffer playback when you attach
- private buffer playback when you attach

The filtering is based on the **sender only**.

A rule can be written as either:

- a **nick-only mask** such as `BadNick`
- a **full nick!ident@host-style mask** such as `*!ident@host.example`

Wildcard matching is supported with:

- `*` for any sequence of characters
- `?` for a single character

Matching is case-insensitive.

---

## Why this module exists / what it is aimed at

ZNC users often end up with one of two imperfect approaches:

1. use client-side ignores, which only affect one client and typically act after traffic has already been delivered
2. use coarse-grained ZNC or IRC-side solutions that do not distinguish well between attached and detached behavior

`ignore_drop` is aimed at the gap between those two approaches.

It gives you a **lightweight, network-local, sender-based ignore layer** that lives in the bouncer. That means:

- one ignore definition can affect every client connected through that ZNC network
- detached behavior can be different from attached behavior
- playback can be filtered more strictly than a normal client would manage on its own
- ignore logic remains consistent even when you switch devices or clients

It is especially appropriate for users who:

- keep ZNC connected 24/7
- use detached playback heavily
- want fine control over what is dropped live versus what is only hidden during replay
- prefer low-overhead modules with predictable behavior and simple command-driven administration

---

## Feature summary

- network module, not a global/user module
- per-network storage using ZNC NV
- case-insensitive wildcard masks
- separate semantics for **always** and **detached** rules
- optional playback hiding for detached rules
- supports both nick-only and full hostmask-style matching
- includes a built-in `Test` command for checking whether a sender would be dropped
- optimized around pre-lowered masks and separate internal buckets for faster matching paths
- contains fallback playback-line hooks for older ZNC playback paths

---

## Scope model

### `always`

An `always` rule means:

- matching live traffic is dropped whether you are attached or detached
- matching playback is also hidden when ZNC replays buffers

This is the stricter, permanent ignore mode.

### `detached`

A `detached` rule means:

- matching live traffic is dropped **only when that network is detached**
- matching playback is **not** hidden by default
- playback hiding for detached rules can be enabled globally for the module instance with:

```text
/msg *ignore_drop SetDetachedPlayback on
```

This mode is useful when you want to suppress noise while away, but still allow normal traffic while actively attached.

---

## Matching rules

### Nick-only masks

If a mask contains **neither `!` nor `@`**, the module treats it as a nick-only mask.

Examples:

```text
BadNick
spam*
?roll
```

These masks are matched only against the sender's **nickname**.

### Full masks

If a mask contains `!` or `@`, the module treats it as a full sender mask.

Examples:

```text
BadNick!*@*
*!~bot@host.example
*!*@*.example.net
```

These are matched against the sender in the form:

```text
nick!ident@host
```

### Case handling

All matching is case-insensitive.

Internally, masks are normalized to lowercase when stored, and incoming sender samples are lowercased before comparison.

### Wildcards

The module supports:

- `*` = any sequence of characters, including empty
- `?` = exactly one character

---

## Installation

### Build

From a shell on the machine where ZNC is installed:

```bash
znc-buildmod ignore_drop.cpp
```

This compiles the module into a loadable `.so` file for ZNC.

### Load

Because this module is explicitly a **network module**, load it on the target IRC network with:

```text
/msg *status LoadMod --type=network ignore_drop
```

### Help

Once loaded, built-in help is available with:

```text
/msg *ignore_drop help
```

---

## Usage

### Add an ignore

Syntax:

```text
/msg *ignore_drop Add <mask> [detached|always]
```

Notes:

- default scope is `always`
- `detached` can also be given as `d`
- `always` can also be given as `a`
- the code also accepts `both` as an alias for `always`

Examples:

```text
/msg *ignore_drop Add BadNick
/msg *ignore_drop Add spam* detached
/msg *ignore_drop Add *!~bot@host.example always
/msg *ignore_drop Add *!*@*.example.net d
```

### List configured ignores

```text
/msg *ignore_drop List
```

Example output shape:

```text
1) badnick [always|nick]
2) *!~bot@host.example [always|host]
3) spam* [detached|nick]
Detached playback: off
```

### Delete an ignore

You can delete either by numeric index or by exact mask text.

Syntax:

```text
/msg *ignore_drop Del <#|mask>
```

Examples:

```text
/msg *ignore_drop Del 2
/msg *ignore_drop Del BadNick
/msg *ignore_drop Del *!~bot@host.example
```

### Change the scope of an existing ignore

Syntax:

```text
/msg *ignore_drop SetScope <#> <detached|always>
```

Examples:

```text
/msg *ignore_drop SetScope 3 detached
/msg *ignore_drop SetScope 1 always
```

### Control detached-playback hiding

By default, detached rules affect **live detached traffic only**.

To also hide matching playback for detached rules:

```text
/msg *ignore_drop SetDetachedPlayback on
```

To disable that behavior again:

```text
/msg *ignore_drop SetDetachedPlayback off
```

### Test whether a sender would be dropped

Syntax:

```text
/msg *ignore_drop Test <nick!ident@host>
```

Example:

```text
/msg *ignore_drop Test trouble!~user@example.net
```

Example output:

```text
Live: DROP | Playback: pass
```

This is useful when verifying mask behavior without waiting for real traffic.

### Clear all ignores

```text
/msg *ignore_drop Clear
```

---

## Typical workflows

### 1. Permanent ignore for a known spam nick

```text
/msg *ignore_drop Add SpamNick always
```

Effect:

- live traffic from `SpamNick` is dropped at all times
- replayed buffer entries from `SpamNick` are also hidden

### 2. Suppress a noisy bot only while detached

```text
/msg *ignore_drop Add *!bot@host.example detached
```

Effect:

- when you are attached, traffic passes normally
- when you are detached, matching live traffic is dropped
- playback remains visible unless detached playback hiding is enabled

### 3. Hide detached noise both live and in playback

```text
/msg *ignore_drop Add *!*@*.annoying.example detached
/msg *ignore_drop SetDetachedPlayback on
```

Effect:

- detached live traffic is dropped
- playback entries matching detached rules are also hidden

### 4. Verify behavior before relying on it

```text
/msg *ignore_drop Test badguy!~x@evil.example
```

Use this after adding or adjusting rules to confirm the current outcome.

---

## Command reference

### `Add`

```text
Add <mask> [detached|always]
```

Adds a new ignore entry.

Behavior details:

- default scope is `always`
- nick-only vs full-mask handling is determined automatically from the mask text
- no de-duplication is performed; duplicate entries are currently possible

### `Del`

```text
Del <#|mask>
```

Deletes an entry by index or by exact mask.

Behavior details:

- deleting by mask is case-insensitive because the input is normalized before comparison
- if duplicates exist, deleting by exact mask removes the first matching stored entry encountered internally

### `List`

```text
List
```

Prints all configured ignore entries and the current detached-playback setting.

### `Clear`

```text
Clear
```

Removes all ignore entries for the current network.

### `SetScope`

```text
SetScope <#> <detached|always>
```

Moves an existing entry to the requested scope.

Behavior details:

- the entry is removed from its current internal bucket and appended to the target bucket
- this can change numbering/order shown by `List`

### `SetDetachedPlayback`

```text
SetDetachedPlayback <on|off>
```

Controls whether `detached` entries also hide playback.

### `Test`

```text
Test <nick!ident@host>
```

Evaluates a sample sender against the current rule set and reports what would happen for:

- live traffic
- playback

---

## Implementation details

This section describes how the module works internally, based directly on the source code.

### Module type

The module registers itself as a **network module**:

```text
Info.AddType(CModInfo::NetworkModule);
```

That is important because:

- each IRC network gets its own module instance
- rules are stored per network
- detached/attached decisions are evaluated in the context of that specific network

### Internal data model

The code splits ignores into four internal vectors:

- `m_alwaysNick`
- `m_alwaysHost`
- `m_detachedNick`
- `m_detachedHost`

This design separates rules along two axes:

1. scope: `always` vs `detached`
2. mask class: nick-only vs full hostmask-style

That helps keep the decision paths simple and avoids repeatedly reclassifying masks at runtime.

Each stored entry contains:

- `mask_lc`: the lowercased mask
- `nick_only`: whether the mask lacks both `!` and `@`
- `scope`: `ALWAYS` or `DETACHED`

### Normalization strategy

Masks are lowercased when added or loaded from storage.

Incoming sender samples are also lowercased before matching. This gives case-insensitive behavior without repeatedly normalizing stored masks.

### Live traffic path

The live hooks all funnel into `LiveMaybeDrop()`.

That function:

1. builds a lowercase nickname sample
2. builds a lowercase full `nick!ident@host` sample
3. checks `always` rules first
4. only if detached rules exist, computes whether the network is attached
5. if the network is detached, checks detached rules
6. returns `HALT` on a match, otherwise `CONTINUE`

This is a sensible fast-path design:

- `always` matches are checked first because they do not depend on attach state
- attach-state lookup is skipped entirely when there are no detached rules

### Playback path

Playback hooks funnel into `PlaybackMaybeDrop()`.

Its behavior is:

- `always` rules always hide playback
- `detached` rules hide playback only if `m_hideDetachedPlayback` is enabled

This keeps live filtering semantics and playback semantics intentionally distinct.

### Fallback playback hooks

In addition to modern buffer-play message hooks, the source also contains fallback methods:

- `OnChanBufferPlayLine`
- `OnPrivBufferPlayLine`

These parse the line prefix into a temporary `CNick` and then reuse the same playback decision logic.

That means the module is written with some compatibility awareness for older playback hook paths.

### Wildcard engine

Mask matching is performed by `wildmatch_ci()`.

Notable characteristics:

- supports `*` and `?`
- case-insensitive character comparison
- includes a fast path for exact equality before doing the heavier work
- uses a dynamic-programming table to evaluate wildcard matches

This is straightforward and correct, though it also means matching cost grows with mask length and input length. In normal ignore-list usage that is typically acceptable, especially because the surrounding code reduces unnecessary work in other places.

### Storage format

The module uses ZNC NV storage with two keys:

#### `masks`

All ignore entries are stored as newline-separated records in the form:

```text
A|mask
D|mask
```

Where:

- `A` = `always`
- `D` = `detached`

Examples:

```text
A|badnick
A|*!~bot@host.example
D|spam*
D|*!*@*.example.net
```

#### `detached_playback`

Stored as:

```text
0
```

or

```text
1
```

Where `1` means detached rules should also hide playback.

### Load behavior

When the module loads:

- `detached_playback` is read first
- masks are loaded from NV
- entries are split back into the four internal vectors

### Ordering and indexing behavior

`List` presents entries in this flattened order:

1. always nick-only
2. always full-mask
3. detached nick-only
4. detached full-mask

That ordering matters because:

- `Del <number>` uses this numbering
- `SetScope <number> ...` also targets entries using this numbering

If you change scope with `SetScope`, the entry is reinserted into the target bucket, usually at the end of that bucket, so later list numbering may shift.

---

## Performance notes

The source comments explicitly aim for low overhead, and the implementation reflects that in several ways:

- masks are lowercased once, not on every comparison
- masks are split into scope/type buckets up front
- `always` rules are checked before detached logic
- attached-state lookup is deferred until it is actually needed
- a direct equality fast path exists before full wildcard evaluation

For the intended use case—reasonable ignore lists on a ZNC network—this is a practical and efficient design.

---

## Limitations and behavior notes

A few details are worth knowing before you rely on the module heavily:

- filtering is based only on the sender mask, not channel name or message content
- masks are stored normalized to lowercase; original case is not preserved in storage/listing
- duplicate rules are not automatically prevented
- deleting by exact mask removes the first matching internal entry, not every duplicate
- `detached` live behavior depends on whether the **network** is attached, not on per-channel attachment concepts
- nick-only masks do **not** match ident/host; they only match nicknames
- full masks are matched against `nick!ident@host`

---

## Practical advice for repository use

If you are committing this to a public or shared Git repository, you may also want to add:

- a `LICENSE` file if one does not already exist
- a minimal example section in the repo root if the repository contains multiple modules
- build notes matching your ZNC installation layout if your environment is non-standard

This README intentionally focuses on the module behavior visible from the current source file.

---

## Minimal quick start

```text
/msg *status LoadMod --type=network ignore_drop
/msg *ignore_drop Add BadNick
/msg *ignore_drop Add *!~bot@host.example detached
/msg *ignore_drop SetDetachedPlayback on
/msg *ignore_drop List
/msg *ignore_drop Test badguy!~x@evil.example
```

---

## Summary

`ignore_drop` is a focused, low-overhead ZNC network module for sender-based ignore filtering with two distinct modes:

- **always** for strict, persistent suppression of live traffic and playback
- **detached** for away-time suppression, with optional playback filtering

Its main strengths are:

- per-network isolation
- server-side enforcement inside ZNC
- simple command-based management
- predictable scope semantics
- implementation choices that keep runtime work modest

For users who want more control than a client-side ignore, especially in always-on ZNC workflows, it is a clean and practical fit.