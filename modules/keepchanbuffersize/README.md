# keepchanbuffersize

Preserve per-channel `BufferSize` across quick `PART`/`JOIN` cycles.

## Overview

[`keepchanbuffersize`](./src/keepchanbuffersize) is a ZNC network module for one specific pain point: when you have manually customized channel buffer sizes, those per-channel settings can be lost when a channel object is torn down and recreated during a fast part/rejoin sequence.

That matters most in real-world operator workflows where you may deliberately `/cycle` a channel — or `PART` and quickly `JOIN` again — to inspect spam-on-join behavior, check for join drones, or verify how a channel behaves when you enter it fresh as an op. In those cases, you usually want the channel to come back exactly as before from a buffer-retention perspective. This module is designed to keep that per-channel buffer sizing intact.

In other words, this module does **not** try to change how ZNC buffering works globally. It focuses on **preserving channel-specific buffer sizes that were already explicitly set**, so those values survive the temporary teardown/recreation of the channel object during fast leave/rejoin scenarios.

The module is implemented as a **network module**, not a global module, so behavior and remembered settings are scoped per IRC network. The source targets **ZNC 1.9.1** and is built around multiple hooks so it remains reliable even when different clients or scripts produce slightly different timing and message patterns. fileciteturn2file0

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
- also handle broader leave flows such as `JOIN 0`, server-side `PART`, and `KICK` fallbacks fileciteturn2file0

## How it works

At a high level, the module keeps a small remembered mapping of channel name → desired buffer size in module NV storage.

### Remember phase

When you leave a channel, the module tries to capture the current explicit buffer size **before** ZNC drops the in-memory channel object. To make this robust, it hooks both:

- raw user messages (`OnUserRawMessage`) for early `PART`/`JOIN` visibility
- structured user hooks (`OnUserPartMessage`, `OnUserJoinMessage`) as an additional safety net

This is important for ultra-fast `/cycle` behavior, where timing can matter. The raw hook is specifically useful because it can see the `PART` early enough to remember the channel’s current explicit setting before teardown happens. fileciteturn2file0

### Restore phase

When you rejoin, the module restores the remembered value once the channel exists again. Its primary restore point is `OnJoinMessage`, where the server-side join confirms that the channel object should be present. It also performs an opportunistic early restore during user-side join handling if the channel already exists by that point. fileciteturn2file0

### Fallback coverage

The module also remembers settings when:

- the server sees you `PART`
- you are `KICK`ed from a channel
- you issue `JOIN 0`, in which case it snapshots all current channels before they may be torn down

That gives it broader coverage than only handling a simple manual `/cycle`. fileciteturn2file0

### What it stores — and what it does not

The module only persists values for channels that have an **explicit** buffer count set. If a channel is just using defaults and does not have its own explicit buffer count, the module does not force a stored override for it. In that case it removes any remembered value and lets defaults continue to apply normally. fileciteturn2file0

This is a good design choice because it keeps the module focused on preserving intentional per-channel customization rather than inventing new policy.

## Intended use cases

Typical situations where this module is useful include:

- **Operator spam checks**: you `/cycle` to see what a normal join looks like or to inspect automated spam delivered on join.
- **Drone detection / join-noise checks**: you briefly leave and re-enter to watch how a channel behaves when rejoined fresh.
- **Per-channel tuning**: some channels need deeper retained history than others, and you do not want those explicit values lost after a quick rejoin.
- **Client/script diversity**: you use clients, aliases, or scripts that may issue raw IRC commands with very tight timing.

The module’s own comments explicitly call out fast `/cycle` handling and robustness across clients/scripts as core goals. fileciteturn2file0

## Compatibility

- **Module type:** network module
- **Target ZNC version:** 1.9.1 fileciteturn2file0

Because this is a network module, load it on each network where you want this behavior.

## Commands

The module exposes the following commands through the module window:

```text
/msg *keepchanbuffersize Status
/msg *keepchanbuffersize Enable
/msg *keepchanbuffersize Disable
/msg *keepchanbuffersize List
/msg *keepchanbuffersize Set <#chan> <lines>
/msg *keepchanbuffersize Forget <#chan>
```

