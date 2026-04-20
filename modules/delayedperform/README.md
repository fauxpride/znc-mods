# delayedperform

[`delayedperform`](./src/delayedperform.cpp) is a ZNC network module that runs post-connect IRC commands after configurable delays.

It is designed for the cases where a normal `perform`-style burst is too aggressive: reconnecting to a network, rejoining a large number of channels, talking to services immediately after connect, or otherwise sending many commands at once can trigger server-side flood protection, join throttles, or other rate limits. Instead of firing everything immediately, `delayedperform` lets you store commands per network and schedule them to run a few seconds later.

This makes it especially useful in situations where sending too many messages at once via `perform` can cause throttling, particularly when you need to rejoin many channels after reconnecting.

Current version: **1.1.1** — see [`CHANGELOG.md`](./CHANGELOG.md).

---

## What this module is for

The default idea behind a traditional “perform” setup is simple: once the IRC connection comes up, send a list of commands immediately.

That works well on many networks, but it becomes less ideal when:

- the network enforces aggressive connect-time flood limits
- joining many channels at once can trigger join throttling
- you want to delay some commands until the connection has fully settled
- you want some commands to run immediately, but others only after a few seconds
- you want the configuration stored per network inside ZNC rather than living in a client-side startup script

`delayedperform` addresses those cases by attaching a delay to each configured command. On every successful IRC connection, the module schedules one-shot timers and sends the stored commands when those timers fire.

In other words, this is a **delayed, persistent, per-network perform queue**.

---

## Key features

- Run multiple IRC commands automatically after connect
- Store commands per network via ZNC NV storage
- Set a **global default delay** for new entries
- Override the delay **per command**
- Accept both raw IRC commands and common slash-style commands
- Expand `%nick%` at send time using the **current IRC nick**, with strict IRC nick-grammar validation before splicing
- Mark individual entries as **secret** so their text is hidden in `List` and in the module's echo output (useful for `NickServ IDENTIFY`, `OPER`, etc.)
- Reject commands containing CR, LF, or NUL characters before storing them (and again before sending)
- Clean up scheduled timers on disconnect so reconnects do not pile up stale jobs

---

## Typical use cases

### Rejoining many channels more gently

If you normally use `perform` to join a long list of channels, all those `JOIN`s may be sent too quickly. With `delayedperform`, you can spread them out:

```text
/msg *delayedperform Add 3 /join #chan1
/msg *delayedperform Add 5 /join #chan2
/msg *delayedperform Add 7 /join #chan3
/msg *delayedperform Add 9 /join #chan4
```

### Delaying service messages until a few seconds after connect

Use `AddSecret` for lines that contain credentials so their text is not echoed back to any attached client or included in `List` output:

```text
/msg *delayedperform AddSecret 4 /ns IDENTIFY hunter2
/msg *delayedperform Add 6 /cs OP #channel mynick
```

### Sending commands that depend on the current nick

```text
/msg *delayedperform Add 5 /msg BotServ setuser *%nick%* enabled
```

If ZNC had to connect with a fallback nick first and later regained your preferred nick before the timer fires, `%nick%` expands to whatever your actual IRC nick is at that moment — provided the nick is RFC-legal. If the current nick contains characters outside IRC nick grammar (space, `:`, `,`, `@`, CR, LF, etc.), the module refuses to splice it in and logs `Skipped (invalid nick for expansion): ...` instead of sending a potentially split or reframed line.

---

## Module behavior at a glance

When the IRC network connects:

1. the module clears any previously scheduled timers
2. it loads all stored command entries for that network
3. it creates one one-shot timer per command
4. when a timer fires, the command is sent with `PutIRC()` and the entry is removed from the module's internal tracking
5. the module logs the action to the module window (secret entries log as `Ran: [hidden]`)

When the IRC network disconnects:

- all queued timers are removed

This keeps reconnect behavior clean and predictable.

---

## Installation

The exact installation flow depends on how you build third-party ZNC modules on your system, but the usual pattern is:

```bash
znc-buildmod delayedperform.cpp
```

That produces a module shared object which you can place in your ZNC modules directory, then load on a specific network.

### Loading the module

From ZNC:

```text
/znc LoadMod delayedperform
```

Or, depending on how you manage modules, load it on the target network through your normal ZNC module workflow.

This is a **network module**, so its configuration is intended to be separate per IRC network.

### Checking the installed version

Once loaded, the module's version is visible from three places:

```text
/msg *delayedperform Version
/msg *delayedperform Help
/msg *status ListMods
```

`Version` returns something like `delayedperform version 1.1.1`. `Help` shows the version in its header. `ListMods` shows the description, which ends in `v1.1.1`.

