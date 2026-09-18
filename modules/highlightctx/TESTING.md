# Testing `highlightctx`

Reference for the module's test suite: what it verifies, how to run it, and what it does not cover.

The suite drives a **real ZNC process** against a fake IRC server and asserts on the actual replay delivered to an IRC client. The module is never mocked, stubbed, or compiled into a test harness; every assertion is made against a loaded `.so` running inside ZNC.

---

## Layout

Everything lives under [`tests/`](./tests/), self-contained within the module:

| File | Contents |
|---|---|
| `hx.py` | Harness. Fake IRC server, ZNC process manager, IRC client, replay parser. |
| `suite.py` | Runner, differential-regression scenarios (`reg_*`), extension scenarios (`ext_*`). |
| `journal_off.py` | Journaling opt-out scenarios (`jo_*`). |
| `growth.py` | Growth-control scenarios (`gc_*`). |
| `compaction.py` | Journal compaction scenarios (`cmp_*`). |
| `symbols.py` | Symbol-hygiene scenarios (`sym_*`). |
| `casemap.py` | RFC 1459 casemapping scenarios (`cm_*`). |

---

## Requirements

- ZNC and `znc-buildmod`. The suite is exercised against 1.9.0 and 1.9.1.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user.
- Python 3. No third-party packages.
- Two module builds: a baseline to compare against, and the version under test.
- For `ext_ignore_drop`, a build of the companion [`ignore_drop`](../ignore_drop/README.md) module.

---

## Building the modules

```sh
mkdir -p /tmp/b/base /tmp/b/new
cp <baseline>/highlightctx.cpp /tmp/b/base/ && (cd /tmp/b/base && znc-buildmod highlightctx.cpp)
cp <new>/highlightctx.cpp      /tmp/b/new/  && (cd /tmp/b/new  && znc-buildmod highlightctx.cpp)
```

## Running

```sh
cd modules/highlightctx/tests

V080=/tmp/b/base/highlightctx.so \
V090=/tmp/b/new/highlightctx.so \
IGNORE_DROP=/tmp/b/ignore_drop/ignore_drop.so \
HX_ROOT=/tmp/hx \
python3 suite.py
```

`V080` names the baseline build and `V090` the build under test. The names are historical and do not imply particular versions; the suite reads the baseline's version at startup and adapts the assertions that are specific to pre-extension (0.8.0) behaviour.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `ZNC_BIN` | `/usr/bin/znc` | ZNC binary to run. |
| `V080` | — | Baseline module `.so`. |
| `V090` | — | Module `.so` under test. |
| `IGNORE_DROP` | — | `ignore_drop.so`, for the integration scenario. |
| `HX_ROOT` | `/tmp/hx` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

### Selecting scenarios

Pass scenario names, or a group prefix (`reg`, `ext`, `jo`, `gc`, `cmp`, `sym`, `cm`):

```sh
python3 suite.py gc                    # growth-control scenarios only
python3 suite.py ext_tim_exact         # a single scenario
```

The process exits non-zero if any assertion fails. Each failure prints the scenario, the assertion, and the observed value.

---

## Sanitizer runs

Build both modules with sanitizers and preload the runtimes into ZNC:

```sh
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g" \
LDFLAGS="-fsanitize=address,undefined" znc-buildmod highlightctx.cpp

ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
V080=... V090=... python3 suite.py
```

After each scenario the suite scans that ZNC instance's log for sanitizer output and fails the run on a hit.

A clean sanitizer run proves nothing unless the plumbing is known to work. Verify it by injecting a deliberate fault into a throwaway copy of the source — an out-of-bounds write, or a leaked allocation with `detect_leaks=1` — and confirming the suite reports it.

---

## Harness behaviour

**Fake IRC server.** Accepts one ZNC connection, completes registration, answers `PING`, and joins any channel ZNC requests. It advertises `server-time` and stamps every injected line, so event timestamps are deterministic. Its clock can be set (`ircd.ts`) to inject lines that are arbitrarily old, which is how expiry is tested without waiting.

**ZNC process manager.** Generates a `znc.conf` with one user, one network, and the module load line; installs module `.so` files by atomic rename, never overwriting a mapped file in place; and supports graceful stop, `SIGKILL`, and restart against the same data directory. Each scenario gets its own data directory under `HX_ROOT`, left in place afterwards for inspection. `znc.out` in each holds that instance's debug log.

**Client.** Logs in over the listener, optionally negotiating `server-time`, captures the replay delivered during attach, and issues module commands. Because the module replays during attach, a command sent immediately afterwards acts as a sentinel marking the end of the replay.

**Replay parser.** Converts module output into structured events: channel, id, state, `before`/`after` counts, target, `triggers`, `capped`, and per-line text with its trigger marker and timestamp.

---

## Coverage

### `reg_*` — differential regression

