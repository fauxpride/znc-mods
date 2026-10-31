# Testing `keepchanbuffersize`

Reference for the module's test suite: what it verifies, how to run it, and what it does not cover.

The suite drives a **real ZNC process** against fake IRC servers. The module is never mocked, stubbed, or compiled into a test harness; every assertion is made against a loaded `.so` running inside ZNC. The only exception is `sym_unique`, which inspects the built file.

Assertions are made on what ZNC itself reports and does: the `Buffer` column of `*status ListChans`, where a leading `*` marks an explicitly set size; the number of lines a client actually receives from buffer playback; the module's replies; and the NV registry file ZNC writes. A **control scenario** runs the same channel cycle without the module, which shows that the behaviour the suite checks is the module's doing and not ZNC's.

---

## Layout

Everything lives under [`tests/`](./tests/), self-contained within the module:

| File | Contents |
|---|---|
| `hx.py` | Harness. Fake IRC server, ZNC process manager, IRC client. A separate copy of the shared harness, extended with `PART`/`KICK` echo, connection drops and timed waits. |
| `suite.py` | Runner and all scenarios. |

---

## Requirements

- ZNC and `znc-buildmod`. The suite is exercised against ZNC 1.10.3 built with GCC 13.3 and with GCC 11.5.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user. ZNC, its module directory and `HX_ROOT` must be reachable by `znctest`.
- Python 3. No third-party packages.
- `nm` and `c++filt` from binutils, for `sym_unique` and for deciding what `up_inplace` should expect.
- Two module builds: the version under test, and a **probe**: the same source rebuilt with a different version string, so a successful in-place update is visible.

---

## Building the modules

```sh
mkdir -p /tmp/b/new /tmp/b/probe
cp src/keepchanbuffersize.cpp /tmp/b/new/ && (cd /tmp/b/new && znc-buildmod keepchanbuffersize.cpp)
sed 's/#define KEEPCHANBUFFERSIZE_VERSION "[^"]*"/#define KEEPCHANBUFFERSIZE_VERSION "9.9.9"/' src/keepchanbuffersize.cpp \
    > /tmp/b/probe/keepchanbuffersize.cpp && (cd /tmp/b/probe && znc-buildmod keepchanbuffersize.cpp)
```

Build both with the same compiler as the ZNC they will run in. `up_inplace` and `sym_unique` depend on it.

## Running

```sh
cd modules/keepchanbuffersize/tests

ZNC_BIN=/usr/local/bin/znc \
NEW=/tmp/b/new/keepchanbuffersize.so \
PROBE=/tmp/b/probe/keepchanbuffersize.so \
HX_ROOT=/tmp/kcb \
python3 suite.py
```

A full run takes about a minute.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `ZNC_BIN` | `/usr/bin/znc` | ZNC binary to run. |
| `NEW` | — | Module `.so` under test. Required. |
| `PROBE` | — | The build under test with another version string. Needed by `up_inplace`, which fails without it. |
| `HX_ROOT` | `/tmp/keepchanbuffersize-tests` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

The suite reads the build's version from the module description in the `.so`, so it knows what a `Version` reply should say without loading anything.

### Selecting scenarios

Pass scenario names, or a group prefix (`ctl`, `cy`, `fb`, `cmd`, `ps`, `ld`, `ty`, `up`, `sym`):

```sh
python3 suite.py cy                 # channel-cycle scenarios only
python3 suite.py fb_kick_then_part  # a single scenario
```

The process exits non-zero if any assertion fails. Each failure prints the scenario, the assertion, and the observed value.

---

## Sanitizer runs

Build the module with sanitizers and preload the runtimes into ZNC:

```sh
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g" \
LDFLAGS="-fsanitize=address,undefined" znc-buildmod keepchanbuffersize.cpp

ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
NEW=... PROBE=... python3 suite.py
```

After each scenario the suite scans that ZNC instance's log for sanitizer output and for `Segmentation`, and fails the run on a hit. A clean sanitizer run proves nothing unless the plumbing is known to work: verify it by injecting a deliberate fault into a throwaway copy of the source, such as an out-of-bounds read in a command handler, and confirming the suite reports it.

---

## Harness behaviour