---

## Command reference

Interact with the module through its module window:

```text
/msg *delayedperform Help
/msg *delayedperform Version
/msg *delayedperform SetDelay <seconds>
/msg *delayedperform Add [seconds] <irc-or-slash-command>
/msg *delayedperform AddSecret [seconds] <irc-or-slash-command>
/msg *delayedperform List
/msg *delayedperform Del <index|all>
/msg *delayedperform Clear
```

### `Help`

Shows built-in usage help, including the current module version.

### `Version`

Prints the module version on its own line, for scripting or quick identification.

```text
/msg *delayedperform Version
```

### `SetDelay <seconds>`

Sets the global default delay used when `Add` or `AddSecret` is called **without** an explicit per-command delay.

```text
/msg *delayedperform SetDelay 5
```

### `Add [seconds] <irc-or-slash-command>`

Adds a delayed command.

- If `seconds` is supplied, that entry uses its own delay.
- If `seconds` is omitted, the current global default delay is used.
- Commands can be raw IRC lines or supported slash-style shorthands.
- The command text is rejected if it contains CR, LF, or NUL.

Examples:

```text
/msg *delayedperform Add 5 JOIN #znc
/msg *delayedperform Add /join #znc
/msg *delayedperform Add 10 MODE #channel +o mynick
```

Added entries are echoed to the module window in the form:

```text
Added [2]: delay=5s, cmd=JOIN #znc
```

### `AddSecret [seconds] <irc-or-slash-command>`

Like `Add`, but the entry is flagged as secret. The behavior is identical at fire time (the full decoded command is still sent to IRC), but the module hides the text everywhere else:

- the confirmation when you add it shows `cmd=[hidden]` instead of the actual text
- `List` shows `[hidden]` in the command column
- the `Ran:` echo that fires when the timer runs says `Ran: [hidden]` instead of the full line
- the `Skipped (not connected)` / `Skipped (contains control chars)` / `Skipped (invalid nick for expansion)` messages also say `[hidden]`

Use this for any entry that contains credentials:

```text
/msg *delayedperform AddSecret 4 /ns IDENTIFY hunter2
/msg *delayedperform AddSecret 6 /oper myname verystrongpassword
```

The stored command is still subject to the same control-character rejection as `Add`.

Note: the underlying storage is still base64 on disk; `AddSecret` is about keeping credentials out of the module's visible output and logs, not about encrypting them at rest. See [Operational notes](#important-operational-notes).

### `List`

Shows:

- the current global default delay
- all configured entries
- each entry's index, delay, and decoded command text — or `[hidden]` if the entry was added with `AddSecret`, or `[corrupt entry]` if the stored value could not be decoded

```text
/msg *delayedperform List
```

### `Del <index|all>`

Deletes a specific stored command by index, or everything if `all` is used. Remaining entries are renumbered to a contiguous `0..N-1` range and the secret flag on each entry is preserved through the rewrite.

```text
/msg *delayedperform Del 2
/msg *delayedperform Del all
```

### `Clear`

Alias for deleting all entries.

```text
/msg *delayedperform Clear
```

---

## Supported slash-command shorthands

The module can normalize many client-style commands into raw IRC before storing them.

Supported shorthands include:

- `/msg`
- `/notice`
- `/join`
- `/part`
- `/quit`
- `/nick`
- `/topic`
- `/mode`
- `/kick`
- `/invite`
- `/ctcp`
- `/me` *(requires an explicit target)*
- `/whois`
- `/away`
- `/oper`
- `/raw`
- `/quote`
- service aliases:
  - `/ns` → `NickServ`
  - `/cs` → `ChanServ`
  - `/hs` → `HostServ`
  - `/ms` → `MemoServ`
  - `/os` → `OperServ`
  - `/bs` → `BotServ`

Unknown slash commands fall back to a generic conversion:

- the leading `/` is removed
- the command verb is uppercased
- the rest of the line is preserved

For example:

```text
/cap ls
```

becomes:

```text
CAP ls
```

---

## Variable expansion

The module supports:

- `%nick%` — expands to the current IRC nick **at send time**, if and only if that nick conforms to IRC nick grammar

Expansion does **not** happen when the command is added. It happens right before the timer sends the line. That means the module uses your live nick at execution time, which is useful after reconnects or fallback-nick scenarios.

