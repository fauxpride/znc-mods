# Testing `keepnick_instant`

Reference for the module's test suite: what it verifies, how to run it, and what it does not cover.

The suite drives a **real ZNC process** against fake IRC servers. The module is never mocked, stubbed, or compiled into a test harness; every assertion is made against a loaded `.so` running inside ZNC. The only exception is `sym_unique`, which inspects the built file.

The fake server keeps track of which nicks other (simulated) users hold. It refuses ZNC's `NICK` to a held nick with `433`, or on request with `433` or `437` even when the nick is free, answers `ISON`, and answers `MONITOR` in one of four ways chosen per scenario. Assertions are made on the `ISON`, `MONITOR` and `NICK` lines ZNC sends to the server, **with their arrival times relative to registration**, on what the client receives (in particular, which numerics it does not see), and on the module's NV store.

The module is timer-driven, so the suite runs in real time. Timing assertions allow 0.6 seconds of slack.

---

## Layout

Everything lives under [`tests/`](./tests/), self-contained within the module:

| File | Contents |
|---|---|
| `hx.py` | Harness. Fake IRC server, ZNC process manager, IRC client. A separate copy of the shared harness, extended with nick ownership, `433`/`437`, `ISON`, `MONITOR`, connection drops and a timestamped log of what ZNC sends. |
| `suite.py` | Runner and all scenarios. |

---

## Requirements

- ZNC and `znc-buildmod`. The suite is exercised against ZNC 1.10.3 built with GCC 13.3 and with GCC 11.5.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user. ZNC, its module directory and `HX_ROOT` must be reachable by `znctest`.
- Python 3. No third-party packages.
- `nm` and `c++filt` from binutils, for `sym_unique` and for deciding what `up_inplace` should expect.
- Two module builds: the version under test, and a **probe**: the same source rebuilt with a different version string, so a successful in-place update is visible.
- A machine that is not heavily loaded. Run one suite at a time; a busy CPU delays ZNC's timers and can fail the timing assertions.

---

## Building the modules

```sh
mkdir -p /tmp/b/new /tmp/b/probe
cp src/keepnick_instant.cpp /tmp/b/new/ && (cd /tmp/b/new && znc-buildmod keepnick_instant.cpp)
sed 's/#define KEEPNICK_INSTANT_VERSION "[^"]*"/#define KEEPNICK_INSTANT_VERSION "9.9.9"/' src/keepnick_instant.cpp \
    > /tmp/b/probe/keepnick_instant.cpp && (cd /tmp/b/probe && znc-buildmod keepnick_instant.cpp)
```

Since 1.7.0, `Show` and the module description both take the version from the `KEEPNICK_INSTANT_VERSION` define. For 1.6.9 and earlier, which repeat the version string in the source, replace every occurrence instead (for example `sed 's/1\.6\.9/9.9.9/g'`). Build both with the same compiler as the ZNC they will run in.

## Running

```sh
cd modules/keepnick_instant/tests

ZNC_BIN=/usr/local/bin/znc \
NEW=/tmp/b/new/keepnick_instant.so \
PROBE=/tmp/b/probe/keepnick_instant.so \
HX_ROOT=/tmp/kni \
python3 suite.py
```

A full run takes six to seven minutes, almost all of it waiting for the module's timers.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `ZNC_BIN` | `/usr/bin/znc` | ZNC binary to run. |
| `NEW` | — | Module `.so` under test. Required. |
| `PROBE` | — | The build under test with another version string. Needed by `up_inplace`, which fails without it. |
| `HX_ROOT` | `/tmp/keepnick_instant-tests` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

### Selecting scenarios

Pass scenario names, or a group prefix (`is`, `mon`, `ev`, `ac`, `cfg`, `be`, `lc`, `ty`, `up`, `sym`):

```sh
python3 suite.py be                 # backend-selection scenarios only
python3 suite.py lc_reconnect       # a single scenario
```

The process exits non-zero if any assertion fails. Each failure prints the scenario, the assertion, and the observed value, with times in seconds since the server's `001`.

---

## Sanitizer runs

Build the module with sanitizers and preload the runtimes into ZNC:

```sh
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g" \
LDFLAGS="-fsanitize=address,undefined" znc-buildmod keepnick_instant.cpp

ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
NEW=... PROBE=... python3 suite.py
```

After each scenario the suite scans that ZNC instance's log for sanitizer output and for `Segmentation`, and fails the run on a hit. A clean sanitizer run proves nothing unless the plumbing is known to work: verify it by injecting a deliberate fault into a throwaway copy of the source and confirming the suite reports it.