**Fake IRC servers.** One per ZNC network. Each accepts ZNC's connection, completes registration (answering anything else sent before it with `451`), answers `PING`, and advertises `server-time`, `CHANTYPES=#` and `CASEMAPPING=rfc1459`. It echoes ZNC's `JOIN` with a `NAMES` reply and ZNC's `PART` for each channel it is in, and answers `JOIN 0` by parting every channel. A scenario can also make the server part ZNC on its own (as `SAPART` does), kick it, or send channel traffic from a chosen `nick!ident@host`. `sync()` returns once ZNC has processed everything sent before it.

**ZNC process manager.** Generates a `znc.conf` with one user and any number of networks, each pointing at its own fake server, with `LoadModule` lines at global, user or network level and extra settings for the global section, the user, a network or a channel. `ConnectDelay` and `ServerThrottle` are set to 0, and each network's `FloodRate` to -1: with flood protection on, ZNC queues lines once registration has used the burst, and a `PART`/`JOIN` would leave seconds later. The suite sets `MaxBufferSize = 300` and the user's `ChanBufferSize = 7`, so the default, an explicit size and the ceiling can never be confused. The manager seeds a module's NV registry before ZNC starts and reads it back afterwards (ZNC deletes the registry file once the last key is gone, which the suite reads as an empty store), installs `.so` files by atomic rename, never overwriting a mapped file in place, and supports graceful stop, `SIGKILL`, and restart against the same data directory. Each scenario gets its own data directory under `HX_ROOT`, left in place afterwards for inspection; `znc.out` in each holds that instance's debug log.

**Client.** Logs in to one network. Every request is followed by a `PING` to ZNC, and ZNC's `PONG` marks the point where all output caused by the request has arrived. A `PART` or `JOIN` waits for the server's echo (or the `366` end of `NAMES`) before continuing, so no assertion depends on a sleep. The lines received while attaching, which include buffer playback, are kept separately.

**Playback count.** To measure the size ZNC applies, independently of what `ListChans` reports, the suite detaches the client, has the server send 60 lines to the channel (500 for `cy_over_max`), reattaches, and counts the channel lines played back.

---

## Coverage

### `ctl_*` — the problem, without the module

`ctl_without_module` loads no module. An explicit size set with `SetBuffer` (42) and a size above `MaxBufferSize` from `znc.conf` (450) are both lost when the channel is parted and rejoined, because ZNC deletes the channel object on `PART` and creates a new one with the user default; playback then holds the default 7 lines. Every `cy_*` scenario turns this result around.

### `cy_*` — channel cycles

- **`PART`, then `JOIN`** (`cy_part_join`). The explicit size returns, playback holds 42 lines, the NV store holds exactly `buf:#a = 42`, and a channel without an explicit size stays at the default and gains no stored entry.
- **Fast cycle** (`cy_fast_cycle`). `PART` and `JOIN` in a single write, as a `/cycle` alias sends them, three times in a row.
- **Several targets** (`cy_multi_target`). `PART #a,#b,#c` and `JOIN #a,#b,#c`: each size returns to its own channel.
- **`JOIN 0`** (`cy_join0`). The server parts every channel; rejoining restores each size.
- **Above `MaxBufferSize`** (`cy_over_max`). A size of 450 from `znc.conf` is restored at 450 and playback holds 450 lines, because restore forces the size as the config loader does.
- **Case folding** (`cy_casefold`). `#Foo[x]` is stored as `buf:#foo{x}` and restored when rejoined as `#FOO{X}`.

### `fb_*` — fallback paths

- **Server-initiated `PART`** (`fb_server_part`). No client `PART`: the size is stored from the server's `PART` and restored on the next `JOIN`.
- **`KICK`** (`fb_kick_then_part`). ZNC keeps a kicked channel's object and size; the module stores the size. A client `PART` of the kicked channel makes ZNC remove it locally without asking the server, and a later `JOIN` restores the size.
- **Stored zero** (`fb_zero_value`). A stored `0`, possible only by editing the registry, is never applied, and the next `PART` of that channel deletes the entry.

### `cmd_*` — commands

