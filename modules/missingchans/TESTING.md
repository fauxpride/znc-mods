# Testing `missingchans`

The suite loads the **real compiled module into ZNC 1.10.3**. It runs two fake IRC servers on loopback and attaches one client to each network. The production module is not mocked or rewritten for ordinary tests. Assertions examine actual IRC commands, client messages, module status, and process exits.

See [tests/RESULTS.md](./tests/RESULTS.md) for the recorded release validation.

## Layout

| File | Purpose |
|---|---|
| `tests/harness.py` | Loopback IRC servers, IRC clients, temporary ZNC configuration, process lifecycle, logs, and sanitizer checks. |
| `tests/suite.py` | Test runner, 34 integration scenarios, r9 reproductions, differential command comparison, and real r9/r10-to-r11 upgrade checks. |
| `tests/baseline/missingchans.cpp` | Unmodified r9 source from repository commit `32f7fe19cdd0e69566e2985b4d7531ed1ccb9c18`; test fixture only. |
| `tests/baseline/r10/missingchans.cpp` | Unmodified r10 source delivered before this update; status/upgrade fixture only. |
| `tests/RESULTS.md` | Environment, outcomes, source hashes, and limitations. |
| `tests/results/*.txt` | Captured suite/build summaries and sanitizer plumbing check. |

## Requirements

- Linux, Python 3.9+, a C++ compiler, and matching ZNC 1.10.3 / `znc-buildmod` development files. Python requires no third-party packages.
- ZNC's normal `perform` and `route_replies` modules installed for that same ZNC build.
- Permission to create loopback sockets and start local processes.
- Run as an ordinary unprivileged user where possible. If launched as root, the harness changes ownership of **its newly created temporary test directories only**, starts ZNC under UID/GID 65534, and restores ownership after stopping it. It does not create users or modify system accounts. That UID must be able to read the test ZNC installation and traverse the test root's parents.

The suite uses synthetic account `tester`, password `test-password`, and channels such as `#gate`. It does not load your ZNC configuration or contact public IRC servers. ZNC's own automatic join retry limit is set to one in the test configuration to separate core autojoins from module retries. Server throttle is disabled for the two localhost connections.

## Build

Use a separate output folder; `znc-buildmod` writes into its current directory. From the repository root:

```sh
repo="$PWD"
mkdir -p /tmp/missingchans-test-build/r9 /tmp/missingchans-test-build/r10 /tmp/missingchans-test-build/r11
(cd /tmp/missingchans-test-build/r9 && znc-buildmod "$repo/modules/missingchans/tests/baseline/missingchans.cpp")
(cd /tmp/missingchans-test-build/r10 && znc-buildmod "$repo/modules/missingchans/tests/baseline/r10/missingchans.cpp")
(cd /tmp/missingchans-test-build/r11 && znc-buildmod "$repo/modules/missingchans/src/missingchans.cpp")
```

These test binaries are not installed into your running ZNC.

## Run

First prove the baseline failures exist:

```sh
ZNC_BIN=/path/to/znc MC_TEST_ROOT=/tmp/missingchans-tests/r9 \
python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/r9/missingchans.so --baseline
```

All three baseline scenarios should say PASS: here PASS means that the **old bug was reproduced**, not that r9 behaved correctly.

Then run the fixed module and compare the unchanged commands:

```sh
ZNC_BIN=/path/to/znc MC_TEST_ROOT=/tmp/missingchans-tests/r11 \
python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/r11/missingchans.so \
  --compare-r9 /tmp/missingchans-test-build/r9/missingchans.so \
  --compare-r10 /tmp/missingchans-test-build/r10/missingchans.so
```

`--compare-r9` runs a 21-response byte comparison (including default and configured STATUS) and an atomic r9-to-r11 binary replacement followed by `UpdateMod`. `--compare-r10` compares all 21 r10 STATUS rows with r11 Debug ON (excluding the new Debug row) and runs an r10-to-r11 upgrade. Upgrade checks verify both network instances, saved settings, Debug default OFF, and cancellation of an old pending retry. If the loader retains the old revision, each test verifies a full ZNC restart against the same data directory as the fallback. These four comparison/upgrade checks are counted separately: the full release run has 38 entries. The ordinary/sanitized run has 34 entries.

Select individual scenarios by name after the options:

```sh
ZNC_BIN=/path/to/znc python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/r11/missingchans.so \
  late_join user_perform whois_clients
```

A failed assertion, abnormal ZNC exit, background harness exception, or detected sanitizer error makes the suite exit nonzero. Each test prints its unique temporary log directory. Failed tests retain their data too.

## Environment

| Variable | Meaning |
|---|---|
| `ZNC_BIN` | ZNC executable; defaults to `znc` on PATH. |
| `MC_TEST_ROOT` | Parent for unique per-scenario temporary directories; system temporary directory if unset. Existing directories are not deleted. |
| `ZNC_PRELOAD` | Optional `LD_PRELOAD` applied only to the ZNC child. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | Passed through to ZNC for instrumented runs. |

Each directory contains `znc.log`, `irc-a.log`, `irc-b.log`, and `client-N.log`, alongside the synthetic configuration and module registry. The reconnect scenario also saves the old connection transcript. Client/server barriers and explicit event predicates are preferred to fixed sleeps. Small sleeps test absence of actions after a known deadline. Backoff checks allow scheduling tolerance; the 30-second watchdog is exercised at its real production duration.

