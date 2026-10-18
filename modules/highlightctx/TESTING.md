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
| `boundary.py` | Nick-boundary and formatting scenarios (`nb_*`). |
| `coupling.py` | `ignore_drop` coupling scenarios (`idc_*`). |
| `numeric.py` | Numeric input scenarios (`num_*`). |

---

## Requirements

- ZNC and `znc-buildmod`. 0.14.0 and 0.14.1 were exercised against ZNC 1.10.3 built with GCC 13.3 and, separately, with GCC 11.5; earlier releases also against 1.9.0 and 1.9.1.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user.
- Python 3. No third-party packages.
- Two module builds: a baseline to compare against, and the version under test.
- For `ext_ignore_drop` and the `idc_*` scenarios, a build of the companion [`ignore_drop`](../ignore_drop/README.md) module. `idc_user_scope_ignore_drop` additionally needs an `ignore_drop` build that can still be loaded at user scope, which means 1.1.0 or earlier.

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
IGNORE_DROP_OLD=/tmp/b/ignore_drop-1.1.0/ignore_drop.so \
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
| `IGNORE_DROP` | — | `ignore_drop.so`, for `ext_ignore_drop` and `idc_*`. |
| `IGNORE_DROP_OLD` | — | An `ignore_drop.so` that accepts user scope (1.1.0 or earlier), for `idc_user_scope_ignore_drop`. |
| `HX_ROOT` | `/tmp/hx` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

### Selecting scenarios

Pass scenario names, or a group prefix (`reg`, `ext`, `jo`, `gc`, `cmp`, `sym`, `cm`, `nb`, `idc`, `num`):

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

#### Command surface (`reg_commands`)

`reg_commands` runs the same 27 commands, valid and invalid, on both builds and compares the transcripts command by command, in order. Repeated commands (`Status`, `ListExcludes`) are compared at each position, not only the last.

Expected differences come from `SURFACE_CHANGES` in `suite.py`, a table of every change to that output keyed by the release that made it. Each change names a line by a stable prefix and says whether it was reworded (present in both, new text), added (only in the newer version) or removed (only in the older version). The scenario reads both builds' version markers and applies the entries newer than the baseline and no newer than the build under test. For each command with expected changes, it then asserts:

- every removed line is a recorded rewording or removal, and every added line is a recorded rewording or addition;
- every recorded change actually happened, exactly once;
- lines common to both keep their relative order.

Every other command must be identical. Version numbers and per-scenario paths are normalised before comparison.

The comparison is therefore exact for any baseline from 0.8.0 onward, the oldest version with the current command set. An older baseline fails a dedicated check, as does a build under test that is older than its baseline.

**When a release changes command output, add an entry to `SURFACE_CHANGES`.** Without one, the scenario fails against any older baseline. That is the intended signal: a command-output change has to be recorded deliberately.

The table was validated against real builds of 0.8.0, 0.9.0, 0.11.1, 0.12.0 and 0.13.0, in all ten older-to-newer pairings, and 0.14.0 against each of those five as baseline. 0.10.0 and 0.11.0 have no standalone builds in the repository, so their split of the 0.9.0 → 0.11.1 changes follows the CHANGELOG. It was also checked against mutant builds: an unrecorded change, a recorded change missing, a recorded removal that did not happen (0.14.0 with the old `SetRequireIgnoreDrop on` refusal restored), a baseline older than 0.8.0, and the two builds swapped. Each one fails.

### `ext_*` — overlapping-highlight extension

Window boundaries (a trigger on the last line of the window extends; one on the first line after does not); chained triggers; partial extended events; spam bursts; multi-channel independence; triggers that must not extend (self, excluded, `ignore_drop`-dropped); cap sourcing across a reload; crash recovery mid-extension; compaction round-trips; hand-crafted journals with malformed, duplicate, out-of-range, and premature records; upgrade from a journal containing overlapping open events; downgrade; and `UpdateMod`.

### `jo_*` — journaling opt-out

That `journal=off` writes nothing to disk at all; that in-session replay is identical with journaling on and off; that state is genuinely lost across a restart; NV persistence of the setting; that `Reset` never re-enables it; leftover-file handling (ignored, reported, removed only by `Compact`); re-enabling; and load-argument validation.

### `cmp_*` — journal compaction

That a large pending queue no longer triggers a rewrite on every channel line; that compaction count grows with journal size rather than with appends; that the file stays within about twice live state; that dead records are still reclaimed; that recovery from a less-compacted journal reproduces events exactly, including extension targets and capped flags; that journals below the 512-line floor behave byte-identically to the previous version; and that replay resets the trigger point.

### `cm_*` — RFC 1459 casemapping

That a bracketed nick is matched in its case-equivalent form (asserted differentially against the baseline, which does not match it), that literal bracketed nicks and channels still work, that exclusions can be added and removed across spellings, and that ASCII case handling and self-detection are unchanged.

These use the harness's per-scenario nick support (`mk(..., nick="ti[m]")`), since the effect is only observable with a nick containing `[ ] \ ~`.

