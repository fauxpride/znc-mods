# Testing `missingchans`

The suite loads the **real compiled module into ZNC 1.10.3**. It runs two fake IRC servers on loopback and attaches one client to each network. The production module is not mocked or rewritten for ordinary tests. Assertions examine actual IRC commands, client messages, module status, the ZNC process's memory mappings, and process exits.

See [tests/RESULTS.md](./tests/RESULTS.md) for the recorded release validation.

## Layout

| File | Purpose |
|---|---|
| `tests/harness.py` | Loopback IRC servers, IRC clients, temporary ZNC configuration, process lifecycle, module installation by rename, mapped-file inspection, logs, and sanitizer checks. |
| `tests/suite.py` | Test runner: 34 integration scenarios, r9 reproductions, comparisons with r9/r10/r11, upgrade checks, and the r12 in-place update scenarios. |
| `tests/symbols.py` | Static checks of a built `.so`; also runs standalone without ZNC. |
| `tests/probe_loadorder.py` | Diagnostic, not pass/fail: reports whether `UpdateMod` took effect for a given module load order, and which modules bound symbols into the resident copy. |
| `tests/baseline/missingchans.cpp` | Unmodified r9 source from repository commit `32f7fe19cdd0e69566e2985b4d7531ed1ccb9c18`; test fixture only. |
| `tests/baseline/r10/missingchans.cpp` | Unmodified r10 source; comparison and upgrade fixture only. |
| `tests/baseline/r11/missingchans.cpp` | Unmodified r11 source from repository commit `2a8c760a87ecc4e711ba1ed398f11e459f32b229`; comparison, upgrade, and downgrade fixture only. |
| `tests/RESULTS.md` | Environment, outcomes, source hashes, and limitations of the recorded run. |
| `tests/results/*.txt` | Captured suite, build, symbol, probe, and sanitizer output. |

## Requirements

- Linux with glibc, Python 3.9+, a C++ compiler, binutils (`readelf`, `c++filt`), and matching ZNC 1.10.3 / `znc-buildmod` development files. Python needs no third-party packages.
- ZNC's normal `perform` and `route_replies` modules installed for that same ZNC build.
- Permission to create loopback sockets and start local processes.
- Run as an ordinary unprivileged user where possible. If launched as root, the harness changes ownership of **its newly created temporary test directories only**, starts ZNC under UID/GID 65534, and restores ownership after stopping it. It does not create users or modify system accounts. That UID must be able to read the test ZNC installation and traverse the test root's parents.

The suite uses synthetic account `tester`, password `test-password`, and channels such as `#gate`. It does not load your ZNC configuration or contact public IRC servers. ZNC's own automatic join retry limit is set to one in the test configuration to separate core autojoins from module retries. Server throttle is disabled for the two localhost connections.

## Build

Use a separate output folder for each binary; `znc-buildmod` writes into its current directory. These test binaries are not installed into your running ZNC.

The in-place update tests need two extra builds of the same source:

- a **variant**, identical except for its build marker (`+r12-variant`), so a test can tell which build is running after an update;
- a **mismatch** build that claims a different ZNC version, to exercise the loader's version check.

From the repository root:

