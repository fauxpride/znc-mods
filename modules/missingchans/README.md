See [CHANGELOG.md](./CHANGELOG.md) for version history and release notes, and [TESTING.md](./TESTING.md) for the reproducible test suite.

# missingchans

[`missingchans`](./src/missingchans.cpp) is a ZNC network module that verifies whether the channels you **expect** to be on are actually joined on the **IRC server**, and can optionally retry joins for anything that is still missing.

It is designed for reconnect scenarios where your configured channel list and your real server-side membership can drift apart for a while after connect — especially on networks where joining certain community channels depends on first authenticating to a **non-service bot**, obtaining a **community-specific cloak**, or waiting for some other post-connect condition to complete.

With the proper configuration, this helps make sure your channels are eventually joined even if there is a **temporary server issue**, a **community bot outage**, or a race where the first burst of joins happens before the network is ready to admit you everywhere.

In practical terms, this module is useful when:

- some channels only become joinable **after** a custom auth bot grants access;
- you rely on a bot-driven cloak, vhost, or account state before certain channels will accept you;
- the initial post-connect join burst can fail partially during outages, lag, or service instability;
- you want ZNC to **verify** the final joined state instead of assuming the first attempt worked.

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
4. combines that WHOIS reply with what ZNC itself knows is joined and with any self-targeted numeric `443` confirmations;
5. compares **expected** vs **known joined**;
6. if anything is missing and `JoinMissing` is on, schedules a bounded sequence of delayed join attempts;
7. immediately before each attempt, rebuilds the expected and missing sets from current state, and skips the attempt if nothing is missing any more;
8. re-checks with WHOIS three seconds after each attempt.

It can also optionally trigger ZNC's built-in `perform` module before a retry attempt, which is useful if your recovery flow depends on re-sending authentication or helper-bot commands before retrying the missing joins.