### `nb_*` — nick boundaries and formatting

What counts as a mention of your nick. Each probe is sent into its own channel with `before=0 after=0`, so every line is judged on its own and no result depends on window extension.

- **Decorated mentions** — symbol-only nick characters touching the nick (`tim-`, `tim_`, `tim^^`, `[tim]`, `_tim_`, and similar, including uppercase, end-of-line, and a non-ASCII character after the decoration) trigger. Asserted differentially: the baseline misses every one. The exact reported line shape, `fauxpride- i`, has its own scenario with full context checked.
- **Embedded forms** — a letter or digit anywhere in the touching run (`timothy`, `xtim`, `tim_2`, `tim-tam`, `tim|away`, `[tim]x`) still does not trigger.
- **Plain mentions** — undecorated forms give the same result as the baseline.
- **Formatting** — `\x03` colours with one or two digits and a background, `\x04` hex colours, a literal comma after a colour, and bold/italic/underline/reverse no longer hide a mention; the baseline misses the cases where colour digits touch the nick. Formatting splitting a longer word (`\x0312tim\x03othy`), and `\x03,05` (no foreground, so `,05` is text), do not trigger.
- **Someone else's nick** — with `tim_`, `[tim]` and `TIM^` in the channel, mentions spelled as those nicks, including RFC 1459 and ASCII case-equivalent spellings, do not trigger; other decorations still do, and a second qualifying occurrence on the same line does. The guard follows the live nick list through `PART`, `QUIT` and `NICK` in both directions, and is per channel.
- **Interaction with existing rules** — decorated mentions trigger from notices and actions, never from excluded senders or yourself, and extend an open event like any other trigger. Own nicks containing or ending in symbols (`ti-m`, `tim_`, `ti[m]`) match correctly. After a server-side rename to an alternate nick (`tim-`, `tim_`), matching follows the nick in use: every baseline hit is kept, decorated forms are added, and the old nick no longer matches.

The nick-list scenarios inject `JOIN`, `PART`, `QUIT` and `NICK` through `ircd.raw()`. A mutation build with the nick-list check disabled fails both of them, which confirms they discriminate.

`reg_nick_boundaries` keeps its byte-identical comparison with the baseline by using only inputs whose result did not change (`tim_x`, `[tim]x`); the inputs whose result did change are covered here with explicit deltas.

The contrast checks that expect the baseline to miss decorated and coloured mentions apply only to a baseline older than 0.13.0. Against a 0.13.0 or later baseline they assert that both builds match the same forms instead. `nb_version_marker` accepts any version from 0.13.0 on; `idc_version_marker` pins the current one.

### `idc_*` — `ignore_drop` coupling

`ignore_drop` protects `highlightctx` only while ZNC dispatches messages to it first: ahead of `highlightctx` in the network's module list, or loaded at user scope. These scenarios load both modules in a chosen order, add an always-scope rule for `mallory`, and send the same traffic each time while detached: one highlight from `mallory` and one from `bob`. They assert on the replay, the pause note at its top, and the `Status` lines. Most run the baseline as well, to show the problem they guard against.

- **Reload reverses the order** (`idc_reloadmod_pauses`, `idc_updatemod_pauses`). `ReloadMod` and `UpdateMod` of `ignore_drop` re-append it behind `highlightctx`. The baseline then captures and replays `mallory`. The build under test shows the live position, a pause note and capture inactive in `Status`; captures nothing; reports exactly 2 missed highlights at the next replay; and has cleared the count afterwards.
- **Restoring the order** (`idc_restore_with_updatemod`). `UpdateMod highlightctx` after the reversal: armed and capturing again, `bob` captured, `mallory` dropped, no pause note.
- **Giving up protection** (`idc_rearm_gives_up_protection`). `Rearm` after the reversal disarms, warns that capture is now unprotected, points at `UpdateMod highlightctx`, and capture then includes `mallory`, as documented.
- **Explicit re-checks** (`idc_mode_change_rechecks`). `SetRequireIgnoreDrop auto` and `Reset` disarm a sticky armed state with `ignore_drop` behind; reloading `highlightctx` arms it again.
- **`on` with the wrong `znc.conf` order** (`idc_on_mode_conf_order`). `highlightctx require_ignore_drop=on` listed before `ignore_drop`. The baseline makes ZNC exit with `Unrecoverable config error`. The build under test starts, notes the pause in its load message, is paused with `ignore_drop` behind, reports both highlights as missed, and captures with protection after `UpdateMod highlightctx`.
- **`on` without `ignore_drop`** (`idc_on_mode_without_ignore_drop`). `SetRequireIgnoreDrop on` is accepted with the exact reply text, `Status` says how to load `ignore_drop`, one missed highlight is reported, a runtime `LoadMod` of `ignore_drop` lands behind and keeps capture paused, and `UpdateMod highlightctx` resumes it.
- **Restart after a reversed config** (`idc_sticky_across_restart`). `UpdateMod ignore_drop`, `SaveConfig`, restart. The written config lists `highlightctx` first. The baseline boots unarmed and captures `mallory`; the build under test boots armed from NV, pauses, and reports 2 missed.
- **Unload** (`idc_unload_pauses_and_reports`). Both builds pause when an armed `ignore_drop` is unloaded; only the build under test reports the missed highlights, naming the cause.
- **What is counted** (`idc_missed_counting`). Self lines, excluded senders, excluded channels and ordinary chatter are not counted; text, notice, action and decorated mentions are, 4 in all. The count survives `SIGKILL` and is cleared by the replay.
- **User-scope `ignore_drop`** (`idc_user_scope_ignore_drop`, needs `IGNORE_DROP_OLD`). `ignore_drop` loaded from a user-level `LoadModule` line. In `auto` mode the baseline reports it as not loaded and unarmed, while the build under test labels it as a user-scope instance and arms; both drop `mallory`, because the user-scope instance runs first either way. In `on` mode the baseline keeps ZNC from starting; the build under test starts and captures with protection.