```sh
repo="$PWD"
src="$repo/modules/missingchans/src/missingchans.cpp"
out=/tmp/missingchans-test-build
mkdir -p "$out"/r9 "$out"/r10 "$out"/r11 "$out"/r12 "$out"/variant "$out"/mismatch
sed 's/+r12 (/+r12-variant (/' "$src" > "$out/variant/missingchans.cpp"
sed -e 's/+r12 (/+r12-variant (/' \
    -e 's|^#include <znc/User.h>$|#include <znc/User.h>\n#undef VERSION_EXTRA\n#define VERSION_EXTRA "-mismatch"|' \
    "$src" > "$out/mismatch/missingchans.cpp"
grep -q '+r12-variant (' "$out/variant/missingchans.cpp" && grep -q '"-mismatch"' "$out/mismatch/missingchans.cpp"

(cd "$out/r9"  && znc-buildmod "$repo/modules/missingchans/tests/baseline/missingchans.cpp")
(cd "$out/r10" && znc-buildmod "$repo/modules/missingchans/tests/baseline/r10/missingchans.cpp")
(cd "$out/r11" && znc-buildmod "$repo/modules/missingchans/tests/baseline/r11/missingchans.cpp")
(cd "$out/r12" && CXXFLAGS='-Wall -Wextra -Wpedantic -Werror' znc-buildmod "$src")
(cd "$out/variant"  && CXXFLAGS='-Wall -Wextra -Wpedantic -Werror' znc-buildmod missingchans.cpp)
(cd "$out/mismatch" && znc-buildmod missingchans.cpp)
```

The `grep` line guards against a source edit silently breaking either `sed` substitution. The mismatch build redefines a ZNC macro, so it is not built with `-Werror`.

## Run

First prove the original r9 failures exist:

```sh
ZNC_BIN=/path/to/znc MC_TEST_ROOT=/tmp/missingchans-tests/r9 \
python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/r9/missingchans.so --baseline
```

All three baseline scenarios should say PASS: here PASS means that the **old bug was reproduced**, not that r9 behaved correctly.

Then run the full release check:

```sh
b=/tmp/missingchans-test-build
ZNC_BIN=/path/to/znc MC_TEST_ROOT=/tmp/missingchans-tests/r12 \
python3 modules/missingchans/tests/suite.py \
  --module "$b/r12/missingchans.so" \
  --compare-r9 "$b/r9/missingchans.so" \
  --compare-r10 "$b/r10/missingchans.so" \
  --compare-r11 "$b/r11/missingchans.so" \
  --variant "$b/variant/missingchans.so" \
  --mismatch "$b/mismatch/missingchans.so"
```

That runs 48 entries: the 34 integration scenarios, the symbol checks, six comparison/upgrade checks, and seven in-place update checks. Every option after `--module` is optional and adds its own checks:

