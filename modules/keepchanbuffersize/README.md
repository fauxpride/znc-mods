# keepchanbuffersize

Preserve per-channel `BufferSize` across quick `PART`/`JOIN` cycles.

**Current version:** 1.2.0 — see [`CHANGELOG.md`](./CHANGELOG.md).

## Overview

[`keepchanbuffersize`](./src/keepchanbuffersize.cpp) is a ZNC network module for one specific pain point: when you have manually customized channel buffer sizes, those per-channel settings can be lost when a channel object is torn down and recreated during a fast part/rejoin sequence.

That matters most in real-world operator workflows where you may deliberately `/cycle` a channel — or `PART` and quickly `JOIN` again — to inspect spam-on-join behavior, check for join drones, or verify how a channel behaves when you enter it fresh as an op. In those cases, you usually want the channel to come back exactly as before from a buffer-retention perspective. This module is designed to keep that per-channel buffer sizing intact.

In other words, this module does **not** try to change how ZNC buffering works globally. It focuses on **preserving channel-specific buffer sizes that were already explicitly set**, so those values survive the temporary teardown/recreation of the channel object during fast leave/rejoin scenarios.

The module is implemented as a **network module**, not a global module, so behavior and remembered settings are scoped per IRC network. The source targets **ZNC 1.9.1** and is built around multiple hooks so it remains reliable even when different clients or scripts produce slightly different timing and message patterns.

## What problem this solves

Without a helper like this, a quick `/cycle` can cause you to lose a channel's explicit `BufferSize` setting if the underlying channel object disappears and is later recreated. That is especially annoying when:

- you are an op and temporarily leave/rejoin to inspect spam or automated join noise
- you use channel-specific buffer sizes rather than one-size-fits-all defaults
- you want a channel to come back with the same retained history depth after a fast rejoin
- you use clients or scripts that can trigger very fast `PART`/`JOIN` flows

This module preserves the remembered value and reapplies it when the channel comes back.

## Design goals

The module’s source expresses a few clear goals:

- preserve per-channel `SetBuffer` / `BufferSize` values across fast `/cycle`
- remain robust across different clients and scripting styles
- use both raw and structured hooks for better coverage
- restore on join at the point where the channel object definitely exists
- also handle broader leave flows such as `JOIN 0`, server-side `PART`, and `KICK` fallbacks

## How it works

At a high level, the module keeps a small remembered mapping of channel name → desired buffer size in module NV storage. It is always active while loaded — there is no on/off toggle (see *Active while loaded* below).

### Remember phase

When you leave a channel, the module tries to capture the current explicit buffer size **before** ZNC drops the in-memory channel object. To make this robust, it hooks both:

- raw user messages (`OnUserRawMessage`) for early `PART`/`JOIN` visibility
- structured user hooks (`OnUserPartMessage`, `OnUserJoinMessage`) as an additional safety net

This is important for ultra-fast `/cycle` behavior, where timing can matter. The raw hook is specifically useful because it can see the `PART` early enough to remember the channel’s current explicit setting before teardown happens.

### Restore phase

When you rejoin, the module restores the remembered value once the channel exists again. Its primary restore point is `OnJoinMessage`, where the server-side join confirms that the channel object should be present. It also performs an opportunistic early restore during user-side join handling if the channel already exists by that point.

### Fallback coverage

The module also remembers settings when:

- the server sees you `PART`
- you are `KICK`ed from a channel
- you issue `JOIN 0`, in which case it snapshots all current channels before they may be torn down

That gives it broader coverage than only handling a simple manual `/cycle`.

### What it stores — and what it does not

The module only persists values for channels that have an **explicit** buffer count set. If a channel is just using defaults and does not have its own explicit buffer count, the module does not force a stored override for it. In that case it removes any remembered value and lets defaults continue to apply normally.

This is a good design choice because it keeps the module focused on preserving intentional per-channel customization rather than inventing new policy.

### Active while loaded

In versions up to 1.1.x, the module had an on/off toggle (`Enable`/`Disable` commands and an `enabled=0|1` load argument) controlling whether remember/restore behavior ran. That toggle was removed in 1.2.0 because a loaded-but-disabled state was a subtle foot-gun: the module appeared present and its commands worked, but `/cycle` would silently not preserve the buffer. From 1.2.0 onward the rule is simpler — if the module is loaded, it is active; to stop it, use `UnloadMod keepchanbuffersize`. Stored per-channel values persist across unload/reload, so you do not lose state.