- **Command surface** (`cmd_surface`). 27 commands, valid and invalid, compared reply by reply: `Status`, `List` (empty and as a table), `Version`, `Set` with missing arguments, a name without a channel prefix, a prefix the server does not advertise (`&`), `-1`, `+5`, `10abc`, `0`, a value one above `MaxBufferSize`, values that overflow 32 bits, leading zeros, and a name needing RFC 1459 folding; `Forget` in the same ways; and the removed `Enable` and `Disable`, which must answer `Unknown command!`. `Help` must list every command, and the NV store must match the sequence.
- **Immediate apply** (`cmd_set_applies`). `Set` on a joined channel applies at once (in `ListChans` and in playback) and survives a cycle.
- **Set before joining** (`cmd_set_then_join`). The value is applied when the channel is joined later.
- **`Forget` and a live explicit size** (`cmd_forget_stops_restore`). `Forget` clears the stored entry, but a channel that still has an explicit size is stored again at its next `PART`. This is recorded so the interaction stays visible.

### `ps_*` — persistence and stored-data migration

- **Restart** (`ps_restart`). A value stored with `Set` is applied on `JOIN` after a graceful restart and after a `SIGKILL`.
- **Legacy registry** (`ps_legacy`). A registry as written by 1.0.x/1.1.x: the `Enabled` key is deleted, ASCII-folded keys are rewritten with RFC 1459 folding, and where both forms exist the RFC 1459 one wins. The migrated values are applied.
- **Load arguments** (`ps_load_args`). `enabled=0` and `ENABLED=off foo=bar` print the deprecation notice exactly once; `foo=bar` and no argument print nothing. The module restores sizes in every case.

### `ld_*` — loading and unloading

- **Load while joined** (`ld_apply_existing`). `LoadMod` applies stored values to channels that already exist.
- **Load at startup** (`ld_startup_apply`). A module in `znc.conf` with a stored value: the channel comes up with it.
- **Unload** (`ld_unload_keeps`). While unloaded, a cycle loses the size, but the NV store is kept; `LoadMod` applies the kept value again.

### `ty_*` — module type and isolation

- **Runtime load** (`ty_runtime_load`). Plain `LoadMod` and `--type=network` give a network instance; `--type=user` and `--type=global` are refused.
- **Isolation** (`ty_isolation`). Two networks with the same channel name and different sizes: each network restores its own.

### `up_*` — `/znc updatemod`

`up_inplace` replaces the running build with the probe and updates it. `UpdateMod` must report `Done`, `Version` must report the probe's version, and a stored size must still be restored afterwards. If the build defines an `STB_GNU_UNIQUE` symbol that the `znc` binary lacks, glibc cannot unload it and the old code keeps running; the suite decides which outcome to expect per build with `nm` rather than assuming one.

### `sym_*` — symbol hygiene

`sym_unique` asserts that the build defines no `STB_GNU_UNIQUE` symbol the `znc` binary lacks. `ZNC_BIN` must point at the ZNC the module will be loaded into.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold: the exact `ListChans` tuple, the exact reply lines, the exact stored NV text, the exact number of lines played back.

Behaviour that is arguably wrong but current is asserted as it is and marked **characterised** in the scenario's description, so that a change shows up as a failure and is then a deliberate decision.

The suite was checked against mutated builds to confirm that it discriminates: restore without forcing (the bug fixed in 1.1.1), ASCII-only case folding, the server-`PART` hook disabled, the digits-only check on `Set` removed, and the `KICK` hook disabled. Each one failed the scenarios aimed at it.

---

## Known gaps

- **32-bit wrap in `Set`** (characterised in `cmd_surface`). `CString::ToUInt` truncates to 32 bits, so `Set #w 4294967297` stores `1` instead of rejecting the value, and `4294967296` is reported as zero.
- **`JOIN 0` and ZNC.** When a client sends `JOIN 0`, ZNC itself creates a channel object for the literal target, listed as `#0`. `cy_join0` records this as information and asserts only on the real channels.
- **Before `ISUPPORT`.** `Set` and `Forget` fall back to the RFC channel prefixes (`#&+!`) when the server has not yet advertised `CHANTYPES`. The fake server always advertises it before a client can send commands, so the fallback is not exercised.
- **Webadmin.** Explicit sizes are set with `*status SetBuffer`, the module's `Set`, and `znc.conf`. Webadmin is not loaded, so clearing an explicit size there (which would make the next `PART` delete the stored entry) is not exercised.
- **Detached channels.** Cycles are run on attached channels only.
- **Single user.** Several networks are exercised, several users are not.
- **Timing.** Nothing is measured. The fake servers feed lines serially, so any timing figure would describe the harness.