| Option | Adds |
|---|---|
| (always, except with `--baseline`) | `symbols`: the checks in `tests/symbols.py` against the module and, if given, the variant. |
| `--compare-r9` | `differential_commands` (21 responses byte-for-byte, including default and configured STATUS) and `upgrade_from_r9`. |
| `--compare-r10` | `differential_r10_status` (all 21 r10 STATUS rows preserved with Debug ON) and `upgrade_from_r10`. |
| `--compare-r11` | `differential_r11` (14 responses, including both STATUS layouts and HELP, identical apart from HELP's build line; VERSION gains exactly one `Loader:` line) and `upgrade_from_r11`. With `--variant`, also `inplace_downgrade`. |
| `--variant` | The six `inplace_*` scenarios below, and an in-place update at the end of each `upgrade_from_*` check. |
| `--mismatch` | Requires `--variant`; adds a wrong-ZNC-version replacement to `inplace_bad_file`. |

Each `upgrade_from_*` check installs r12 over a running older build by atomic rename, with a retry pending, and runs `UpdateMod` once. If the older binary stays resident — expected in this load order, which is why r12 exists — the check verifies that a full restart against the same data directory loads r12, and prints `restart=True`. It then verifies both networks, saved settings, Debug defaulting to OFF, and that the old pending retry never fires. With `--variant` it finally installs the variant and checks that this second update applies **without** a restart.

Select individual scenarios by name after the options:

```sh
ZNC_BIN=/path/to/znc python3 modules/missingchans/tests/suite.py \
  --module /tmp/missingchans-test-build/r12/missingchans.so \
  late_join user_perform whois_clients
```

Named scenarios run first; the option-driven checks always follow. A failed assertion, abnormal ZNC exit, background harness exception, or detected sanitizer error makes the suite exit nonzero. Each test prints its unique temporary log directory. Failed tests retain their data too.

### Symbol checks on their own

`tests/symbols.py` inspects a built module without starting ZNC:

```sh
python3 modules/missingchans/tests/symbols.py \
  --module /tmp/missingchans-test-build/r12/missingchans.so --znc "$(command -v znc)"
```

It fails if the module defines an `STB_GNU_UNIQUE` symbol that the ZNC binary does not (glibc would mark even a private load undeletable), lacks `ZNCModuleEntry` or `MissingChansDirectEntry`, or exports any symbol named after a module-private class or helper. Run it on a VPS build after compiling there to confirm the toolchain did not introduce a pinning symbol. r9 and r11 builds fail it, which serves as a negative control.

To confirm internal linkage does not depend on the `-fvisibility=hidden` flag that `znc-buildmod` passes, compile the source directly without it and check that build too:

```sh
g++ -fPIC -shared -isystem /path/to/znc/include -include znc/zncconfig.h \
  modules/missingchans/src/missingchans.cpp -o /tmp/missingchans-defvis.so
python3 modules/missingchans/tests/symbols.py --module /tmp/missingchans-defvis.so --znc "$(command -v znc)"
```

### Load-order diagnostic

`tests/probe_loadorder.py` reproduces the mechanism behind the fix. It starts ZNC with `LD_DEBUG=bindings`, loads the given global and network modules in order, replaces `missingchans.so`, runs `UpdateMod` once, and reports whether the running build changed and which objects bound symbols into the resident copy:

```sh
ZNC_BIN=/path/to/znc python3 modules/missingchans/tests/probe_loadorder.py \
  /tmp/missingchans-test-build/r11/missingchans.so /tmp/missingchans-test-build/variant/missingchans.so \
  --global webadmin --network missingchans,perform,highlightctx --extra /path/to/highlightctx.so
```

It makes no assertions; use it to check a specific module mix.

## Environment

| Variable | Meaning |
|---|---|
| `ZNC_BIN` | ZNC executable; defaults to `znc` on PATH. Also used by the symbol check. |
| `MC_TEST_ROOT` | Parent for unique per-scenario temporary directories; system temporary directory if unset. Existing directories are not deleted. |
| `ZNC_PRELOAD` | Optional `LD_PRELOAD` applied only to the ZNC child. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | Passed through to ZNC for instrumented runs. |

Each directory contains `znc.log`, `irc-a.log`, `irc-b.log`, and `client-N.log`, alongside the synthetic configuration and module registry. The reconnect and restart scenarios also keep the earlier transcripts. Client/server barriers and explicit event predicates are preferred to fixed sleeps. Small sleeps test absence of actions after a known deadline. Backoff checks allow scheduling tolerance; the 30-second watchdog is exercised at its real production duration.

## Coverage

| Scenarios | Assertions |
|---|---|
| `omitted_joined`, `late_join`, `user_perform` | Reproduce r9 failures; assert the r10 fixes preventing false retries remain, and that a working user-level perform is routed to its origin only. Context restoration is checked by invoking perform from the other network afterwards. |
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
| `debug_recovery_guards` | Repeats WHOIS-omission, late-JOIN, and scoped-user-perform regression checks with Debug ON. |
| `symbols` | No pinning unique symbols; both entry points exported; no module-private names exported. |
| `differential_commands`, `differential_r10_status`, `differential_r11` | Output compatibility with r9, r10, and r11 as described under [Run](#run). |
| `upgrade_from_r9`, `upgrade_from_r10`, `upgrade_from_r11` | Real binary replacement of each older revision; restart fallback when the old binary stays resident; the following update applies in place. |
| `inplace_update` | Worst-case load order (missingchans first, perform after it): six consecutive updates alternating two different files. After each, both networks run the newly installed build and report `updated in place`; all seven settings and Debug survive; the second network keeps the compact STATUS; exactly two missingchans files are mapped in the ZNC process (resident loader plus running build), so superseded builds are really unloaded; no temporary link is left behind. A retry pending before the first update never fires, and a new `RUN` still repairs. |
| `inplace_load_order` | missingchans loaded after `perform` and `route_replies`: updates apply in place the same way. |
| `inplace_reloadmod` | `ReloadMod` on one network runs the installed build there while the other network keeps the resident build; a later `UpdateMod` unifies them and unloads the superseded build. |
| `inplace_unload_load` | `UnloadMod` on both networks, then `LoadMod` with a replaced file loads the new build; `ListAvailMods` works and does not disturb it. |
| `inplace_bad_file` | A file that is not a module (and, with `--mismatch`, one built for another ZNC version) is refused: both networks keep running the previous build, `VERSION` shows the warning and reason, attached clients receive it, STATUS still works, and no temporary link remains. Installing a good file then updates normally. |
| `inplace_link_denied` | With the module directory made read-only, the update is refused with a `cannot create a temporary link` warning and the previous build keeps running; once writable again, `UpdateMod` succeeds. |
| `inplace_downgrade` | With r12 resident, installing r11 runs r11 on both networks (no `Loader:` line) with settings intact; installing the variant afterwards updates in place again. r11 stays mapped (three files mapped), as documented. |

## Sanitizers

Build separate instrumented copies of the module and of the variant:

```sh
repo="$PWD"; b=/tmp/missingchans-test-build
san='-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g'
mkdir -p "$b/san-r12" "$b/san-variant"
(cd "$b/san-r12" && CXXFLAGS="$san" LDFLAGS='-fsanitize=address,undefined' \
  znc-buildmod "$repo/modules/missingchans/src/missingchans.cpp")
(cd "$b/san-variant" && cp "$b/variant/missingchans.cpp" . && CXXFLAGS="$san" LDFLAGS='-fsanitize=address,undefined' \
  znc-buildmod missingchans.cpp)

ZNC_BIN=/path/to/znc \
ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
MC_TEST_ROOT=/tmp/missingchans-tests/sanitized \
python3 modules/missingchans/tests/suite.py \
  --module "$b/san-r12/missingchans.so" \
  --compare-r11 "$b/r11/missingchans.so" \
  --variant "$b/san-variant/missingchans.so"
```

Use the `gcc` that built the modules for `-print-file-name`. The modules are instrumented; the ZNC core need not be. The harness scans every process log and checks exit codes after termination. The in-place scenarios run the instrumented code both as the resident loader and as a private load. `tests/results/r12-sanitizer-probe.txt` records a check that a fault inside a private load is caught: a disposable variant with a deliberate use-after-free was rejected with an AddressSanitizer report. That faulty source is not part of the release.

## What this does not prove

- No test connected to the user's VPS, live IRC networks, authentication services, or production client configuration.
- The test ZNC is a local build without TLS, SASL, or third-party auth modules. The tests validate membership, module context, timers, loading, and protocol flow, not certificate/authentication correctness.
- In-place updates were tested with glibc 2.35 and 2.39 on Linux x86-64. Other C libraries, other dynamic loaders, and non-Linux systems are not covered.
- The load-order probe used the other modules from this repository and ZNC's stock modules; it cannot cover every third-party module mix. The loader is designed not to depend on load order, and the `inplace_*` tests check two very different orders.
- The upgrade checks show that r9, r10, and r11 stay resident in the harness load order; whether they do on a particular installation depends on its module mix.
- The unit of concurrency is event ordering on ZNC's normal single-threaded module path. This is not a load/performance test or an exhaustive protocol fuzzer.
- Full IRC punctuation CASEMAPPING, every CHANTYPES ambiguity, arbitrary WHOIS reordering/interception by other modules, and all custom IRCd numerics remain outside coverage.
- Channel lists use positive live membership as evidence. This cannot independently detect an IRCd/core disagreement if neither side supplies a correcting event.
- Address/undefined/leak checks cover the exercised module paths; they are not proof of the absence of all defects.
