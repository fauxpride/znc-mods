# Testing `delayedperform`

Reference for the module's test suite: what it verifies, how to run it, and what it does not cover.

The suite drives a **real ZNC process** against fake IRC servers. The module is never mocked, stubbed, or compiled into a test harness; every assertion is made against a loaded `.so` running inside ZNC. The only exception is `sym_unique`, which inspects the built file.

Assertions are made on the **exact lines ZNC sends to the server** after it connects, with their arrival times relative to the server's `001`; on the module's replies and timer output as a client receives them; and on the NV store. The module only acts when ZNC connects, so to observe its connect-time output the suite attaches a client first and then has the fake server drop the connection: ZNC reconnects at once, with the client present.

---

## Layout

Everything lives under [`tests/`](./tests/), self-contained within the module:

| File | Contents |
|---|---|
| `hx.py` | Harness. Fake IRC server, ZNC process manager, IRC client. A separate copy of the shared harness, extended with connection drops, a configurable welcome nick, and a timestamped log of what ZNC sends. |
| `suite.py` | Runner and all scenarios. |

---

## Requirements

- ZNC and `znc-buildmod`. The suite is exercised against ZNC 1.10.3 built with GCC 13.3 and with GCC 11.5.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user. ZNC, its module directory and `HX_ROOT` must be reachable by `znctest`.
- Python 3. No third-party packages.
- `nm` and `c++filt` from binutils, for `sym_unique` and for deciding what `up_inplace` should expect.
- Three module builds: the version under test; a **probe**, the same source rebuilt with a different version string, so a successful in-place update is visible; and a **baseline**, the previous release, for the upgrade and downgrade checks in `up_from_baseline`.

---

## Building the modules

```sh
mkdir -p /tmp/b/base /tmp/b/new /tmp/b/probe
cp <previous release>/delayedperform.cpp /tmp/b/base/ && (cd /tmp/b/base && znc-buildmod delayedperform.cpp)
cp src/delayedperform.cpp /tmp/b/new/ && (cd /tmp/b/new && znc-buildmod delayedperform.cpp)
sed 's/#define DELAYED_PERFORM_VERSION "[^"]*"/#define DELAYED_PERFORM_VERSION "9.9.9"/' src/delayedperform.cpp \
    > /tmp/b/probe/delayedperform.cpp && (cd /tmp/b/probe && znc-buildmod delayedperform.cpp)
```

The previous release is the file at its git tag or commit, for example `git show <commit>:modules/delayedperform/src/delayedperform.cpp`. Build all three with the same compiler as the ZNC they will run in.

## Running

```sh
cd modules/delayedperform/tests

ZNC_BIN=/usr/local/bin/znc \
NEW=/tmp/b/new/delayedperform.so \
PROBE=/tmp/b/probe/delayedperform.so \
BASE=/tmp/b/base/delayedperform.so \
HX_ROOT=/tmp/dp \
python3 suite.py
```

A full run takes about a minute.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `ZNC_BIN` | `/usr/bin/znc` | ZNC binary to run. |
| `NEW` | — | Module `.so` under test. Required. |
| `PROBE` | — | The build under test with another version string. Needed by `up_inplace`, which fails without it. |
| `BASE` | — | The previous release. Needed by `up_from_baseline`, which fails without it. |
| `HX_ROOT` | `/tmp/delayedperform-tests` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

### Selecting scenarios

Pass scenario names, or a group prefix (`fire`, `sh`, `var`, `sec`, `cmd`, `ty`, `up`, `sym`):

```sh
python3 suite.py fire               # scheduling scenarios only
python3 suite.py sec_secret         # a single scenario
```

The process exits non-zero if any assertion fails. Each failure prints the scenario, the assertion, and the observed value.

---

## Sanitizer runs

Build the module with sanitizers and preload the runtimes into ZNC:

```sh
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g" \
LDFLAGS="-fsanitize=address,undefined" znc-buildmod delayedperform.cpp

ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
NEW=... PROBE=... BASE=... python3 suite.py
```

Build the baseline with sanitizers too, since `up_from_baseline` loads it into the same ZNC. After each scenario the suite scans that ZNC instance's log for sanitizer output and for `Segmentation`, and fails the run on a hit. The timer scenarios (`fire_reconnect_cancels`, `fire_unload_pending`) are the ones where a sanitizer run adds the most: they remove timers that have not yet fired. A clean sanitizer run proves nothing unless the plumbing is known to work: verify it by injecting a deliberate fault into a throwaway copy of the source and confirming the suite reports it.

---

## Harness behaviour

**Fake IRC servers.** One per ZNC network. Each accepts ZNC's connection, completes registration (answering anything else sent before it with `451`), answers `PING`, echoes `JOIN` and `PART`, and records every line ZNC sends with its arrival time. A scenario can make `001` assign a nick of its choosing (`tim.x` in `var_nick_invalid`), hand ZNC's preferred nick to another user so ZNC registers on its alternate, and drop the connection so ZNC reconnects. Times are reported relative to the moment the server sent `001`, which is stamped before `001` goes out.