## Intended use cases

Typical situations where this module is useful include:

- **Operator spam checks**: you `/cycle` to see what a normal join looks like or to inspect automated spam delivered on join.
- **Drone detection / join-noise checks**: you briefly leave and re-enter to watch how a channel behaves when rejoined fresh.
- **Per-channel tuning**: some channels need deeper retained history than others, and you do not want those explicit values lost after a quick rejoin.
- **Client/script diversity**: you use clients, aliases, or scripts that may issue raw IRC commands with very tight timing.

The module’s own comments explicitly call out fast `/cycle` handling and robustness across clients/scripts as core goals.

## Compatibility

- **Module type:** network module
- **Target ZNC version:** 1.9.1

Because this is a network module, load it on each network where you want this behavior.

## Commands

The module exposes the following commands through the module window:

```text
/msg *keepchanbuffersize Status
/msg *keepchanbuffersize Version
/msg *keepchanbuffersize List
/msg *keepchanbuffersize Set <#chan> <lines>
/msg *keepchanbuffersize Forget <#chan>
```

These commands are defined directly in the module source.

### `Status`

Shows how many channels have a remembered buffer size in this network's module storage.

### `Version`

Prints the module version (for example, `keepchanbuffersize version 1.2.0`). Useful for confirming which build is loaded.

### `List`

Lists remembered channels and the stored buffer size for each one. If nothing is stored, the module reports that no channels are remembered.

### `Set <#chan> <lines>`

Manually stores a buffer size for a channel. The channel name must begin with a valid channel prefix (`#`, `&`, `+`, or `!`, or whatever the server advertises in `CHANTYPES`). The line count must be a positive integer and must not exceed ZNC's configured `MaxBufferSize`; invalid input is rejected without writing anything to storage. If the channel already exists in the current network, the module also tries to apply the value immediately.

### `Forget <#chan>`

Removes the stored remembered buffer size for that channel. As with `Set`, the channel name must begin with a valid channel prefix.

## Usage examples

### Basic load

```text
/znc LoadMod keepchanbuffersize
```

The module is active from the moment it is loaded; there is no additional "enable" step to perform.

### Stop the module

```text
/znc UnloadMod keepchanbuffersize
```

Unloading is the supported way to stop the module from acting. Per-channel values you have stored are preserved and will be picked up again the next time the module is loaded.

### Check status

```text
/msg *keepchanbuffersize Status
```

### Check the loaded version

```text
/msg *keepchanbuffersize Version
```

### Store a channel explicitly

```text
/msg *keepchanbuffersize Set #channel 200
```

### List remembered channels

```text
/msg *keepchanbuffersize List
```

### Forget one channel

```text
/msg *keepchanbuffersize Forget #channel
```

### Example operator workflow

1. Set a larger explicit buffer on a channel you moderate.
2. Use a quick `/cycle` or manual `PART` + `JOIN` to inspect spam-on-join, drone patterns, or other fresh-join behavior.
3. Let the module preserve the per-channel value while the channel object disappears and reappears.
4. Rejoin with the same remembered per-channel buffer size restored.

That operator-focused workflow is exactly where this module makes the most sense.

## Installation

### 1. Save the source

Place `keepchanbuffersize.cpp` in your ZNC modules source/build location.

### 2. Build the module

Build it the same way you build other out-of-tree ZNC C++ modules for your local ZNC installation.

A common pattern is:

```bash
znc-buildmod keepchanbuffersize.cpp
```

That should produce a loadable module shared object for your ZNC environment.

### 3. Install the compiled module

Copy the resulting module file into the appropriate per-user or site module directory used by your ZNC installation.

### 4. Load it on the desired network

```text
/znc LoadMod keepchanbuffersize
```

## Upgrading from 1.1.x or 1.0.x

Upgrading to 1.2.0 is a drop-in replacement: install the new `.so`, `UnloadMod` the module, then `LoadMod` it again. Two things happen automatically on the first 1.2.0 load:

- **The old `Enabled` NV key is deleted.** It controlled the toggle that no longer exists. No action required on your part.
- **Channel keys whose stored form predates RFC 1459 casemap normalization are rewritten** (this was introduced in 1.1.0 and carries over unchanged). This is a no-op for channel names that use only ASCII characters.

Per-channel remembered buffer values are preserved across the upgrade. The only behavior change you may notice is that passing `enabled=0` (or `enabled=1`, `enabled=off`, etc.) to `LoadMod` now prints a deprecation notice in the module window; the argument is otherwise ignored. Previously-removed `Enable` and `Disable` commands now return `Unknown command!`; use `UnloadMod` / `LoadMod` instead.