## Coverage

| Scenarios | Assertions |
|---|---|
| `omitted_joined`, `late_join`, `user_perform` | Reproduce r9 failures; assert r11 retains the r10 fixes preventing false retries and routes a working user-level perform to its origin only. Context restoration is checked by invoking perform from the other network afterwards. |
| `sentinel_suppression`, `network_preference`, `successful_recovery` | Sentinel suppresses perform while other joins proceed; network perform takes precedence; a fake auth command enables a keyed channel, and recovery stops after success. |
| `pending_settings`, `numeric443` | Pending retries honor three settings changes; another user's 443 cannot count as self-membership; self-443 suppresses/skips a pending retry. |
| `prefixes` | Multiline 319, ASCII case differences, voice/op/owner/admin/compound/unusual advertised prefixes. ZNC local membership is false so the parser itself must succeed. |
| `whois_clients`, `whois_multi_error`, `whois_overlap` | User replies remain visible before/after internal verification, including ordinary route_replies use, multi-target error replies, and a concurrent same-nick client request. Internal replies stay hidden in these cases. |
| `whois_failure`, `whois_401_only`, `unrelated401`, `whois_timeout` | Failed/absent WHOIS does not trigger repair; an unrelated 401 does not consume the internal request; both 401-only and 401+318 failures retire exactly one request; a late reply cannot complete a new check; recovery can resume after draining it. |
| `manual_restart`, `busy_run` | A new cycle replaces an old pending timer; a RUN during an outstanding verification cannot overlap/reset it. |
| `departures`, `unrelated_departures` | Self-PART follows ZNC's removal of the expected entry; self-KICK invalidates stale positive membership; other users leaving does not invalidate our membership. |
| `settings_persistence`, `reload_pending` | Settings survive unload/load; UpdateMod removes pending callbacks; a new manual run still works. |
| `expected_modes`, `defaults`, `no_perform` | All/config/enabled selection including disabled and transient entries; unchanged defaults; missing perform does not prevent requested channel joins. |
| `bounded_backoff`, `zero_step` | Exact retry limit and increasing wait schedule; zero step has a one-second minimum instead of a zero-interval timer. |
| `diagnostic_readonly`, `command_surface` | Diagnostic commands do not query/reset/rearm repair; existing setting aliases and invalid-command responses remain available. |
| `disconnect_reconnect` | Old connection timers cannot act after reconnect; a fresh connection performs its own bounded recovery. |
| `debug_status` | Exactly nine r9 fields by default; all debug fields when enabled; boolean aliases and case handling; no cross-network setting changes or repair side effects. |
| `debug_persistence` | Debug ON and OFF survive UpdateMod and full process restart, preserving all seven original settings; the other network remains OFF. |
| `debug_pending_recovery` | Toggles while waiting do not cancel, duplicate, or restart a retry; real perform/JOIN count unchanged; counters/history are retained while hidden. Runs from both initial modes. |
| `debug_recovery_guards` | Repeats WHOIS-omission, late-JOIN, and scoped-user-perform regression checks with Debug ON. Original versions run with Debug OFF. |
| `--compare-r9` | 21 responses match byte-for-byte, including complete STATUS tables before and after setting changes; r9-to-r11 upgrade preserves settings. Empty SET usage intentionally changes to include Debug and is tested separately. |
| `--compare-r10` | All 21 r10 STATUS rows preserved with Debug ON; r10-to-r11 upgrade preserves settings. Both upgrade tests verify the restart fallback if the old binary stays resident. |

## Sanitizers

Build a separate instrumented module:

```sh
repo="$PWD"
mkdir -p /tmp/missingchans-test-build/sanitized
(cd /tmp/missingchans-test-build/sanitized && \
 CXXFLAGS='-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g' \
 LDFLAGS='-fsanitize=address,undefined' \
 znc-buildmod "$repo/modules/missingchans/src/missingchans.cpp")

ZNC_BIN=/path/to/znc \
ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
MC_TEST_ROOT=/tmp/missingchans-tests/sanitized \
python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/sanitized/missingchans.so
```

The module is instrumented; the ZNC core need not be. The harness scans all process logs and checks exit codes after termination. The prior r10 validation of this unchanged harness also used a disposable copy with a deliberate heap use-after-free at module load and checked that the harness rejected it. That deliberately faulty binary/source is not part of the release. Its retained log is labeled as an r10 harness check; r11 sanitizer results come from the final r11 source.

## What this does not prove

- No test connected to the user's VPS, live IRC networks, authentication services, or production client configuration. Reproducing defects does not identify which live network originated the reported messages.
- The test ZNC is a local debug build without TLS, SASL, or third-party auth modules. The regression tests validate membership, module context, timers, and protocol flow, not certificate/authentication correctness.
- The unit of concurrency is event ordering on ZNC's normal single-threaded module path. This is not a load/performance test or an exhaustive protocol fuzzer.
- Full IRC punctuation CASEMAPPING, every CHANTYPES ambiguity, arbitrary WHOIS reordering/interception by other modules, and all custom IRCd numerics remain outside coverage.
- Channel lists use positive live membership as evidence. This cannot independently detect an IRCd/core disagreement if neither side supplies a correcting event.
- Address/undefined/leak checks cover the exercised module paths; they are not proof of the absence of all defects.
