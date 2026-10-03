# missingchans

See [CHANGELOG.md](./CHANGELOG.md) for revision history and [TESTING.md](./TESTING.md) for the reproducible test suite.

`missingchans` is a **network module** for recovering missing channel joins after an IRC connection. It is useful when a community bot, authentication step, cloak, or temporary service outage prevents the initial joins from succeeding.

**Current build:** `2026-10-03+r11 (optional debug status; r10 recovery fixes retained)`

**Source:** [`src/missingchans.cpp`](./src/missingchans.cpp)

**Build and integration-test target:** ZNC 1.10.3. Recompile for the ZNC installation that will load the module. Older ZNC releases have not been validated for r11.

## How it works

1. After connecting to IRC, wait `Delay` seconds and query the current nick with WHOIS.
2. Build the expected-channel set from this network's ZNC channel entries.
3. Combine positive WHOIS evidence, ZNC's current `CChan::IsOn()` state, and any relevant self-targeted numeric 443 confirmations. An omitted WHOIS channel is **not** proof that you are absent.
4. If channels are missing and `JoinMissing` is on, schedule a bounded retry sequence.
5. Immediately before each retry, rebuild the expected and missing sets using current membership and settings. Self JOIN/PART/KICK events invalidate outdated evidence. Skip the retry if nothing is missing.
6. If enabled and not suppressed, invoke `perform Execute` on this network; then send JOINs, reusing stored channel keys. Recheck via WHOIS three seconds later.

This is connection recovery, not perpetual channel monitoring. A manual `RUN` starts another cycle. Merely attaching a client, reading `STATUS`, or receiving a PART/KICK does not start a new cycle. Loading or reloading the module on an already connected network does not automatically run recovery.

A self-PART normally removes the channel from ZNC's list, so it is no longer expected. A KICK can leave a disabled channel entry; whether it remains expected depends on `ExpectedMode`.

### Membership and WHOIS

`actual` rows in the check output are parsed WHOIS channels. `live` rows identify channels ZNC knows are joined but which were not in that WHOIS reply. `verified` rows are additional self-443 evidence. `missing` is the difference from the expected set.

The parser uses the server's advertised `PREFIX` and `CHANTYPES`, including unusual rank symbols. Prefixes such as `@#chan`, `+#chan`, `~@#chan`, and, when advertised, `!#chan` are stripped. Comparison remains ASCII case-insensitive, as in r9; it does not implement full IRC CASEMAPPING equivalence for punctuation.

The module hides its recognized internal WHOIS numerics. Client WHOIS requests are tracked separately, including multiple targets, errors ending with `401` alone, and the common `401` followed by `318` error sequence. Numerics are observed in the raw-message hook so `route_replies` cannot make the bookkeeping miss ordinary client replies.

A WHOIS that fails or has not completed within **30 seconds** causes **no repair action**. The timeout includes ZNC's outbound flood queue. After a timeout, late replies are allowed through to clients and must finish before another `RUN` can proceed; reconnect if the old reply never finishes. This prevents a late reply from being mistaken for the next verification. Client WHOIS tracking is limited to 128 queued entries, with at most one additional internal request; overflow aborts verification and requires reconnect if tracking cannot drain.

### Perform scope

The module prefers this network's `perform` instance. If only a user-level `perform` exists, r10 temporarily gives it the originating network/user context and clears the client context, invokes it, then restores all three fields. Commands and the confirmation therefore go to the originating network, including its other attached clients.

This corrects r9's user-level fallback: it could print `perform commands sent` across networks without actually sending the configured IRC commands. **Upgrading makes that fallback functional.** Review the user-level perform list if a network relies on it; all its commands will now be replayed on that originating network when a genuine retry requires it. Network-level perform still takes precedence.

`perform Execute` replays the whole selected perform list, not only authentication commands. `missingchans` does not invoke `delayedperform`, modify either perform list, or change normal connection-time execution. It does not wait for asynchronous bot authentication between Execute and JOIN; later retries accommodate delayed bot responses.

## Settings

Set options with `/msg *missingchans SET <key> <value>` on the intended network. All seven existing NV keys and defaults are preserved; no migration is required. r11 adds one optional, per-network `debug` setting, defaulting to off when absent.