## Behavior details and implementation notes

### Network-scoped storage

The module uses NV storage with keys of the form:

```text
buf:#channel
```

Channel names are normalized using **RFC 1459 casemapping** before key generation so storage remains stable and lookup is consistent across sessions. RFC 1459 treats `[`, `]`, `\`, and `~` as the uppercase forms of `{`, `}`, `|`, and `^`, which is the historical IRC default and matches how most IRC servers compare identifiers. For channels whose names use only ASCII letters and digits this produces exactly the same result as plain lowercase conversion, so it is fully backward compatible with the common case.

### Legacy key migration

On module load, any previously stored key that was written under plain ASCII lowercasing is transparently rewritten to its RFC 1459 form. The migration preserves the stored buffer value, runs at most once per key, and skips any key whose RFC 1459 form already has a value to avoid clobbering live data. In practice, this migration is a no-op for ASCII-only channel names.

### Input validation

The `Set` command validates input before it writes anything to NV storage:

- the channel argument must begin with a valid channel prefix (server-advertised `CHANTYPES` if available, otherwise `#`, `&`, `+`, `!`)
- the line count must be digits only — a leading sign or any non-digit character is rejected
- the line count must be greater than zero
- the line count must not exceed ZNC's configured global `MaxBufferSize`

The `Forget` command applies the same channel-prefix validation.

### Existing channels on load

When the module loads, it immediately iterates current channel objects and reapplies any remembered values that already exist in storage. This helps after reloads or reconnect-related situations where channels already exist at module load time.

### Multiple-target support

The parser handles comma-separated channel targets for commands such as:

- `PART #a,#b`
- `JOIN #a,#b`

It also handles `JOIN 0` as a special case by snapshotting all current channels before they may be removed.

### Restore safety and forcing

If a remembered value is empty or resolves to `0`, the module does nothing. If a channel already has the correct explicit buffer count, it also does nothing. When applying a remembered value, the module passes `bForce = true` to `CChan::SetBufferCount()` — matching how webadmin and the config loader set per-channel buffers — so that values above `MaxBufferSize` (which are legally configurable through webadmin) are still restorable after a channel object is torn down. The up-front validation in `Set` ensures that any value the module persists has already cleared the `MaxBufferSize` check, so forcing on restore only ever re-applies values that were previously accepted by ZNC.

### “Explicit setting only” policy

One subtle but important design choice is that the module only stores a channel’s value if `HasBufferCountSet()` is true. That prevents the module from converting default behavior into unnecessary persistent overrides.

## Operational notes

- This module preserves **per-channel buffer size settings**, not the actual message contents themselves.
- It is most useful when you already use explicit per-channel `BufferSize` values.
- It is especially helpful for fast leave/rejoin workflows where channel teardown would otherwise discard the channel-specific setting.
- Since it is a network module, remember to load it separately on each network where you want this behavior.

## Limitations

- The module can only restore values that were explicitly stored or manually set.
- If ZNC refuses an attempted buffer size because of limits or policy, the module warns but cannot force the apply.
- The module is targeted at ZNC 1.9.1; other versions may require small adjustments depending on API differences.

## Why this module exists

There are plenty of IRC workflows where a quick leave/rejoin is intentional rather than accidental. Operators often do this to inspect what happens on join, validate anti-spam behavior, or check whether drones or scripted noise appear to a fresh entrant. In those moments, losing a carefully tuned per-channel buffer size is just friction.

`keepchanbuffersize` exists to remove that friction: you can briefly leave, come back, and keep the per-channel buffer depth you intentionally configured before the cycle.

## Source summary

The module implements:

- early remember logic on user raw `PART`
- structured user `PART`/`JOIN` hooks as extra protection
- restore on server `JOIN` with `bForce = true`, matching webadmin and config-loader semantics
- fallback remember on server `PART` and `KICK`
- `JOIN 0` snapshot support
- command handlers for status, version, list, manual set, and forget
- NV-backed per-channel remembered state keyed by RFC 1459 casemap-normalized channel name, with one-time migration from the legacy ASCII-lowercase form
- one-time cleanup of the legacy `Enabled` NV key on load (no-op for installs that never had it)
- deprecation notice for the legacy `enabled=0|1` load argument

## License

Add the license that matches the rest of your repository.
