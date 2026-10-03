# r11 validation record

Validated on 2026-10-03 against real ZNC 1.10.3 processes and synthetic loopback IRC servers/clients. No live user network or production configuration was accessed.

## Final outcomes

| Run | Result | Record |
|---|---|---|
| Final r11 integration and compatibility run | **38/38 passed**, exit 0 | [integration.txt](./results/integration.txt) |
| Final r11 AddressSanitizer / UndefinedBehaviorSanitizer run | **34/34 passed**, exit 0; no sanitizer diagnostics | [sanitizers.txt](./results/sanitizers.txt) |
| Strict production and instrumented builds | Passed with `-Wall -Wextra -Wpedantic -Werror` | [build.txt](./results/build.txt) |
| Original r9 bug reproductions (retained from earlier same-day r10 validation) | 3/3 reproduced | [baseline.txt](./results/baseline.txt) |
| Deliberate faulty-module probe (retained r10 harness validation) | Correctly rejected with heap-use-after-free diagnostic and nonzero exit | [r10-sanitizer-probe.txt](./results/r10-sanitizer-probe.txt) |

The release run has 34 integration entries (all 30 r10 scenarios plus four Debug scenario groups) and four differential/upgrade checks. Some entries include several subcases. These counts are runner entries, not assertion counts. All final-source scenarios passed without failures. The source was not changed after either build.

The sanitizer run instruments the module, not the entire ZNC core. Leak detection was enabled. An additional scan of the release run's 51 and sanitized run's 43 retained process logs, including pre-restart logs, found no address/undefined/leak sanitizer diagnostics. The deliberate faulty probe is a retained check of the unchanged harness from r10, not a newly instrumented r11 probe; the faulty code is not shipped.

## What the new checks establish

- **Default OFF:** An absent Debug key gives exactly the nine r9 STATUS rows, in order. Debug itself is not shown while off.
- **Exact compact compatibility:** The r9 differential run compares 21 responses byte-for-byte, including the entire STATUS table both at defaults and after setting changes. Empty SET usage is intentionally excluded from that comparison because it now documents Debug; its new behavior is tested separately.
- **Complete extended display:** Debug ON shows Debug=ON and all twelve r10 diagnostic rows. A direct comparison with the unchanged r10 binary confirms all 21 original r10 rows/values are preserved in an equivalent state, excluding only the new Debug row.
- **Persistence and isolation:** Debug ON and OFF survive UpdateMod and full process restart. All seven original saved settings are preserved. Enabling Debug on network a leaves network b off. A full restart also reconnects IRC, verifying that connection-state reset does not erase Debug.
- **No recovery side effects:** Toggling Debug while a retry is pending neither cancels nor duplicates that retry. Tests starting in either mode observe exactly one perform invocation and the expected JOIN count. Counters, last-perform time/source, and last-attempt channels are retained across off/on toggles; toggling generates no extra repair traffic.
- **r10 fixes retained:** The ordinary regression cases run with Debug OFF; the WHOIS-omission, late-JOIN, and scoped-user-perform checks also run with Debug ON. All pass. A source comparison additionally confirms that recovery hooks, membership/WHOIS logic, perform invocation, retry logic, and timer implementations are unchanged from the delivered r10 source.
- **Command behavior:** HELP aliases, empty SET usage, case-insensitive setting names and boolean aliases are covered. Debug intentionally uses the same boolean parser as JoinMissing/RetryPerform: non-true values, including empty/invalid input, mean OFF.

## Upgrade results

Actual r9-to-r11 and r10-to-r11 binary replacements were tested using an atomic rename followed by ZNC's administrator `UpdateMod missingchans`. Both older libraries remained resident in this test build, so VERSION still reported the old revision. Both tests then used a full ZNC restart against the same data directory and passed: both network instances reported r11, saved settings survived, Debug defaulted to OFF, and the old pending retry did not execute. The logs explicitly report `restart=True`.

This does not claim that in-process replacement of those older binaries succeeded. Verify VERSION after UpdateMod; if it still reports an older revision, restart ZNC using the usual service/process method. A full restart reconnects IRC networks. Reload of an already loaded r11 and pending-timer cancellation were tested separately.

## Original defects covered

1. r9 could invoke perform when WHOIS omitted a channel that ZNC knew was joined. Positive live membership prevents that false retry.
2. r9 could invoke perform from a stale snapshot after joining during the retry delay. Membership is rechecked immediately before acting.
3. r9's user-level fallback could announce perform across networks without sending the configured IRC commands. The corrected invocation supplies and restores originating-network context.

Those r10 fixes remain intact in r11. Debug is a STATUS display option; it does not disable diagnostics collection or change other messages. A zero VerifiedJoined count still means only that there is no cached self-443 evidence, not that no channels are joined.

## Environment and source identity

- Repository baseline: `fauxpride/znc-mods` commit `32f7fe19cdd0e69566e2985b4d7531ed1ccb9c18`.
- r10 fixture: the exact source delivered in the preceding r10 ZIP, before this Debug change.
- ZNC: tag `znc-1.10.3`, commit `6bd91573cebd1e4ee954ebd4eb6db5b654e0e412`; local debug build with matching `perform` and `route_replies` modules.
- Linux x86-64; GNU C++ 13.3.0; CMake 3.31.10; Python 3.12.14.
- The test build disabled OpenSSL, zlib, Cyrus, Argon, ICU, i18n, Perl, and Python modules. These are test-environment choices, not installation requirements.
- `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`; `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. Commands and instrumentation flags are in [TESTING.md](../TESTING.md).

SHA-256:

```text
1ab402eeab0c99840b0bfe54f589eb9fb2882ec7c8b69bb4b72ccc6301ebd425  src/missingchans.cpp
5ed704d69972688380c4c3c9a05e70b958d8e5fa9aeaa6878f7b8b0f3d2ab2d4  tests/baseline/r10/missingchans.cpp
0ace67585dc0446bf6a574c2cdfad435b64989ed2ace7f4e737e925fcf2131f1  tests/baseline/missingchans.cpp
```

## Limits

Only local synthetic credentials and loopback IRC connections were used. No live IRC networks, TLS/SASL, ChanServ/NickServ authentication, VPS, or production client configuration was tested. The full protocol, third-party-module, and sanitizer limitations in TESTING.md still apply. Passing the suite is not a guarantee against every possible regression.

Runner logs retain original temporary paths for this validation run. Those directories are not required by the package; a new run creates new transcripts and synthetic configurations. Summary/build logs are included rather than every process transcript. Baseline/probe logs are explicitly retained earlier results; integration/sanitizer/build logs were regenerated for r11.