These commands are defined directly in the module source. fileciteturn2file0

### `Status`

Shows whether automatic remember/restore behavior is currently enabled and how many channels are remembered.

### `Enable`

Enables automatic remember/restore behavior and immediately applies remembered values to any currently existing channel objects on that network. Stored data is preserved. fileciteturn2file0

### `Disable`

Disables automatic remember/restore behavior. Stored remembered values are kept, but the module stops automatically capturing/restoring until re-enabled. fileciteturn2file0

### `List`

Lists remembered channels and the stored buffer size for each one. If nothing is stored, the module reports that no channels are remembered. fileciteturn2file0

### `Set <#chan> <lines>`

Manually stores a buffer size for a channel. If that channel already exists in the current network, the module also tries to apply the value immediately. If the apply fails — for example because of limits — it still keeps the stored value and tells you the immediate apply did not succeed. fileciteturn2file0

### `Forget <#chan>`

Removes the stored remembered buffer size for that channel. fileciteturn2file0

## Usage examples

### Basic load

```text
/znc LoadMod keepchanbuffersize
```

### Load disabled

```text
/znc LoadMod keepchanbuffersize enabled=0
```

The module supports `enabled=0|1` style load arguments, and also accepts `off/on/false/true` equivalents. If no stored setting exists, it defaults to enabled. fileciteturn2file0

### Check status

```text
/msg *keepchanbuffersize Status
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

## Behavior details and implementation notes

### Network-scoped storage

The module uses NV storage with keys of the form:

```text
buf:#channel
```

Channel names are normalized to lowercase before key generation so storage remains stable and lookup is consistent. The enabled/disabled state is also stored in NV using the `Enabled` key. fileciteturn2file0

### Existing channels on load

When the module loads and is enabled, it immediately iterates current channel objects and reapplies any remembered values that already exist in storage. This helps after reloads or reconnect-related situations where channels already exist at module load time. fileciteturn2file0

### Multiple-target support

The parser handles comma-separated channel targets for commands such as:

- `PART #a,#b`
- `JOIN #a,#b`

It also handles `JOIN 0` as a special case by snapshotting all current channels before they may be removed. fileciteturn2file0

### Restore safety

If a remembered value is empty or resolves to `0`, the module does nothing. If a channel already has the correct explicit buffer count, it also does nothing. If `SetBufferCount()` fails, it emits a warning rather than silently pretending success. fileciteturn2file0

### “Explicit setting only” policy

One subtle but important design choice is that the module only stores a channel’s value if `HasBufferCountSet()` is true. That prevents the module from converting default behavior into unnecessary persistent overrides. fileciteturn2file0

## Operational notes

- This module preserves **per-channel buffer size settings**, not the actual message contents themselves.
- It is most useful when you already use explicit per-channel `BufferSize` values.
- It is especially helpful for fast leave/rejoin workflows where channel teardown would otherwise discard the channel-specific setting.
- Since it is a network module, remember to load it separately on each network where you want this behavior.

## Limitations

- The module can only restore values that were explicitly stored or manually set.
- If ZNC refuses an attempted buffer size because of limits or policy, the module warns but cannot force the apply. fileciteturn2file0
- The module is targeted at ZNC 1.9.1; other versions may require small adjustments depending on API differences. fileciteturn2file0

## Why this module exists

There are plenty of IRC workflows where a quick leave/rejoin is intentional rather than accidental. Operators often do this to inspect what happens on join, validate anti-spam behavior, or check whether drones or scripted noise appear to a fresh entrant. In those moments, losing a carefully tuned per-channel buffer size is just friction.

`keepchanbuffersize` exists to remove that friction: you can briefly leave, come back, and keep the per-channel buffer depth you intentionally configured before the cycle.

## Source summary

The module implements:

- early remember logic on user raw `PART`
- structured user `PART`/`JOIN` hooks as extra protection
- restore on server `JOIN`
- fallback remember on server `PART` and `KICK`
- `JOIN 0` snapshot support
- command handlers for status, enable/disable, list, manual set, and forget
- NV-backed per-channel remembered state keyed by normalized channel name fileciteturn2file0

## License

Add the license that matches the rest of your repository.
