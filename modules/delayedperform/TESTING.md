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
- Two module builds: the version under test, and a **probe**: the same source rebuilt with a different version string, so a successful in-place update is visible.

---

## Building the modules

```sh
mkdir -p /tmp/b/new /tmp/b/probe
cp src/delayedperform.cpp /tmp/b/new/ && (cd /tmp/b/new && znc-buildmod delayedperform.cpp)
sed 's/#define DELAYED_PERFORM_VERSION "[^"]*"/#define DELAYED_PERFORM_VERSION "9.9.9"/' src/delayedperform.cpp \
    > /tmp/b/probe/delayedperform.cpp && (cd /tmp/b/probe && znc-buildmod delayedperform.cpp)
```

Build both with the same compiler as the ZNC they will run in.

## Running

```sh
cd modules/delayedperform/tests

ZNC_BIN=/usr/local/bin/znc \
NEW=/tmp/b/new/delayedperform.so \
PROBE=/tmp/b/probe/delayedperform.so \
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
NEW=... PROBE=... python3 suite.py
```

After each scenario the suite scans that ZNC instance's log for sanitizer output and for `Segmentation`, and fails the run on a hit. The timer scenarios (`fire_reconnect_cancels`, `fire_unload_pending`) are the ones where a sanitizer run adds the most: they remove timers that have not yet fired. A clean sanitizer run proves nothing unless the plumbing is known to work: verify it by injecting a deliberate fault into a throwaway copy of the source and confirming the suite reports it.

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
- **Load while connected** (`fire_hot_load`, characterised). `LoadMod` into a live session schedules nothing; the stored entries run on the next connect.
- **Restart** (`fire_restart`). An entry added by command fires after a ZNC restart.
- **Isolation** (`fire_isolation`). Two networks with their own entries: each sends only its own.

### `sh_*` — slash shorthands

`sh_normalise` adds 43 commands and compares each `Added` echo, which shows the stored form, with the expected raw IRC line: `/msg` (including text that already starts with `:`), `/notice`, `/ctcp`, `/me`, `/join` with and without a key, `/part` with and without a reason, `/quit` with and without a message, `/nick`, `/topic`, `/mode`, `/kick` with and without a reason, `/invite`, `/whois` with one argument, two nicks, and a server name in either position, `/away`, `/oper`, `/raw`, `/quote`, the six service aliases, the generic fallback for unknown verbs (`/cap ls`, `/Who #a`), a raw line, and six shorthands missing a required argument. It then checks that the NV store holds exactly the normalised forms, and fires the harmless ones to confirm that the stored line is the line the server receives.

### `var_*` — `%nick%` expansion

- **Expansion** (`var_nick`). `%nick%` is replaced everywhere it appears, including inside `*%nick%*`; `%NICK%` and `%Nick%` are sent literally.
- **Alternate nick** (`var_nick_alt`). With `tim` held, ZNC registers as `tim_` and `%nick%` expands to `tim_`.
- **Nick outside IRC grammar** (`var_nick_invalid`). The server assigns `tim.x`. Entries containing `%nick%` are skipped with `Skipped (invalid nick for expansion): ...` (masked for a secret entry), and an entry without `%nick%` is sent.

### `sec_*` — control characters and the secret flag

- **Control characters** (`sec_control_chars`). Stored commands containing a lone CR, an LF, a NUL, or CR LF in a secret entry (possible only by editing the registry) are never sent; the clean entry is. The skip message is masked for the secret entry. Characterised: for a non-secret entry the message carries the stored line itself; see Known gaps.
- **Secret entries** (`sec_secret`). `AddSecret` masks the command in `Added`, `List` and `Ran:`, and the text appears in no module output at all. The server receives the command in full, and the NV value is the `s` flag followed by plain base64 of the command, as the README states: output masking, not encryption.

### `cmd_*` — commands and stored data

- **Command surface** (`cmd_surface`). 27 commands compared reply by reply: `Version`; the module's own `Help` text, line by line; `Help add`, which must give ZNC's filtered table; `List` empty and populated; `SetDelay` without an argument, with `x`, `-1` and `4294967297`, and valid; `Add` and `AddSecret` without a command; `Add` with and without a delay; `Add -3 ...`; `Del` without an argument, with `x`, `-1`, an index out of range, and a valid index (after which `List` must show the remaining entries renumbered); `Del 4294967296`; `Add 4294967296 ...`; and an unknown command. The NV store is checked before `Clear`, after `Clear` (only `global_delay` left), and `Del ALL` is accepted in any case.
- **Legacy and hand-edited stores** (`cmd_legacy_store`). A v1.0 `delay|base64` entry loads; an unknown flag (`zq`) is kept when the store is rewritten; an entry with an empty payload is listed as `[corrupt entry]` and is not scheduled; an entry with a non-numeric delay is not listed; a non-numeric `global_delay` reads as 0; keys other than `cmd.<n>` are ignored. Characterised: see Known gaps.