| Key | Default | Meaning |
|---|---|---|
| `debug` | `off` | Show the extra r10 diagnostic fields in STATUS. Persisted per network; changes presentation only. |
| `delay` | `300` | Seconds from IRC connection to the first check; minimum 1. |
| `joinmissing` | `off` | Whether to send JOINs for missing channels. |
| `expectedmode` | `all` | `all`: every known channel; `config`: only `InConfig` entries; `enabled`: `InConfig` entries that are not disabled. |
| `retryperform` | `off` | Invoke perform before a retry, unless suppressed. |
| `retries` | `3` | Maximum JOIN attempts per cycle; minimum 1. |
| `retrystep` | `20` | Attempt N waits N × this many seconds; effective wait is at least one second. Multiplication is protected against overflow. |
| `stopperformon` | `off` | Sentinel channel whose known membership suppresses further perform calls for the current cycle. `off`, `none`, `-`, or an empty value clears the configured sentinel. |

`StopPerformOn` suppression is **latched for the cycle**. Once triggered it stays set even if the sentinel later parts or the setting is cleared. A new `RUN` or connection cycle resets it and evaluates current membership again. It suppresses perform, not JOIN attempts for other missing channels.

Pending retries respect changes to `JoinMissing`, `RetryPerform`, `ExpectedMode`, `StopPerformOn`, and a lowered retry limit when they fire. Changing `RetryStep` does not move an already scheduled timer; it affects subsequently scheduled attempts. Changing `Delay` affects future connection cycles.

For `Delay=300`, `RetryStep=300`, `Retries=10`, the retry waits are 5, 10, 15, …, 50 minutes. With prompt replies the last attempt is roughly 4 hours 40 minutes after connection, including the intervening three-second checks. These are increasing delays, not ten retries at fixed five-minute intervals.

## Commands and diagnostics

```irc
/msg *missingchans HELP
/msg *missingchans VERSION
/msg *missingchans STATUS
/msg *missingchans SHOW
/msg *missingchans RUN
```

- `HELP`: commands and all settings; `h` and `?` are aliases.
- `VERSION`: build marker.
- `SHOW`: expected channels; does not query WHOIS, invoke perform, reset suppression, or start a cycle.
- `STATUS`: the nine original r9 fields by default; extended diagnostics when `Debug` is on. No repair action or timer reset.
- `RUN`: starts a new verification/recovery cycle and replaces pending timers. It can lead to real perform/JOIN commands with the configured settings. While an internal WHOIS is outstanding, a second `RUN` is rejected rather than overlapping it.

r11 gates the fields introduced in r10 behind `Debug`. To enable them on the current network:

```irc
/msg *missingchans SET debug on
/msg *missingchans STATUS
```

Restore the compact display with `/msg *missingchans SET debug off`. The change takes effect immediately and survives module reload, IRC reconnect, and a ZNC restart. It does not change membership checks, perform invocation, timers, retry limits, suppression, or other module messages. Diagnostic history continues to be recorded while hidden; toggling Debug does not reset counters or replay commands.

With Debug off, STATUS contains exactly these nine rows, in the r9 order: `Delay`, `JoinMissing`, `ExpectedMode`, `RetryPerform`, `Retries`, `RetryStep`, `StopPerformOn`, `PerformSuppressed(this run)`, and `VerifiedJoined(count)`. There is no Debug row in this mode.

With Debug on, STATUS adds `Debug = ON` and all of these r10 fields:

| Field | Meaning |
|---|---|
| `Network` | Originating network. |
| `Phase`, `Attempt` | Current recovery phase and scheduled/last attempt number. |
| `LiveJoined(count)` | Channels currently marked joined by ZNC. |
| `WhoisJoined(last snapshot)` | Channels in the latest parsed WHOIS snapshot. |
| `Missing(last check)` | Cached missing set, also pruned by self JOINs; not a new verification. |
| `LastAction` | Last recorded repair decision. |
| `LastAttemptMissing` | Channels considered missing immediately before the last attempted repair. |
| `LastAttemptTriggeredPerform` | Whether that attempt invoked perform. |
| `PerformCalls(this connection)` | Number of Execute invocations by this instance, including invocations with an empty perform list. |
| `LastPerformSource`, `LastPerformAt` | Network/user module selection and timestamp in UTC of the last invocation. |