Before substituting, the module validates the current IRC nick against IRC nick grammar (letters, digits, and the RFC 2812 "special" characters ``[ ] \ ` _ ^ { | }`` plus hyphen). If the nick contains anything outside that set — space, `:`, `,`, `@`, CR, LF, or anything else that could split the IRC line — the command is **not** sent, and the module logs:

```text
Skipped (invalid nick for expansion): <the stored command>
```

In practice your nick will almost always be fine; this check is defense in depth against malformed/hostile server input.

Example:

```text
/msg *delayedperform Add 6 /msg SomeBot hello from %nick%
```

---

## Important operational notes

### Delays are relative to connect time

Each command's delay is measured from the moment the IRC connection comes up.

- a command with delay `3` runs about 3 seconds after connect
- a command with delay `10` runs about 10 seconds after connect
- commands that share the same delay are scheduled for roughly the same moment

### This is not an automatic sequential queue

The module does **not** automatically space entries one after another unless you configure different delays yourself. If you add five commands all with delay `5`, they will all be scheduled for about 5 seconds after connect. Use increasing per-command delays if you want staggering.

### Commands are stored per network

This is a network module and uses ZNC NV storage, so entries are meant to follow the network they were configured on.

### Control-character filtering

Stored commands are rejected if they contain CR, LF, or NUL. This is enforced at `Add`/`AddSecret` time (before persisting) and re-checked just before the command is sent. In practice, IRC clients strip these before sending to ZNC, but the check gives defense in depth against manually-crafted NV values, imported configs, or future code paths that might accept templated input.

### `%nick%` substitution is validated, not trusted

The module refuses to splice a non-conforming nick into a stored command. This catches the theoretical case where a misbehaving or hostile IRC server sends back a nick containing space, `:`, CR/LF, or other characters that would split the resulting IRC line or reframe its semantics. In that situation the whole command is skipped rather than sent with the malformed nick substituted in.

### Timers are cleaned up on disconnect and when they fire

If the IRC connection drops before the delayed commands fire, the module removes any pending timers. On the next connection, it schedules a fresh set from stored configuration. When a timer does fire, the module also removes its internal reference to that timer so no stale pointers hang around between connect cycles.

### `AddSecret` hides the text from the module window, not from disk

The on-disk storage is still base64-encoded, which is obfuscation, not encryption. Anyone with filesystem access to your ZNC's data directory can recover the stored command text. `AddSecret` is aimed at the much more common exposure vector: the module's `PutModule()` output is broadcast to every IRC client currently attached to your ZNC user, and typical IRC clients log module-window text to disk by default. `AddSecret` keeps sensitive commands out of that channel.

### Duplicate behavior is your responsibility

If you also use another startup mechanism that sends the same commands on connect, you may end up running both. In practice, you should decide whether a given command belongs in:

- normal `perform`
- `delayedperform`
- client-side startup automation

and avoid duplicating the same line in multiple places.

---

## Example workflows

### 1. Replace a bursty channel-join perform

Instead of doing this all at once:

```text
JOIN #chan1
JOIN #chan2
JOIN #chan3
JOIN #chan4
JOIN #chan5
```

you can configure:

```text
/msg *delayedperform SetDelay 0
/msg *delayedperform Add 2 /join #chan1
/msg *delayedperform Add 4 /join #chan2
/msg *delayedperform Add 6 /join #chan3
/msg *delayedperform Add 8 /join #chan4
/msg *delayedperform Add 10 /join #chan5
```

### 2. Keep service commands slightly behind the initial connect burst

```text
/msg *delayedperform AddSecret 3 /ns IDENTIFY hunter2
/msg *delayedperform Add 5 /msg ChanFix REQUEST #channel
```

### 3. Use a global default delay for convenience

```text
/msg *delayedperform SetDelay 5
/msg *delayedperform Add /join #help
/msg *delayedperform Add /join #ops
/msg *delayedperform Add /msg SomeBot status
```

All three entries inherit the default `5` second delay unless you later add one with an explicit override.

---

## Implementation details

This section documents how the module works internally.

### Network lifecycle hooks

The module uses:

- `OnLoad()` to load the saved global delay from NV storage
- `OnIRCConnected()` to schedule delayed commands
- `OnIRCDisconnected()` to remove scheduled timers

### Timer model

For each stored command, the module creates a separate `CTimer` subclass instance (`CCmdTimer`).

Each timer is:

- one-shot
- labeled with a `dp#<index>` style identifier
- configured with the stored delay in seconds
- carries the entry's secret flag so its `RunJob` callback can emit the right module output

When a timer fires, it calls back into the module, executes the stored command, and then calls `ForgetTimer(this)` so the module drops its internal pointer before ZNC deletes the timer. This avoids dangling pointers in the module's timer-tracking vector across disconnect/reconnect cycles.

### Persistent storage model

The module uses ZNC NV storage for persistence.

Keys include:

- `global_delay`
- `cmd.<index>`

Each command entry is stored in one of two formats:

```text
<delay>|<flags>|<base64>       # v1.1+ format (written by this version)
<delay>|<base64>               # v1.0 legacy format (still read for compatibility)
```

The `<flags>` segment is a short string of single-character flags. Currently defined flags:

- `s` — entry is secret (its text is hidden in `List` and module output)

Unknown flag characters are preserved through rewrites but otherwise ignored, leaving room to add more flags in future versions without breaking older configurations.

Examples conceptually look like:

```text
global_delay = 5
cmd.0 = 3||Sk9JTiAjY2hhbjE=
cmd.1 = 7|s|UFJJVk1TRyBOaWNrU2VydiA6SURFTlRJRlkgaHVudGVyMg==
cmd.2 = 9|Sk9JTiAjbGVnYWN5-cm9tZS12MQ==      # legacy v1.0 entry, no flags segment
```

Commands are base64-encoded before storage so the module can safely persist raw IRC lines without having to worry about embedded spaces or formatting quirks in the NV value. Base64 is an encoding, not an encryption — see the note on `AddSecret` above.

### Entry ordering

Stored `cmd.<index>` keys are loaded, index-sorted, and then presented in ascending order.

If you delete an entry, the module rewrites the list into a contiguous `0..N-1` index range, preserving each remaining entry's flags.

### Command normalization

When you add a command, the module checks whether it starts with `/`.

- If it does not, it is treated as raw IRC.
- If it does, the module attempts to normalize it into the equivalent raw IRC line before saving it.

This gives you the convenience of client-like command syntax while keeping the actual stored representation IRC-native.

### Send-time variable expansion

Before a line is sent, the module runs variable expansion over the stored command text.

At present, the implemented variable is:

- `%nick%`

The value comes from `GetNetwork()->GetIRCNick().GetNick()`, so the module deliberately uses the current nick at execution time rather than a stale nick captured when the command was configured.

If the stored command does not contain `%nick%`, the nick is never fetched and never validated — non-`%nick%` commands pay no overhead and can never be blocked by nick validation. If the command does contain `%nick%`, the current nick must conform to IRC nick grammar (letters, digits, and ``[ ] \ ` _ ^ { | }`` plus hyphen); otherwise the module skips the send.

### Actual send path

After expansion, the module verifies the final line contains no CR/LF/NUL and only then sends it with `PutIRC()`. It also logs what it did to the module window using `PutModule()`, using `[hidden]` in place of the command text if the entry was marked secret.

### Connectivity guard

Before sending, the module checks that the network still exists and is IRC-connected. If not, it skips the send and logs that the command was not run.

---

## Design rationale

`delayedperform` exists because “run these commands after connect” is not always the same thing as “run all these commands immediately.”

On some networks and setups, a raw post-connect burst can be undesirable. For example:

- reconnect storms
- many-channel rejoin scenarios
- networks with stricter flood controls
- services or bots that respond more reliably a few seconds after connect

A per-network, ZNC-side delayed command runner gives you a middle ground:

- more automatic than manual client scripting
- more controlled than immediate `perform`
- more persistent than ad-hoc one-off reconnect commands

---

## When to use this module vs normal perform

Use normal `perform` when:

- everything is safe to send immediately
- the command list is short
- your network does not care about the burst

Use `delayedperform` when:

- you want connect-time automation but not all at once
- you need to spread out joins or service commands
- you are trying to avoid server-side throttling
- rejoining many channels can trigger penalties or join limits

A common split is:

- keep simple, harmless setup in normal `perform`
- move potentially bursty or timing-sensitive commands into `delayedperform`

---

## Limitations and caveats

- Delays are second-based, not sub-second.
- Entries with the same delay fire at about the same time.
- The module does not inspect server feedback to dynamically slow itself down.
- It does not automatically retry failed commands.
- It is a scheduler, not a full queue manager with rate adaptation.
- On-disk storage is base64-encoded, not encrypted.

So the pacing is explicit and manual: you decide which commands run when.

---

## Practical recommendations

- Use small staggered delays instead of one large pile at the same second.
- Group the most important commands first.
- Use `AddSecret` for anything containing a password or token, so the text does not appear in `List` or in any attached client's module window.
- Prefer service aliases or slash shorthands when that makes maintenance easier.
- Revisit your timings if the network changes its flood policy or your channel count grows.

---

## Summary

`delayedperform` is a small but practical ZNC network module for people who want the convenience of `perform`-style automation without the all-at-once burst. It is aimed squarely at reconnect scenarios where pacing matters, especially if immediate channel rejoins or service commands can run into throttling.