**What counts as the module's output.** ZNC sends its own lines on every connect (`CAP`, `NICK`, `USER`, `WHO`, `JOIN #a`, and a `MODE` for each channel it joins). The suite leaves these out when it compares what ZNC sent with what the module should have sent. For the same reason `sh_normalise` does not fire a stored `MODE #a`, since it could not be told apart from ZNC's own.

**ZNC process manager.** Generates a `znc.conf` with one user (`Nick = tim`, `AltNick = tim_`) and any number of networks, each pointing at its own fake server. `ConnectDelay` and `ServerThrottle` are set to 0, so a reconnect happens at once, and each network's `FloodRate` to -1: with flood protection on, ZNC queues lines once registration has used the burst, and the timing assertions would measure the throttle rather than the module. It seeds the module's NV store before ZNC starts (entries are written as `<delay>|<flags>|<base64>`, exactly as the module writes them) and reads it back afterwards, installs `.so` files by atomic rename, and supports graceful stop, `SIGKILL`, and restart against the same data directory. Each scenario gets its own data directory under `HX_ROOT`; `znc.out` in each holds that instance's debug log.

**Client.** Logs in to one network. Every request is followed by a `PING` to ZNC, and ZNC's `PONG` marks the point where all output caused by the request has arrived. Timer output that arrives later is collected for a fixed period after the reconnect.

---

## Coverage

### `fire_*` — scheduling and sending

- **Order and timing** (`fire_order`). Four entries added by command, with delays 0, 1, 3 and 3, fire on the next connect: each exactly once, each at its own delay from `001` (within 0.6 s), the two 3-second entries together. The client sees `Scheduled 4 delayed command(s).` and one `Ran:` per entry, in firing order, and nothing repeats during a further three seconds.
- **Global delay** (`fire_global_delay`). `Add` without a delay uses the global delay in force when the entry is added; changing `SetDelay` later does not move existing entries.
- **Reconnect before firing** (`fire_reconnect_cancels`). The connection drops one second after connect, before either timer fires. After the reconnect each entry fires exactly once in total, timed from the second `001`, and the secret entry's `Ran:` is masked.
- **Unload with timers pending** (`fire_unload_pending`). `UnloadMod` before the timers fire: nothing is sent and ZNC keeps running.
- **Load while connected** (`fire_hot_load`). `LoadMod` into a live session schedules nothing, by design; the stored entries run on the next connect. `up_inplace` checks the reason for that design: an update must not send the perform again.
- **Restart** (`fire_restart`). An entry added by command fires after a ZNC restart.
- **Isolation** (`fire_isolation`). Two networks with their own entries: each sends only its own.

### `sh_*` — slash shorthands

`sh_normalise` adds 37 commands and compares each `Added` echo, which shows the stored form, with the expected raw IRC line: `/msg` (including text that already starts with `:`), `/notice`, `/ctcp`, `/me`, `/join` with and without a key, `/part` with and without a reason, `/quit` with and without a message, `/nick`, `/topic`, `/mode`, `/kick` with and without a reason, `/invite`, `/whois` with one argument, two nicks, and a server name in either position, `/away`, `/oper`, `/raw`, `/quote`, the six service aliases, the generic fallback for unknown verbs (`/cap ls`, `/Who #a`), and a raw line. It then adds 23 incomplete shorthands, each through both `Add` and `AddSecret`: every supported shorthand without its required arguments, in mixed case where that matters, and a `/` on its own. Each must be rejected with `<command>: incomplete shorthand. Usage: <usage>` and its exact usage line. Finally it checks that the NV store holds exactly the 37 normalised forms and nothing from the rejected commands, and fires the harmless ones to confirm that the stored line is the line the server receives.

### `var_*` — `%nick%` expansion

- **Expansion** (`var_nick`). `%nick%` is replaced everywhere it appears, including inside `*%nick%*`; `%NICK%` and `%Nick%` are sent literally.
- **Alternate nick** (`var_nick_alt`). With `tim` held, ZNC registers as `tim_` and `%nick%` expands to `tim_`.
- **Nick outside IRC grammar** (`var_nick_invalid`). The server assigns `tim.x`. Entries containing `%nick%` are skipped with `Skipped (invalid nick for expansion): ...` (masked for a secret entry), and an entry without `%nick%` is sent.

### `sec_*` — control characters and the secret flag

- **Control characters** (`sec_control_chars`). Stored commands containing a lone CR, an LF, a NUL, or CR LF in a secret entry (possible only by editing the registry) are never sent; the clean entry is. `List` and the skip messages show the bytes escaped as `\r`, `\n` and `\0`, one module line per entry, the secret entry masked, and no raw CR reaches the client at all.
- **Secret entries** (`sec_secret`). `AddSecret` masks the command in `Added`, `List` and `Ran:`, and the text appears in no module output at all. The server receives the command in full, and the NV value is the `s` flag followed by plain base64 of the command, as the README states: output masking, not encryption.

### `cmd_*` — commands and stored data