---

## Harness behaviour

**Fake IRC servers.** One per ZNC network. Each accepts ZNC's connection and completes registration. A `NICK` to a nick another user holds is refused with `433`, so ZNC registers on its alternate nick (`tim_`) when `tim` is held; anything other than `CAP`, `NICK`, `USER`, `PASS`, `PING`, `PONG` and `QUIT` sent before registration is answered with `451`, as a real server does. After registration a `NICK` is refused with `433` if the nick is held, or with `433` or `437` if the scenario has asked for that nick to be refused; otherwise the change is confirmed. `ISON` lists the asked-for nicks that are held, including ZNC's own. `MONITOR` behaves in one of four ways:

| Mode | `005` | Behaviour |
|---|---|---|
| none | no `MONITOR` | `MONITOR` is answered with `421`. |
| `ok` | `MONITOR=100` | `MONITOR +` answers `730` for online nicks (including ZNC's own) and `731` for the others; a monitored nick that comes or goes produces `730` or `731`. |
| `full` | `MONITOR=100` | Every `MONITOR +` is answered with `734`. |
| `silent` | `MONITOR=100` | `MONITOR` gets no reply at all. |

A scenario can hand a nick to another user or take it away, silently or with a `NICK` or `QUIT` that ZNC sees in a shared channel, send any raw line, and drop the connection so ZNC reconnects. Every line ZNC sends is recorded with its arrival time; times are reported relative to the moment the server sent `001`, which is stamped before `001` goes out.

**ZNC process manager.** Generates a `znc.conf` with one user (`Nick = tim`, `AltNick = tim_`) and any number of networks, each pointing at its own fake server. `ConnectDelay` and `ServerThrottle` are set to 0, so a reconnect happens at once, and each network's `FloodRate` to -1, so ZNC's flood protection never delays the module's lines. Most scenarios seed the module's NV store with fast settings before ZNC starts: `StartDelaySec = 2`, `IntervalSec = 5`, `IdleGapSec = 0`, `MinGapSec = 0`. The manager reads the NV store back afterwards, installs `.so` files by atomic rename, and supports graceful stop, `SIGKILL`, and restart against the same data directory. Each scenario gets its own data directory under `HX_ROOT`; `znc.out` in each holds that instance's debug log.

**Client.** Logs in to one network. Every request is followed by a `PING` to ZNC, and ZNC's `PONG` marks the point where all output caused by the request has arrived. Before any assertion about what the client did or did not receive, the suite reads everything ZNC has sent so far, so a "never seen" assertion covers the whole scenario. The client's `PING`s do not count as activity to the module, so they do not disturb the idle and timing scenarios.

---

## Coverage

### `is_*` and `mon_*` — the two backends, from connect to reclaim

- **ISON server** (`is_startup`). With `tim` held, ZNC registers as `tim_`. Nothing is sent before `StartDelay`; the first action is one `MONITOR + tim` probe at `StartDelay`, whose `421` the client never sees; then `ISON tim` every `Interval`, whose `303` replies the client never sees. When `tim` frees up silently, the next `ISON` tick is followed immediately by `NICK tim`, the client sees the nick change, and the module sends nothing more.
- **MONITOR server** (`mon_startup`). The first action is `MONITOR + tim` at `StartDelay` (answered `730`). No `ISON` and no `MONITOR S` are sent while waiting. When `tim` goes offline, the server's `731` is followed by `NICK tim` within half a second, and the next tick, now owning the nick, sends `MONITOR - tim`. `Show` reports the effective backend, the `MONITOR=100` limit, and the subscription state before and after.
- **Refused attempts on MONITOR** (`mon_refused`). After the `731`, every `NICK tim` is refused, first with `433`, then with `437`. The client sees neither; the module retries once per `Interval` on its remembered offline state, without `MONITOR S`, and takes the nick once the refusal stops.
- **`437` on ISON** (`is_refused_437`). The nick is free but locked, as during a ratbox-style post-split delay. One attempt per `ISON` tick, each `437` swallowed, and the nick is taken when the lock ends.

### `ev_*` — visible `NICK` and `QUIT`

- **Reactive reclaim** (`ev_visible`). With `StartDelay` at 600 so no tick runs, the holder of `tim` changes nick in a shared channel (network n1) or quits (n2): `NICK tim` follows within half a second on each. An unrelated `NICK`, an unrelated `QUIT`, and another user taking `tim` cause nothing.
- **`MinGap`** (`ev_mingap`). `MinGap 3`, `tim` refused with `433`. Three `QUIT` events within half a second give one attempt; another `QUIT` after `MinGap` gives a second. The dedupe timer re-arms the scheduler only when no tick is pending; here the `StartDelay` tick is pending, so no polling starts early.

### `ac_*` — activity and the client's own traffic

- **`IdleGap`** (`ac_idle_gap`). `IdleGap 4`. While the client sends a channel message every second, no tick runs; once it stops, ticks resume within `Interval + IdleGap`. With a `PING` every second instead, and separately a `PONG`, ticks run on schedule: the probe at `StartDelay`, then `ISON` every `Interval`.
- **User `ISON` and `NICK`** (`ac_user_traffic`). Every reply to a user's `ISON` reaches the client. `ISON tim` while `tim` is held, `ISON bob`, and `ISON bob carol` cause nothing more. A manual `NICK tim` that fails shows its `433` to the client. `ISON bob tim` after `tim` frees up gets its (empty) reply and makes the module reclaim at once.
- **Reply order** (`ac_ison_order`). The user's `ISON bob` and a module `ISON tim` (from `Poke`, with `Backend Ison`) are sent in one write, the user's first. The client must receive exactly the reply to its own query, the empty one, and not the module's reply, which lists `tim`; the module must not send `NICK`.

### `cfg_*` — commands and settings

- **Settings commands** (`cfg_commands`). 31 commands compared reply by reply: each numeric setting at its upper bound and one past it; `Interval` one below its lower bound; non-numeric input for `Interval`, `IdleGap`, `StartDelay` and `Jitter`; a signed value for `Interval` (`+6`) and `MinGap` (`-1`); `IdleGap` without a value; `Interval 4294967301` and `IdleGap 4294967297`, which would wrap into range; all of these must answer with the usage line and change nothing; the lower bounds `IdleGap 0`, `Jitter 0` and `Interval 5`; `Backend` with each mode in mixed case and with an invalid one; `SetNick` without an argument; an unknown command; `Disable` and `Enable`. `Show`, `Help` and an empty command must all produce the same output, which reflects the settings, and the NV store must hold every setting.
- **`SetNick`** (`cfg_setnick`). A leading `:`, a comma, a space and a tab are each rejected with the injection-safety message. A valid new nick that someone holds starts work at once (`MONITOR` probe, then `ISON`) despite `StartDelay` 600, is reclaimed when it frees up, and is stored and shown.
- **Primary nick at load** (`cfg_load_args`). Five combinations of module argument and stored `PrimaryNick`: a valid argument wins and is stored; an invalid argument is warned about and the stored nick is used; an invalid stored nick is warned about, deleted, and the configured user nick is used; both invalid give both warnings; neither gives the user nick.
- **`Disable` and `Enable`** (`cfg_enable_disable`). While disabled, nothing is sent and a visible `QUIT` of the holder causes no reclaim; the state survives a restart; `Enable` checks at once and reclaims.
- **Persistence** (`cfg_persist`). Seven settings changed by command are shown unchanged after a restart.
- **Stored values** (`cfg_stored_values`). Stored settings that are non-numeric (`IntervalSec abc`, `MinGapSec x`), negative (`StartDelaySec -1`), too large (`JitterSec 11`), or beyond 32 bits (`IdleGapSec 4294967297`, which would wrap to 1) all load as their defaults.

### `be_*` — backend selection and `MONITOR` edge cases

- **Forced modes** (`be_modes`). `Backend Ison` on a MONITOR server sends only `ISON`, every `Interval`. `Backend Monitor` on a server without `MONITOR` probes, swallows the `421`, falls back to `ISON`, and `Show` reports `Effective: ISON`.
- **List full** (`be_monitor_full`). On `734` the module says so in its window and falls back to `ISON` one `Interval` later, without a second `MONITOR +`. The `734` itself reaches the client.
- **Silent server** (`be_monitor_silent`). A server that advertises `MONITOR` but never answers it. One probe at `StartDelay`. A `Poke` a second later sends nothing, since the probe is still outstanding, and re-arms the schedule. At the next tick, one `Interval` after the `Poke`, the module says `MONITOR got no reply on this connection; falling back to ISON.` and sends `ISON` in the same tick, then every `Interval`; `Show` reports `Effective: ISON`. When `tim` frees up silently, the next `ISON` reclaims it.
- **Late reply** (`be_monitor_late`). After that fallback, a `731` for `tim` that the server sends late is acted on at once: `NICK tim` within half a second, without waiting for the next `ISON`, and `Show` reports `Effective: MONITOR` again.
- **Unrelated `730`** (`be_monitor_unrelated`). A `730` for a nick the module did not ask about leaves `Active: no`.

### `lc_*` — lifecycle

- **Hot load** (`lc_hot_load`). `LoadMod` into a live session with default settings waits one `Interval` (5 s), not `StartDelay` (90 s). On a MONITOR server whose `005` the module never saw, with ZNC already on `tim`, the first tick probes `MONITOR` so support is detected, and the next tick unsubscribes. On an ISON server with `tim` held, the first tick probes, the `421` is swallowed, and `ISON` follows one `Interval` later. On a server that advertises `MONITOR` but never answers, with ZNC on `tim`, the first tick probes and the next announces the fallback; nothing is polled, since the nick is owned.
- **Reconnect** (`lc_reconnect`). After the server drops the connection, `StartDelay` applies again from the new `001`, and there is exactly one tick chain: one `ISON` per `Interval`, with no duplicates from a timer left over from before.
- **`StartDelay 0`** (`lc_startdelay_zero`). Nothing is sent before registration has finished: the tick armed at load time fires while ZNC is still registering and does nothing. After `001` the first tick runs at once, then one per `Interval`, with no tight loop.
- **`Poke`** (`lc_poke`). With `StartDelay` 600, `Poke` runs a check at once (a probe on an ISON server); after the nick frees up, a second `Poke` sends `ISON` and `NICK` at once.
- **Jitter** (`lc_jitter`). With `Jitter 2`, every gap between `ISON` lines lies between `Interval` and `Interval + 2` seconds.

### `ty_*`, `up_*`, `sym_*`

- **Runtime load** (`ty_runtime_load`). Plain `LoadMod` gives a network instance; `--type=user` and `--type=global` are refused.
- **`/znc updatemod`** (`up_inplace`). The running build is replaced by the probe and updated: `UpdateMod` must report `Done`, `Show` must report the probe's version, and a setting changed before the update must carry over. If the build defines an `STB_GNU_UNIQUE` symbol that the `znc` binary lacks, glibc cannot unload it and the old code keeps running; the suite decides which outcome to expect per build with `nm`.
- **Symbol hygiene** (`sym_unique`). The build defines no `STB_GNU_UNIQUE` symbol the `znc` binary lacks.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold: the exact sequence of `ISON`/`MONITOR`/`NICK` lines, when they arrived, and the exact reply lines and NV text.

Behaviour that is arguably wrong but current is asserted as it is and marked **characterised** in the scenario's description, so that a change shows up as a failure and is then a deliberate decision.

The suite was checked against mutated builds to confirm that it discriminates. For 1.7.0:
- the registration guard removed;
- the unanswered-`MONITOR` fallback removed;
- the fallback triggered by `Poke` and without a grace period;
- a late `730`/`731` ignored after the fallback;
- the user's `ISON` recorded as the module's;
- replies matched last-in-first-out;
- any user `ISON` without the primary nick triggering a reclaim;
- `PING` counted as activity;
- non-numeric setting values read as 0.

Carried over from 1.6.9:
- the `303` swallow removed;
- the `433` swallow removed;
- the reaction to a visible `NICK` removed;
- the `IdleGap` check removed;
- the timer cleanup on disconnect and reconnect removed, which produces a duplicate tick chain;
- the match gate on `730`/`731` removed;
- commas allowed in nicks.

Each one failed the scenarios aimed at it. The suite as it stood for 1.6.9 was also run against 1.7.0: it failed only on the checks that characterised the behaviour 1.7.0 changes.

---

## Known gaps

- **Stale `ISON` records.** Each `ISON` is recorded so its `303` can be matched to it. If a query is never answered, for example because another module halted the user's `/ison` after this module recorded it, the record is dropped once it is 60 seconds old; until then later replies are attributed one query late. Neither the halting nor the 60-second expiry is exercised.
- **Real IRC daemons.** The server is simulated. Behaviour on a specific daemon (its `MONITOR` limits, nick-delay windows, casemapping) is not exercised.
- **Visible events in channels only.** `NICK` and `QUIT` are sent as server lines; the module does not look at channel membership, so the suite does not model whether ZNC actually shares a channel with the holder.
- **Single user.** Several networks are exercised, several users are not.