The retained original `VerifiedJoined(count)` is **only the self-443 cache**, not your joined-channel count. Zero is normal. Numeric 443 is not a reliable response to a duplicate JOIN: IRCds commonly ignore duplicate JOINs silently. A 443 naming someone else is ignored.

Diagnostics are in memory and reset on disconnect/reload; configuration, including Debug, persists. Like the existing boolean settings, Debug accepts `on`, `1`, `yes`, `true`, `enable`, or `enabled` (case-insensitive) as on; other values, including an empty value, mean off. Prefer the explicit `on`/`off` forms. Last-perform source/time/count persist across manual runs within the same connection. No perform text, passwords, or channel keys are included in these new diagnostics. Channel names and network names are included.

## Build, replace, and verify

Run the shell commands as the Unix account that runs ZNC. Build with its installed `znc-buildmod` in a separate directory:

```sh
mkdir -p ~/znc-module-build/missingchans-r11
cd ~/znc-module-build/missingchans-r11
cp /path/to/modules/missingchans/src/missingchans.cpp .
znc-buildmod missingchans.cpp
```

For the standard user module directory, back up the existing binary and install using a **rename**, so a loaded shared object is not overwritten in place:

```sh
cp -p ~/.znc/modules/missingchans.so ~/.znc/modules/missingchans.so.pre-r11-backup
install -m 0644 missingchans.so ~/.znc/modules/missingchans.so.new
mv -f ~/.znc/modules/missingchans.so.new ~/.znc/modules/missingchans.so
```

Adjust `~/.znc` if your ZNC uses a different data directory or module location. Then, from an IRC client connected as a ZNC administrator, run **once**:

```irc
/msg *status UpdateMod missingchans
```

`UpdateMod` unloads/reloads **all loaded instances of this module across ZNC users and networks**. It resets their pending recovery cycles. It does not disconnect IRC. Then check each relevant network:

```irc
/msg *missingchans VERSION
/msg *missingchans STATUS
```

Confirm `+r11` and that your settings remain intact. **Do not treat “Done” from UpdateMod as proof of a version change.** In the test build, an r9 library loaded before `perform` was retained by the dynamic loader because `perform` had bound shared C++ symbols to it. UpdateMod therefore still reported r9. If any instance remains on an older revision, restart the ZNC process using your usual service/process-management method and check VERSION again. That full-restart upgrade path was tested; it reconnects your IRC networks and starts normal connection recovery. Both r9-to-r11 and r10-to-r11 upgrades required and passed this restart fallback in the test build. An already loaded old binary cannot be made unloadable by editing the replacement source. Reload cancels that instance's pending retries and resets volatile diagnostics; it does not disconnect IRC. It does not automatically start recovery on an already connected network. Use `RUN` only if you intend to start a repair cycle, or let the next IRC connection start it normally. `RetryPerform` can remain enabled.

Rollback: copy the backed-up binary to a temporary sibling, rename it over `missingchans.so`, and run `UpdateMod missingchans` once again, then verify the version on the affected networks. The seven original settings remain compatible with r9/r10; those revisions ignore the additional Debug key. If the previous revision does not appear after rollback, use the same full-restart fallback.

## Tests and limits

See [TESTING.md](./TESTING.md) and [tests/RESULTS.md](./tests/RESULTS.md). Tests exercise the actual compiled module in real ZNC with two loopback-only fake IRC servers and multiple clients. The unchanged r9 and r10 sources are included solely as reproduction/differential and upgrade fixtures under `tests/baseline/`. The full r10 regression suite is retained and extended with Debug coverage.

Positive live ZNC membership is deliberately trusted. This prevents false retries caused by incomplete WHOIS but does not independently prove that ZNC and the IRCd can never disagree. Tests do not cover every IRCd, service bot, TLS/SASL deployment, or arbitrary third-party module. WHOIS correlation still relies on serialized replies; there are no IRCv3 labeled responses. A module that intercepts, reorders, or injects overlapping WHOIS traffic can interfere. Ordinary `route_replies` use is tested; overlapping route-replies routing may expose internal replies to a client before this module can hide them. If attribution fails, the watchdog stops recovery rather than assuming missing membership.