Contrasts with the baseline apply to a baseline older than 0.14.0. With a 0.14.0 or later baseline, each one asserts instead that the baseline behaves like the build under test. `idc_version_marker` accepts any version from 0.14.0 on.

The harness builds these configurations with the stock `Znc` class plus a subclass that adds user-level `LoadModule` lines. Status assertions match lines by their stable prefix, as `reg_commands` does.

The scenarios were checked against four mutant builds of 0.14.0: the live gate reverted to a presence check, the armed state not restored from NV, user scope not recognised, and missed highlights not counted. Each fails the `idc_*` scenarios aimed at it.

### `num_*` — numeric input

Counts, caps and list indexes. Up to 0.14.0 they were parsed with `CString::ToUInt`, which wraps modulo 2³² instead of rejecting a value that does not fit.

- **Wrapping values rejected** (`num_wrapping_rejected`). `SetBefore`, `SetAfter`, `SetMaxEvents` and `SetMaxEventLines` with values chosen to wrap onto plausible settings (8, 9, 100, 1) are rejected with each command's usage line, and `Status` shows the earlier values unchanged.
- **`DelExclude` indexes** (`num_delexclude_index`). With a channel and a nick exclusion, `4294967297`, `0`, a 20-digit number and an index past the end are all `out of range` and remove nothing; `2` and `01` still remove the right entries.
- **Boundaries** (`num_boundaries`). `4294967295` is accepted and `4294967296` rejected; any number of leading zeros is accepted; `-1`, `+5`, `1e3` and `5x` are rejected.
- **Natural-size note** (`num_natural_size_note`). With `before` at `4294967295`, the `SetMaxEventLines` note shows the full 64-bit sum.
- **Load arguments** (`num_load_args`). A runtime `LoadMod` with a wrapping value for each of the four numeric arguments fails with the existing error message, and a plain reload afterwards keeps the earlier settings. These replies are collected through `*status`, using ZNC's own `Version` reply as the end marker, since `*highlightctx` is not loaded at that point.
- **Load argument in `znc.conf`** (`num_conf_load_arg`). The same value in `znc.conf` stops ZNC from starting, naming the argument. This is the one way the fix can affect an existing setup, so it is recorded.

Against a baseline older than 0.14.1, the suite also asserts that the baseline wraps in each case, so the scenarios are known to discriminate. With a newer baseline those checks are skipped or become equality checks. `num_version_marker` pins the current release.

The scenarios were checked against two mutant builds of 0.14.1: the parser reverted to `ToUInt`, and the natural-size sum computed in 32 bits. Each fails the scenarios aimed at it.

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

- **Single-user, single-network.** Multi-user and multi-network ZNC configurations are not exercised. In particular, `UpdateMod ignore_drop` reorders every network at once; the suite checks one.
- **No concurrency testing.** ZNC is single-threaded for module hooks, so the suite makes no attempt to race attach against capture.
- **No IRCv3 beyond `server-time`.** Other capabilities, and message tags generally, are untested.
- **Timing.** Wall-clock comparisons are deliberately avoided: the fake server feeds lines serially, so the harness dominates and any timing figure would be meaningless. Performance claims should be made with a purpose-built benchmark, not this suite.
- **Journal write volume beyond compaction.** The `cmp_*` scenarios cover how often the journal is rewritten, but not the per-append `fsync` cost, which is inherent to the durability design and is not measured here.
- **Character encodings.** Non-UTF-8 and mixed-encoding channel traffic is not covered. Nick matching treats bytes above `0x7F` as word boundaries, as it always has; a nick adjacent to non-ASCII letters is tested in one direction only (`tim-ș`).
- **Channel-name casemapping end to end.** ZNC resolves channel names with ASCII case-insensitivity before the module sees them, so a case-equivalent channel spelling cannot be delivered to the module in a test. The `cm_*` scenarios cover the module's own folding, not ZNC's routing.