- **Command surface** (`cmd_surface`). 37 commands compared reply by reply: `Version`; the module's own `Help` text, line by line; `Help add`, which must give ZNC's filtered table; `List` empty and populated; `SetDelay` without an argument, with `x` and `-1`, with 4294967296 and 4294967297 (rejected), with 4294967295 (accepted), and valid; `Add` and `AddSecret` without a command; `Add` with and without a delay; delays of `-3`, `+5`, `1.5`, `.5`, 4294967296 and a 20-digit number (each rejected, nothing stored); a delay of 4294967295 and one with leading zeros (accepted); `Del` without an argument, with `x`, `-1`, an index out of range, 4294967296, and a valid index, after which `List` must show the remaining entries closed up; `Add` after a `Del`, which must report its `List` position; and an unknown command. The NV store must show that `Del` removed only its own keys and left the others exactly as stored; it is checked again after `Clear` (only `global_delay` left), and `Del ALL` is accepted in any case.
- **Legacy and hand-edited stores** (`cmd_legacy_store`). A v1.0 `delay|base64` entry loads; an unknown flag (`zq`) is kept; unpadded base64 is accepted. Entries with a non-numeric delay, an empty payload, or a payload that is not base64 are listed as `[corrupt entry]` (the delay column shows `-` when the delay is the problem), are not scheduled, and are counted in the connect message (`Skipped 3 unreadable entries; see List.`). Keys that are not `cmd.<n>`, or whose number exceeds 4294967295, are ignored rather than misread, and a non-numeric `global_delay` reads as 0. `Add` must report the `List` position; two `Del`s remove two corrupt entries; and the store afterwards must equal the seeded store minus exactly those two keys, plus the new entry.
- **Key exhaustion** (`cmd_key_exhausted`). With `cmd.4294967295` stored, `Add` refuses instead of wrapping to `cmd.0`; after that entry is deleted, `Add` stores under `cmd.0`.

### `ty_*`, `up_*`, `sym_*`

- **Runtime load** (`ty_runtime_load`). Plain `LoadMod` gives a network instance; `--type=user` and `--type=global` are refused.
- **`/znc updatemod`** (`up_inplace`). An entry runs at connect; the running build is then replaced by the probe and updated. `UpdateMod` must report `Done`, nothing must be sent to the server as a result of the update, `Version` must report the probe's version, and the entry must fire again on the next connect.
- **Previous release** (`up_from_baseline`). Upgrade: the baseline runs with three entries, one secret, and is updated to the build under test and restarted; `Version` reports the new version, `List` is identical, and the entries fire. Downgrade: the build under test stores three entries and deletes the middle one, leaving a gap in the keys; the baseline, started on that store, lists the two remaining entries in order and fires them. If the build defines an `STB_GNU_UNIQUE` symbol that the `znc` binary lacks, glibc cannot unload it and the old code keeps running; the suite decides which outcome to expect per build with `nm`.
- **Symbol hygiene** (`sym_unique`). The build defines no `STB_GNU_UNIQUE` symbol the `znc` binary lacks.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold: the exact lines the server received, the exact reply and timer lines, the exact stored NV text.

Behaviour that is arguably wrong but current is asserted as it is and marked **characterised** in the scenario's description, so that a change shows up as a failure and is then a deliberate decision.

The suite was checked against mutated builds to confirm that it discriminates. For 1.2.0: range checks removed so numbers wrap, output escaping removed, base64 validation removed, `Del` removing the wrong key, `Add` reporting the key instead of the position, incomplete shorthands stored, signed delays accepted, key allocation wrapping, and unreadable entries scheduled; and, carried over from 1.1.3, timer cleanup on disconnect removed, `Ran:` no longer masked for secret entries, the nick check before `%nick%` expansion removed, the send-time control-character check removed, and the two-argument `/whois` swap reintroduced. Each one failed the scenarios aimed at it. The suite as it stood for 1.1.3 was also run against 1.2.0: it failed only on the checks that characterised the behaviour 1.2.0 changes.

The fix for the `AddTimer` double free cannot be reached through the shipped code, so it was verified with a fault build: timer cleanup and the per-connection label removed, so that the second connect finds a timer with the same label and `AddTimer` refuses it. Under AddressSanitizer, the 1.1.3 code with that fault reports a heap-use-after-free at its `delete` and ZNC dies; the 1.2.0 code reports `Warning: could not schedule entry N` for each entry and keeps running with no sanitizer report.

---

## Known gaps

- **`AddTimer` failure path.** With per-connection labels, ZNC cannot refuse a timer through the shipped code, so the warning branch is not exercised by the suite. It was verified with a fault build; see Conventions.
- **`Add`-time control-character rejection.** `Add` rejects CR, LF and NUL, but an IRC client cannot deliver those bytes in a command, so that path is not exercised end to end; the send-time check and the output escaping are.
- **"Skipped (not connected)".** Timers are removed on disconnect, so a timer cannot fire while disconnected and this message is not exercised.
- **Flood protection.** Turned off in the harness. With it on, entries that fire together can be queued by ZNC and reach the server later than their delay.
- **Single user.** Several networks are exercised, several users are not.