Runs identical traffic through the baseline and the build under test. Replay output and the on-disk journal must be **byte-identical**, and the full command surface must match except where a difference is expected, in which case the specific delta is asserted rather than tolerated.

Covers: complete and partial events; a second highlight after the window closes; cross-channel traffic; nick, hostmask, and channel exclusions; self lines; `after=0` and `before=0`; notice and action message kinds; `max_events`; the inline-timestamp fallback client; nick-boundary matching; ring clearing on detach; pending and open events across a `SIGKILL` restart; and every command including invalid input.

### `ext_*` — overlapping-highlight extension

Window boundaries (a trigger on the last line of the window extends; one on the first line after does not); chained triggers; partial extended events; spam bursts; multi-channel independence; triggers that must not extend (self, excluded, `ignore_drop`-dropped); cap sourcing across a reload; crash recovery mid-extension; compaction round-trips; hand-crafted journals with malformed, duplicate, out-of-range, and premature records; upgrade from a journal containing overlapping open events; downgrade; and `UpdateMod`.

### `jo_*` — journaling opt-out

That `journal=off` writes nothing to disk at all; that in-session replay is identical with journaling on and off; that state is genuinely lost across a restart; NV persistence of the setting; that `Reset` never re-enables it; leftover-file handling (ignored, reported, removed only by `Compact`); re-enabling; and load-argument validation.

### `cmp_*` — journal compaction

That a large pending queue no longer triggers a rewrite on every channel line; that compaction count grows with journal size rather than with appends; that the file stays within about twice live state; that dead records are still reclaimed; that recovery from a less-compacted journal reproduces events exactly, including extension targets and capped flags; that journals below the 512-line floor behave byte-identically to the previous version; and that replay resets the trigger point.

### `cm_*` — RFC 1459 casemapping

That a bracketed nick is matched in its case-equivalent form (asserted differentially against the baseline, which does not match it), that literal bracketed nicks and channels still work, that exclusions can be added and removed across spellings, and that ASCII case handling and self-detection are unchanged.

These use the harness's per-scenario nick support (`mk(..., nick="ti[m]")`), since the effect is only observable with a nick containing `[ ] \ ~`.

### `sym_*` — symbol hygiene

These inspect the built `.so` with `nm` rather than running ZNC.

A library that defines an `STB_GNU_UNIQUE` symbol which nothing else in the process defines is marked non-unloadable by glibc. `dlclose()` then does nothing, so `/znc updatemod` reloads the already-resident old code while reporting success, and the module only updates after a full ZNC restart. libstdc++ emits such symbols for some inline internals — `std::to_string` reaches them on GCC 11 — so the property has to be checked per build rather than assumed.

The scenarios assert that the module defines no unique symbol the `znc` binary lacks, that `__to_chars_10_impl` and `piecewise_construct` specifically do not reappear, and that the build under test is no worse than the baseline. They need `nm` and `c++filt` from binutils, and `ZNC_BIN` pointing at the same ZNC the module will be loaded into.

### `gc_*` — growth controls

Defaults; explicit disabling; rewrite frequency at stock defaults; `max_event_lines` boundary arithmetic; that a cap below the natural event size never truncates the configured window; that the next highlight after a capped event starts a fresh one; live evaluation of the cap; capped events surviving `SIGKILL` and compaction with their target intact; a cap lowered between sessions; flood bounding; `max_event_age` expiry during operation and at load; drop reporting for both causes; drop counters surviving a restart; absence of a drop note in the normal case; and that the default cap retains the newest 100 events.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold. Where arithmetic matters — which after-index a trigger lands on, what target results — the scenario docstring works it through, because an off-by-one in the scenario otherwise looks like a module bug.

Scenarios that depend on baseline-specific behaviour gate on `baseline_version()` rather than assuming a particular version.

---

## Known gaps

- **Single-user, single-network.** Multi-user and multi-network ZNC configurations are not exercised.
- **No concurrency testing.** ZNC is single-threaded for module hooks, so the suite makes no attempt to race attach against capture.
- **No IRCv3 beyond `server-time`.** Other capabilities, and message tags generally, are untested.
- **Timing.** Wall-clock comparisons are deliberately avoided: the fake server feeds lines serially, so the harness dominates and any timing figure would be meaningless. Performance claims should be made with a purpose-built benchmark, not this suite.
- **Journal write volume beyond compaction.** The `cmp_*` scenarios cover how often the journal is rewritten, but not the per-append `fsync` cost, which is inherent to the durability design and is not measured here.
- **Character encodings.** Non-UTF-8 and mixed-encoding channel traffic is not covered.
- **Channel-name casemapping end to end.** ZNC resolves channel names with ASCII case-insensitivity before the module sees them, so a case-equivalent channel spelling cannot be delivered to the module in a test. The `cm_*` scenarios cover the module's own folding, not ZNC's routing.