This is connection recovery, not perpetual channel monitoring. A cycle starts when IRC connects, or when you run `RUN`. Attaching a client, reading `STATUS`, or a PART/KICK does not start a new cycle. Loading or reloading the module on an already connected network does not run recovery either.

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
- Server-side channel verification via self-`WHOIS`, supplemented by ZNC's live joined state, so a channel the WHOIS reply happens to omit is not treated as missing.
- Optional automatic rejoin of missing channels.
- Optional multi-attempt retry logic with increasing delay. Each pending attempt re-checks current membership and settings before it acts.
- Optional integration with ZNC's built-in `perform` module via `perform Execute`, on the originating network only.
- Optional `StopPerformOn` sentinel channel to suppress further `perform` retries once a key channel is confirmed joined.
- Reuses channel keys from ZNC channel configuration when re-sending `JOIN`.
- Case-insensitive (ASCII) channel comparison.
- User-mode-aware WHOIS parsing that follows the server's advertised `PREFIX` and `CHANTYPES`: channels you are voiced (`+`), op (`@`), halfop (`%`), admin (`&`), owner (`~`) or hold an unusual rank in are recognized as the same channel.
- The WHOIS exchange the module performs as part of its own verification is hidden from attached IRC clients. User-initiated `/whois` is unaffected, including multi-target requests, error replies, and use alongside `route_replies`.
- A 30-second WHOIS watchdog: a verification that fails or never completes causes no repair action.
- Extra fallback learning from a self-targeted numeric `443` (“already on channel”).
- Optional `Debug` view in `STATUS` with recovery diagnostics.
- **In-place updates (r12):** `/msg *status UpdateMod missingchans` loads a replaced binary without restarting ZNC, whatever other modules are loaded and in whatever order. See [Updating in place](#updating-in-place-updatemod).
- Persistent configuration via ZNC module NV storage.

---

## Module identity

- **Module name:** `missingchans`
- **Source file:** [`src/missingchans.cpp`](./src/missingchans.cpp)
- **Current build marker in source:** `2026-10-04+r12 (in-place UpdateMod; r11 debug status and r10 recovery fixes retained)`
- **Build and test target:** ZNC 1.10.3. Recompile for the ZNC installation that will load the module. Older ZNC releases have not been validated since r10.

The module advertises itself as:

> Verify/join missing channels by comparing expected list vs WHOIS (with retry + perform support).

---

## Installation

Run shell commands as the Unix account that runs ZNC.

### Build

Build with the installed `znc-buildmod` in a separate directory; it writes its output into the current directory. No extra compiler or linker flags are needed, in-place updates included.

```sh
mkdir -p ~/znc-module-build/missingchans-r12
cd ~/znc-module-build/missingchans-r12
cp /path/to/modules/missingchans/src/missingchans.cpp .
znc-buildmod missingchans.cpp
```

This produces `missingchans.so` for the ZNC installation whose `znc-buildmod` you ran.

### Install

For the standard user module directory, back up the existing binary and install with a **rename**, so a file that ZNC has mapped into memory is never overwritten in place:

```sh
cp -p ~/.znc/modules/missingchans.so ~/.znc/modules/missingchans.so.pre-r12-backup
install -m 0644 missingchans.so ~/.znc/modules/missingchans.so.new
mv -f ~/.znc/modules/missingchans.so.new ~/.znc/modules/missingchans.so
```

Adjust `~/.znc` if your ZNC uses a different data directory. Overwriting the installed file directly (`cp` onto it) can crash a running ZNC, with or without this module's update support. Skip the backup line on a first install.

### Load

From a connected IRC client attached to the target network:

```text
/msg *status LoadMod missingchans
```

Or load it on the specific network through the web interface or your usual module workflow. It is a network module.

On load, the module restores its saved settings from NV storage.

### Updating in place (UpdateMod)

After installing a new build as above, run **once** from an IRC client connected as a ZNC administrator:

```text
/msg *status UpdateMod missingchans
```

`UpdateMod` unloads and reloads **every loaded instance of this module, across all ZNC users and networks**. It cancels their pending retries and resets in-memory diagnostics. It does not disconnect IRC and does not start a recovery cycle. Then check each relevant network:

```text
/msg *missingchans VERSION
/msg *missingchans STATUS
```

`VERSION` prints two lines. The first names the build that is running. The second says how it was loaded:

| `Loader:` line | Meaning |
|---|---|
| `resident loader <build> (in-place UpdateMod enabled)` | This build is the copy ZNC loaded first, and later updates will load in place. |
| `updated in place by resident loader <build>` | A replaced file was loaded without a restart. |
| `WARNING: the installed missingchans.so was not loaded (...)` | The replacement could not be used. The build that ran before is still running on this network; the reason is in the parentheses and may mention the temporary file name described below. Fix the file and run `UpdateMod` again, or restart ZNC. |

The warning is also sent to attached clients when the instance starts, because `UpdateMod` does not display module load messages.

**The first upgrade from r11 or older may still need one ZNC restart.** Earlier builds have no in-place loader. Depending on which modules are loaded after `missingchans`, glibc may keep the old file mapped, and `UpdateMod` then reports `Done` while the old revision keeps running. If `VERSION` still shows the old revision after `UpdateMod`, restart ZNC using your usual service/process method; that reconnects your IRC networks and starts normal connection recovery. From then on, r12 is the resident loader and updates apply in place.

What r12 changes about loading:

- Every time an instance is created — by `UpdateMod`, `ReloadMod`, or `LoadMod` — the module runs **the build currently installed on disk**, not whichever copy happens to be in memory.
- `ReloadMod missingchans` on one network therefore gives that network the installed build while other networks keep theirs until they are reloaded. `UpdateMod` keeps all networks on the same build.
- A replacement must be a module built for the running ZNC version; anything else is refused with a warning, as above.
- Loading a replacement briefly creates a hidden hard link, `.missingchans-load-<pid>-<n>`, next to the installed file and removes it straight away. The ZNC process needs permission to do that; see [Permissions for in-place updates](#permissions-for-in-place-updates). Without it the update is refused with a `cannot create a temporary link` warning.
- The loader logic itself is the one from the first r12-or-later build loaded since ZNC started. A later fix to the loader takes effect after the next ZNC restart; fixes anywhere else in the module apply in place.

### Permissions for in-place updates

To load a replaced build, the ZNC process must be able to do three things to the temporary link in the module directory:

1. create it as a hard link to `missingchans.so` (`link()`);
2. read and map it (`dlopen()`);
3. delete it (`unlink()`).

Ordinary file permissions allow this when ZNC runs as the account that owns its module directory, which is normal for `~/.znc/modules`. A shared, root-owned module directory such as ZNC's system-wide one usually does not allow it.

A mandatory access control system can block it even when file permissions allow it. **AppArmor** is the common case: creating a hard link needs the `l` (link) permission, separate from read and write, and profiles written for ZNC rarely grant it. The `link()` call then fails with `Permission denied`, and `VERSION` shows:

```text
Loader: WARNING: the installed missingchans.so was not loaded (cannot create a temporary link next to /home/<user>/.znc/modules/missingchans.so: Permission denied). Still running <build>. Fix the file and run UpdateMod again, or restart ZNC.
```

Nothing is lost when this happens: every network keeps running the build that ran before.

Not every refusal is logged. An explicit `deny` rule is silent unless the profile audits it. A logged denial may appear in `/var/log/audit/audit.log` (when `auditd` is running) or in `journalctl -k`, rather than in `dmesg`.

To allow in-place updates without granting anything wider, add rules like these inside the profile that confines ZNC. Use the profile's `local/` include file if it has one, so package updates don't overwrite the change.

```text
  owner @{HOME}/.znc/modules/.missingchans-load-* mrw,
  owner link @{HOME}/.znc/modules/.missingchans-load-* -> @{HOME}/.znc/modules/missingchans.so,
```

- The first rule lets ZNC map and then delete the temporary link (`m` and `r` for loading, `w` for deletion).
- The second allows creating the link, and only when the target is `missingchans.so`. No other module or file gains link permission.
- Adjust the paths if ZNC's data directory is not `~/.znc` (`--datadir`), or if the profile does not use the `@{HOME}` tunable.
- If the profile has a `deny` rule covering the module directory, it takes precedence over these rules and has to be narrowed for these paths.

Check the syntax and reload the profile. The running ZNC process picks up the reloaded profile without a restart:

```sh
sudo apparmor_parser -QT -K /etc/apparmor.d/<znc-profile>   # syntax check only
sudo apparmor_parser -r /etc/apparmor.d/<znc-profile>        # reload
sudo aa-status | grep -i znc                                 # still in enforce mode?
```

Then run `UpdateMod missingchans` again and check `VERSION`. Changing the profile is optional: restarting ZNC always loads the installed build.

Other confinement mechanisms can have the same effect. Examples are SELinux policies, and systemd sandboxing that makes the module directory read-only (`ProtectHome=read-only`, `ReadOnlyPaths=`). The requirement is the same: link, map, and delete within the module directory. If granting that is not acceptable, update by restarting ZNC.

### Rollback

Install the backed-up binary the same way (copy to a temporary sibling, rename it over `missingchans.so`), run `UpdateMod missingchans` once, and check `VERSION` on the affected networks. All saved settings remain compatible with r9, r10, and r11; older builds ignore the newer `debug` key.

When r12 is the resident loader, rolling back to r11 or older also takes effect in place. Such a build cannot report when it is no longer in use, so its code stays mapped until the next ZNC restart; this costs a few hundred kilobytes of memory and nothing else. Rolling forward again works in place.

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
- `Debug = off`

These defaults are intentionally conservative:

- verification is delayed long enough for many post-connect flows to settle;
- auto-joining is disabled by default so you can observe behavior first;
- retrying `perform` is also disabled by default;
- `STATUS` shows the compact r9 view unless you ask for diagnostics.

All seven r9 settings, their NV keys and defaults are unchanged; `debug` (r11) is the only addition and is off when absent. No migration is required.

---

## Commands

All module commands are issued through the module query window or via `/msg` to the module.

Examples below assume the module window is `*missingchans`.

### Help

```text
/msg *missingchans HELP
```

Shows built-in help, the build marker, and all setting names. `h` and `?` are aliases.

### Version

```text
/msg *missingchans VERSION
```

Prints the build marker and, since r12, a `Loader:` line. See [Updating in place](#updating-in-place-updatemod) for what that line means.

### Status

```text
/msg *missingchans STATUS
```

Shows the current configuration and run state. It never queries WHOIS, invokes perform, resets timers or suppression, or starts a repair.

With `Debug` off (the default), `STATUS` contains exactly these nine rows, in the r9 order: `Delay`, `JoinMissing`, `ExpectedMode`, `RetryPerform`, `Retries`, `RetryStep`, `StopPerformOn`, `PerformSuppressed(this run)`, and `VerifiedJoined(count)`.

`VerifiedJoined(count)` is **only the self-`443` cache**, not your joined-channel count. Zero is normal: IRC servers commonly ignore a duplicate JOIN silently instead of answering `443`.

With `Debug` on, `STATUS` adds `Debug = ON` and these diagnostic rows:

| Field | Meaning |
|---|---|
| `Network` | Originating network. |
| `Phase`, `Attempt` | Current recovery phase and scheduled/last attempt number. |
| `LiveJoined(count)` | Channels currently marked joined by ZNC. |
| `WhoisJoined(last snapshot)` | Channels in the latest parsed WHOIS reply. |
| `Missing(last check)` | Cached missing set, also pruned by self JOINs; not a new verification. |
| `LastAction` | Last recorded repair decision. |
| `LastAttemptMissing` | Channels considered missing immediately before the last attempted repair. |
| `LastAttemptTriggeredPerform` | Whether that attempt invoked perform. |
| `PerformCalls(this connection)` | Number of Execute invocations by this instance, including ones with an empty perform list. |
| `LastPerformSource`, `LastPerformAt` | Network/user module selection and UTC timestamp of the last invocation. |

Diagnostics are kept in memory and reset on disconnect or reload; they are recorded whether or not `Debug` is on. No perform text, passwords, or channel keys are included. Channel and network names are.

### Show expected channels

```text
/msg *missingchans SHOW
```

Builds and displays the expected-channel set according to the current `ExpectedMode`. It does not query WHOIS, invoke perform, reset suppression, or start a cycle.

### Run a verification immediately

```text
/msg *missingchans RUN
```

Starts a new verification/recovery cycle now and replaces any pending timers. Depending on your settings, it can lead to real perform and JOIN commands. While the module's own WHOIS is outstanding, a second `RUN` is rejected instead of overlapping it. After a WHOIS that timed out, `RUN` is refused until the late reply has drained; reconnect if it never arrives.

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
- `debug`

`SET` with no key prints a usage line listing all eight.

---

## Settings reference

Boolean settings (`joinmissing`, `retryperform`, `debug`) accept `on`, `1`, `yes`, `true`, `enable`, or `enabled`, case-insensitively, as on. Any other value, including an empty one, means off; prefer the explicit `on`/`off` forms.

### `SET delay <seconds>`

Controls how long the module waits **after IRC connection** before starting the verification cycle. Minimum 1.

Example:

```text
/msg *missingchans SET delay 180
```

Use a longer delay when your post-connect flow depends on slow bot responses, cloaks, or other network-side processing. A change applies to future connection cycles, not to one already waiting.

---

### `SET joinmissing <on|off>`

Enables or disables automatic retry-join behavior for channels found to be missing.

Example:

```text
/msg *missingchans SET joinmissing on
```

When off, the module will still detect and report missing channels, but it will not attempt to fix them. Turning it off also cancels an attempt that is already scheduled, when that attempt comes due.

---

### `SET expectedmode <all|config|enabled>`

Controls how the module builds its **expected** channel set from ZNC's channel list. `everything`, `cfg`, and `autojoin` are accepted as aliases of `all`, `config`, and `enabled`; any other value means `all`.

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

If enabled, the module will locate ZNC's built-in `perform` module and call:

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

The module looks for `perform` first at the **network** level, then at the **user** level. `Execute` replays the whole selected perform list, not only authentication commands. Turning the setting off also applies to an attempt that is already scheduled.

If no `perform` module is loaded, it reports that and still sends the JOINs.

---

### `SET retries <N>`

Sets the maximum number of join-attempt rounds per cycle. Minimum 1.

Example:

```text
/msg *missingchans SET retries 5
```

Each attempt can optionally re-run `perform` and then sends `JOIN` for all channels still missing at that moment. Lowering the limit also cancels a scheduled attempt that would exceed it.

---

### `SET retrystep <seconds>`

Controls the base retry spacing.

Attempt `i` waits:

```text
i * retrystep
```

seconds, with a minimum wait of one second, so `retrystep 0` does not create a zero-interval timer. The multiplication is protected against overflow.

So with `retrystep = 20`:

- attempt 1 runs after 20s
- attempt 2 runs after 40s
- attempt 3 runs after 60s

Example:

```text
/msg *missingchans SET retrystep 30
```

This increasing wait pattern is useful for giving services, bots, or network state time to recover. Changing `RetryStep` does not move an attempt that is already scheduled; it applies to attempts scheduled afterwards.

For `Delay=300`, `RetryStep=300`, `Retries=10`, the waits are 5, 10, 15, …, 50 minutes. With prompt replies, the last attempt comes roughly 4 hours 40 minutes after connection. These are increasing delays, not ten retries at fixed five-minute intervals.

---

### `SET stopperformon <#channel|off>`

Sets a sentinel channel that, once known to be joined, suppresses further `perform Execute` calls for the current cycle.

Example:

```text
/msg *missingchans SET stopperformon #communityhub
```

Why this matters:

- maybe your `perform` script contacts a helper bot;
- that helper bot only needs to succeed once;
- once a key channel proves that access is working, there is no need to keep re-running `perform` on later retry rounds.

The module considers the sentinel joined when any of these says so:

- ZNC's live channel state;
- the module's `WHOIS` channel list;
- a self-targeted `443` confirmation.

Suppression is **latched for the cycle**: once triggered it stays set even if the sentinel later parts or the setting is cleared. A new `RUN` or connection cycle resets it and evaluates current membership again. It suppresses perform only, not JOIN attempts for other missing channels.

To disable the sentinel:

```text
/msg *missingchans SET stopperformon off
```

`off`, `none`, `-`, or an empty value clear the sentinel.

---

### `SET debug <on|off>`

*(Added in r11.)* Shows the extra diagnostic rows in `STATUS` (see [Status](#status)). Default off. Persisted per network.

Example:

```text
/msg *missingchans SET debug on
```

It changes presentation only. It does not change membership checks, perform invocation, timers, retry limits, suppression, or other module messages, and toggling it neither resets diagnostics nor replays commands. The setting takes effect immediately and survives module reload, IRC reconnect, and ZNC restart.

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
- once `#communityhub` is known to be joined, repeated `perform Execute` calls are suppressed;
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
- it checks what the server and ZNC say you are actually on;
- it identifies which expected channels are still missing;
- it can re-run `perform Execute` if needed;
- it retries missing joins using any stored keys from ZNC;
- it skips a retry if the channels were joined in the meantime;
- and it re-checks again afterward.

`missingchans` does not wait for the bot to answer between `Execute` and the JOINs; later retries absorb a bot that responds slowly. That is the main reason this module is valuable in environments with helper bots or other non-standard channel-access prerequisites.

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

The set is rebuilt at every check and again immediately before every retry, so changes to your channel list or to `ExpectedMode` are honored by attempts that are already scheduled. A self-PART normally removes the channel from ZNC's list, so it is no longer expected. A KICK can leave a disabled channel entry; whether that remains expected depends on `ExpectedMode`.

---

## How server-side verification works

The module combines three kinds of evidence that you are on a channel. The missing set is:

```text
expected - (live_in_znc ∪ actual_from_whois ∪ verified_from_443)
```

A channel you have since left (self-PART or self-KICK) is excluded from all three until fresh evidence arrives. An omitted WHOIS channel is **not** proof that you are absent.

### Primary mechanism: self-WHOIS

It sends:

```text
WHOIS <your-current-nick>
```

and parses numeric replies:

- `319` — channel list
- `318` — end of WHOIS
- `401` — WHOIS target failure

In the check output, `actual` rows are channels parsed from WHOIS.

### Live ZNC membership

*(Added in r10.)* Channels that ZNC currently knows are joined count as joined even when the WHOIS reply leaves them out, which servers do for some channel modes and configurations. The check output lists them as `live` rows. This prevents false retries — and, with `RetryPerform` on, unnecessary re-authentication — caused by an incomplete WHOIS.

### Fallback mechanism: numeric 443

If the server answers a JOIN or INVITE with:

```text
443 <your-nick> <channel> :is already on channel
```

the module treats that as confirmation that you are already joined there and adds the channel to a verified set (`verified` rows in the check output). Only a `443` naming **your current nick** counts; one naming another user, as an INVITE error can, is ignored.

This is a fallback, not a guarantee: many servers silently ignore a duplicate JOIN instead of answering `443`.

### WHOIS failures and the watchdog

*(Added in r10.)* A WHOIS that fails (`401`) or has not completed within **30 seconds** causes **no repair action**. The timeout includes ZNC's outbound flood queue. After a timeout, late replies are passed through to clients and must finish before another `RUN` can proceed; reconnect if the old reply never finishes. This prevents a late reply from being mistaken for the next verification.

---

## Implementation details

### Delayed automatic run on connect

When `OnIRCConnected()` fires, the module:

- cancels any pending timers from an earlier connection;
- increments an internal generation counter;
- logs that verification is scheduled;
- installs a one-shot timer for `Delay` seconds.

The generation system makes the module ignore stale timer callbacks from older connection cycles.

### State reset on disconnect

When the IRC connection drops, the module:

- removes its pending timers;
- advances the generation;
- clears volatile state, including the WHOIS bookkeeping and in-memory diagnostics;
- resets retry/run-specific bookkeeping.

Saved settings are untouched.

### Retry scheduling model

Retries are not all scheduled at once.

Instead, after each verification cycle that still finds missing channels, the module schedules the **next** join attempt with:

```text
wait = max(1, attempt_number * RetryStep)
```

That means later retries back off naturally.

When an attempt comes due, it first re-reads current settings and state *(r10)*:

- if `JoinMissing` has been turned off, or `Retries` lowered below this attempt, the attempt is cancelled;
- the expected and missing sets are rebuilt from current membership, and the attempt is skipped if nothing is missing;
- the `StopPerformOn` sentinel is re-evaluated before perform is considered.

A manual `RUN` replaces all pending timers, so an older sequence cannot block or overlap a new one. Timer callbacks release their names before calling into the module, so cancellation never removes a callback that is already running.

### Recheck after every join burst

After sending the `JOIN` commands for the current missing set, the module schedules a short recheck timer 3 seconds later.

This keeps the feedback loop tight:

- join attempt;
- short pause;
- WHOIS again;
- recompute missing set.

Self JOIN events also remove channels from the cached missing set as they arrive.

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

The comparison is ASCII case-insensitive. It does not implement full IRC `CASEMAPPING` equivalence for punctuation such as `[` and `{`.

### Robust WHOIS 319 parsing

The module concatenates WHOIS parameters from index 2 onward before splitting the channel list. This is more tolerant of parser/layout differences in the `319` reply. Channel lists split across several `319` lines are combined.

### User-mode prefix handling

WHOIS `319` channel-list tokens often carry your user-mode prefix in each channel — for example, a token of `+#chan` means "voiced in `#chan`", `@#chan` means "op in `#chan`", and `~&@#chan` means "owner + admin + op in `#chan`".

The module strips these user-mode prefixes from each token so that the underlying channel name is compared against the expected list. Since r10 it uses the rank symbols the server advertises in `PREFIX` and the channel types in `CHANTYPES` (assuming `#&!+` if none are advertised), so unusual ranks such as `!` are handled.

The tricky cases are symbols that are both a rank and a channel type, such as `+` and `&`. The parser treats such a symbol as a rank prefix only when the next character is itself another rank or channel-type prefix. Otherwise it is the channel-type prefix and parsing stops there. This correctly handles `+#chan` (voiced in `#chan`), `&#chan` (admin in `#chan`), `+chan` (modeless channel), `&local` (local channel), and combinations like `@+modeless` (op in a modeless channel). A token that does not end up starting with a channel type is discarded rather than guessed at.

### Hiding the module's self-WHOIS from clients

The verification cycle is driven by a `WHOIS` that the module sends against your own current nick. Without intervention, the server's reply numerics (`311`, `312`, `313`, `317`, `319`, `330`, `338`, `671`, `318`, and on failure `401`) would reach every attached client, which is noisy because the user did not ask for that WHOIS. The module drops those replies before they reach any attached client. The internal parsing still runs, so the verification table is produced as before — only the underlying WHOIS exchange is hidden.

User-initiated `/whois` is unaffected. To tell module-initiated from user-initiated WHOIS, the module keeps a FIFO queue of request origins, relying on servers answering WHOIS requests in order on one connection. Since r10 each entry records its **target nick**, so only replies for that target can retire it:

- a client WHOIS with several targets adds one entry per target;
- an unrelated `401` does not consume the module's request;
- a `401` alone completes a request, and a `401` followed by `318` retires it only once;
- numerics are observed in the raw-message hook, so `route_replies` cannot make the bookkeeping miss a client's replies.

Client WHOIS tracking is limited to 128 queued entries, with at most one additional internal request. Overflow aborts the current verification without repair; reconnect if the tracking cannot drain. The queue is cleared on IRC disconnect.

The suppression is unconditional and has no `SET` option. To inspect the module's own WHOIS exchange, use ZNC's traffic log rather than an attached client.

### Client-attach notice

If a retry join attempt happens while no client is attached to ZNC, the module stores a short notice and prints it when a client later attaches.

### Resident loader for in-place updates

*(Added in r12.)* ZNC loads every module with `RTLD_GLOBAL`. `znc-buildmod` compiles without optimization, so each module exports many weak copies of standard-library template code, such as `std::string` construction and the type set that ZNC's `MODULEDEFS` fills in. A module loaded later binds its own references to those symbols in the **earliest** loaded module that defines them, and glibc then keeps that earlier module in memory for as long as the later one is loaded. `UpdateMod` reopens the same path, glibc returns the copy already in memory, and the old code keeps running even though ZNC reports `Done`.

Whether that happens to a given build depends only on what is loaded after it. In tests with ZNC 1.10.3, r11 stayed stale behind stock `perform` when it was the first module in the process, and — with `webadmin` loaded first — behind other repository modules such as highlightctx and keepchanbuffersize loaded after it. This is a different mechanism from the one fixed in highlightctx 0.11.2: `missingchans` has no `STB_GNU_UNIQUE` symbols on GCC 11 or 13. It cannot be prevented from source either, because no source-level visibility setting stops an unoptimized build from exporting those library instantiations.

r12 therefore stops depending on which copy is in memory. ZNC creates each instance through the module's loader callback, and in r12 that callback:

1. records which file it came from when ZNC first maps it, and makes itself permanently resident when it first creates an instance, so `UpdateMod` and `LoadMod` always reach it;
2. compares the installed `missingchans.so` with that file (device and inode);
3. if they are the same, creates the instance from its own code;
4. if they differ, opens the installed file privately with `RTLD_LOCAL` through a temporary hard link (so glibc cannot hand back the old mapping by name), checks it was built for the running ZNC, and creates the instance from that code;
5. closes builds that no longer have any instances the next time it loads one.

Other modules cannot bind to an `RTLD_LOCAL` object, so private builds unload when closed. The one exception would be an `STB_GNU_UNIQUE` symbol, and `tests/symbols.py` fails any build that defines one the ZNC binary does not. All module-specific code has internal linkage regardless of compiler flags, so different builds loaded side by side cannot interfere with each other. If a replacement cannot be used, the loader keeps the network on the build that ran before and reports why, rather than leaving the network without the module.

The module's settings, commands, and recovery behavior are identical whichever way an instance was loaded.

---

## Interaction with ZNC's built-in `perform` module

`missingchans` does **not** replace `perform`. Instead, it can optionally use it as a recovery helper.

The integration flow is:

1. `missingchans` detects missing channels;
2. when a retry comes due and `RetryPerform` is on (and not suppressed), it finds `perform`;
3. if found, it sends `Execute` to that module;
4. it then sends `JOIN` for the channels that are still missing.

Lookup order for `perform` is:

1. network module `perform`
2. user module `perform`

When only a user-level `perform` exists, the module temporarily gives it the originating network and user and no specific client, invokes it, and then restores all three, even if the call throws. Commands and the confirmation therefore go to the network that needed recovery, including its other attached clients. Network-level perform still takes precedence.

**Upgrade note from r9:** r9's user-level fallback could print `perform commands sent` without actually sending the configured IRC commands. Since r10 that fallback works, so all commands in a user-level perform list will now really be replayed on the originating network when a retry needs them. Review that list if a network relies on it.

`missingchans` does not invoke `delayedperform`, modify either perform list, or change normal connection-time execution.

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

Tune from there based on how quickly the network, helper bot, or cloak system normally settles after reconnect. `SET debug on` shows what the last attempt decided and why.

---

## Limitations and caveats

- Positive live ZNC membership is deliberately trusted. This prevents false retries caused by incomplete WHOIS, but cannot detect a disagreement between ZNC and the IRC server if neither side reports it.
- If your network hides a channel from WHOIS, ZNC does not consider it joined, and the server never sends a useful `443`, the channel will continue to appear missing.
- The module only retries channels that are part of the expected set built from ZNC's configured channel objects.
- `RetryPerform` replays the whole perform list; it only helps if that list is safe to re-execute.
- A poorly chosen `Delay` that is too short can make retries start before your normal auth flow has had time to succeed.
- WHOIS correlation relies on the server answering requests in order; IRCv3 labeled responses are not used. A module that intercepts, reorders, or injects overlapping WHOIS traffic can interfere. Ordinary `route_replies` use is tested; overlapping route_replies routing may expose internal replies to a client before this module can hide them. If attribution fails, the watchdog stops recovery rather than assuming membership is missing.
- Channel comparison is ASCII case-insensitive, not full IRC `CASEMAPPING`.
- In-place updates rely on Linux/glibc dynamic-loader behavior and were tested with glibc 2.35 and 2.39. They need permission to create a hard link in the module directory, which an AppArmor profile or other confinement may withhold; see [Permissions for in-place updates](#permissions-for-in-place-updates). Upgrading **to** r12 from an older build may still need one restart. A crash backtrace from a build loaded in place names the temporary file `.missingchans-load-<pid>-<n>` instead of `missingchans.so`.
- Tests do not cover every IRCd, service bot, TLS/SASL deployment, or arbitrary third-party module. See [TESTING.md](./TESTING.md) for exactly what is and is not covered.

---

## Safe rollout advice

A good way to deploy this module is:

1. load it with `JoinMissing` off;
2. watch what `SHOW`, `STATUS`, and `RUN` report after real reconnects (`SET debug on` adds detail);
3. switch to `ExpectedMode enabled` if needed;
4. only then enable `JoinMissing`;
5. add `RetryPerform` after confirming that re-running `perform` is safe in your environment.

That gives you confidence in the verification logic before enabling automated repair actions.

When upgrading, install with a rename, run `UpdateMod missingchans` once, and confirm with `VERSION` on each network that the new build is running and the `Loader:` line shows no warning.

---

## Summary

`missingchans` is best thought of as a **verification and recovery layer** for ZNC reconnect behavior.

It does not assume that the first connect-time join sequence succeeded. Instead, it checks what the server and ZNC say actually happened, identifies what is missing, and can retry in a controlled way — re-checking before every action.

That makes it particularly well suited to IRC environments where channel access depends on additional moving parts such as custom auth bots, cloaks, delayed permissions, or intermittent outages. Since r12 it can also be updated in place with `UpdateMod`, without a ZNC restart that would drop every network's connection.