### `ty_*`, `up_*`, `sym_*`

- **Runtime load** (`ty_runtime_load`). Plain `LoadMod` gives a network instance; `--type=user` and `--type=global` are refused.
- **`/znc updatemod`** (`up_inplace`). The running build is replaced by the probe and updated: `UpdateMod` must report `Done`, `Version` must report the probe's version, and an entry added before the update must fire on the next connect. If the build defines an `STB_GNU_UNIQUE` symbol that the `znc` binary lacks, glibc cannot unload it and the old code keeps running; the suite decides which outcome to expect per build with `nm`.
- **Symbol hygiene** (`sym_unique`). The build defines no `STB_GNU_UNIQUE` symbol the `znc` binary lacks.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold: the exact lines the server received, the exact reply and timer lines, the exact stored NV text.

Behaviour that is arguably wrong but current is asserted as it is and marked **characterised** in the scenario's description, so that a change shows up as a failure and is then a deliberate decision.

The suite was checked against mutated builds to confirm that it discriminates: timer cleanup on disconnect removed, `Ran:` no longer masked for secret entries, the nick check before `%nick%` expansion removed, the send-time control-character check removed, the two-argument `/whois` swap fixed in 1.1.3 reintroduced, and flags dropped when the store is rewritten. Each one failed the scenarios aimed at it.

---

## Known gaps

The first five items are current module behaviour that the suite characterises rather than endorses.

- **32-bit wrap in numbers** (`cmd_surface`). `CString::ToUInt` truncates to 32 bits after the digits-only check, so `Del 4294967296` deletes entry 0 and `Add 4294967296 <cmd>` stores a delay of 0. `SetDelay 4294967297` sets a global delay of 1.
- **A delay that is not a plain number becomes part of the command** (`cmd_surface`). `Add -3 JOIN #neg` stores the command `-3 JOIN #neg` with the global delay.
- **Incomplete shorthands are stored as typed** (`sh_normalise`). A known shorthand missing a required argument (`/msg bob`, `/me waves`, `/ns`, `/join`, `/kick #c`, `/oper name`) is not rejected; the text is stored and later sent with its slash, which the server will reject.
- **The skip message carries the stored line** (`sec_control_chars`). For a non-secret entry, `Skipped (contains control chars):` prints the line itself. An LF splits the message into two module lines, which is harmless; a lone CR reaches the client inside the module line, and a client that treats a bare CR as a line end will see the remainder as a separate line. Such a value can only come from an edited registry.
- **Hand-edited stores with gaps** (`cmd_legacy_store`). `Add` reports an index one above the highest `cmd.<n>` key, while `List` and `Del` count positions, so with gaps the two disagree until the next `Del` rewrites the store contiguously. That rewrite also drops entries it cannot parse and any other `cmd.*` key.
- **`AddTimer` failure path.** When `OnIRCConnected` cannot schedule an entry it deletes the timer after `AddTimer` returns false, but ZNC's `CModule::AddTimer` has already deleted it, so taking that branch is a double free. The branch needs a `dp#<n>` timer to survive `CleanupTimers()`, which the suite could not bring about with the shipped code; a mutant that skips the cleanup reaches it and crashes ZNC.
- **Garbage base64.** ZNC's decoder turns invalid base64 into arbitrary bytes rather than an empty string, so only an empty payload is shown as `[corrupt entry]`; other corruption is listed, and sent or skipped, as whatever it decodes to.
- **`Add`-time control-character rejection.** `Add` rejects CR, LF and NUL, but an IRC client cannot deliver those bytes in a command, so that path is not exercised end to end; the send-time check is.
- **"Skipped (not connected)".** Timers are removed on disconnect, so a timer cannot fire while disconnected and this message is not exercised.
- **Flood protection.** Turned off in the harness. With it on, entries that fire together can be queued by ZNC and reach the server later than their delay.
- **Single user.** Several networks are exercised, several users are not.
