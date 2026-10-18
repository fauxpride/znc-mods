# Testing `ignore_drop`

Reference for the module's test suite: what it verifies, how to run it, and what it does not cover.

The suite drives a **real ZNC process** against fake IRC servers and asserts on what an IRC client connected to ZNC actually receives. The module is never mocked, stubbed, or compiled into a test harness; every assertion is made against a loaded `.so` running inside ZNC. The only exception is `sym_unique`, which inspects the built file.

Most scenarios are **differential**: identical traffic runs through a baseline build and the build under test, and the results must match. Behaviour that is meant to differ between the two, such as the module type fixed in 1.1.1, is asserted explicitly for each build instead.

---

## Layout

Everything lives under [`tests/`](./tests/), self-contained within the module:

| File | Contents |
|---|---|
| `hx.py` | Harness. Fake IRC server, ZNC process manager, IRC client. A separate copy of the `highlightctx` harness, extended for this module. |
| `suite.py` | Runner and all scenarios. |

---

## Requirements

- ZNC and `znc-buildmod`. The suite is exercised against ZNC 1.10.3 built with GCC 13.3 and with GCC 11.5.
- An unprivileged user named `znctest`. ZNC delays startup by 30 seconds when run as root, so the harness launches it through `setpriv`.
- Privileges sufficient to `chown` scratch directories to that user.
- Python 3. No third-party packages.
- `nm` and `c++filt` from binutils, for `sym_unique` and for deciding what `up_*` should expect.
- ZNC's bundled `keepnick` and `corecaps` modules, for `up_pinned_by_later_module`. They are part of every ZNC install.
- Three module builds: a baseline, the version under test, and a **probe**: the version under test rebuilt with a different version string, so a successful in-place update is visible.

---

## Building the modules

```sh
mkdir -p /tmp/b/base /tmp/b/new /tmp/b/probe
cp <baseline>/ignore_drop.cpp /tmp/b/base/ && (cd /tmp/b/base && znc-buildmod ignore_drop.cpp)
cp <new>/ignore_drop.cpp      /tmp/b/new/  && (cd /tmp/b/new  && znc-buildmod ignore_drop.cpp)
sed 's/#define IGNORE_DROP_VERSION "[^"]*"/#define IGNORE_DROP_VERSION "9.9.9"/' <new>/ignore_drop.cpp \
    > /tmp/b/probe/ignore_drop.cpp && (cd /tmp/b/probe && znc-buildmod ignore_drop.cpp)
```

Build all three with the same compiler as the ZNC they will run in. Several results depend on it; see `up_*` and `sym_unique` below.

## Running

```sh
cd modules/ignore_drop/tests

ZNC_BIN=/usr/local/bin/znc \
BASE=/tmp/b/base/ignore_drop.so \
NEW=/tmp/b/new/ignore_drop.so \
PROBE=/tmp/b/probe/ignore_drop.so \
HX_ROOT=/tmp/idx \
python3 suite.py
```

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `ZNC_BIN` | `/usr/bin/znc` | ZNC binary to run. |
| `BASE` | — | Baseline module `.so`. |
| `NEW` | — | Module `.so` under test. |
| `PROBE` | — | The build under test with another version string. Needed by `up_inplace` and `up_pinned_by_later_module`. |
| `HX_ROOT` | `/tmp/ignore_drop-tests` | Scratch root for per-scenario ZNC data directories. |
| `ZNC_PRELOAD` | unset | `LD_PRELOAD` value for sanitizer runs. |
| `ASAN_OPTIONS`, `UBSAN_OPTIONS` | unset | Passed through to the ZNC process. |

The suite reads each build's version from the module description in the `.so`, so it knows what a `Version` reply should say without loading anything.

### Selecting scenarios

Pass scenario names, or a group prefix (`lv`, `sc`, `cmd`, `ps`, `ty`, `up`, `sym`):

```sh
python3 suite.py ty                 # module-type scenarios only
python3 suite.py sc_playback        # a single scenario
```

The process exits non-zero if any assertion fails. Each failure prints the scenario, the assertion, and the observed value.

---

## Sanitizer runs

Build all three modules with sanitizers and preload the runtimes into ZNC:

```sh
CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g" \
LDFLAGS="-fsanitize=address,undefined" znc-buildmod ignore_drop.cpp

ZNC_PRELOAD="$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)" \
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
BASE=... NEW=... PROBE=... python3 suite.py
```

After each scenario the suite scans that ZNC instance's log for sanitizer output and fails the run on a hit. The one exception is a probe that is expected to crash ZNC (`cmd_index_edge` against a baseline that has the `SetScope 0` bug), whose log is not scanned.

A clean sanitizer run proves nothing unless the plumbing is known to work. Verify it by injecting a deliberate fault into a throwaway copy of the source, such as an out-of-bounds read in a command handler, and confirming the suite reports it.

---

## Harness behaviour

**Fake IRC servers.** One per ZNC network. Each accepts ZNC's connection, completes registration, answers `PING`, joins any channel ZNC requests, and advertises `server-time`. A scenario chooses the sender's full `nick!ident@host` for every line and can send channel or private `PRIVMSG`, `NOTICE`, CTCP `ACTION` and other CTCP requests, or raw lines such as `JOIN` and `PART`. `sync()` returns once ZNC has processed everything sent before it.

**ZNC process manager.** Generates a `znc.conf` with one user and any number of networks, each pointing at its own fake server, with `LoadModule` lines at global, user or network level. `ConnectDelay` and `ServerThrottle` are set to 0 so that several networks on `127.0.0.1` connect at once. It can seed a module's NV registry file (user-level or per network) before ZNC starts and read it back afterwards, installs module `.so` files by atomic rename, never overwriting a mapped file in place, and supports graceful stop, `SIGKILL`, and restart against the same data directory. A start that is expected to fail can be observed through its exit code. Each scenario gets its own data directory under `HX_ROOT`, left in place afterwards for inspection; `znc.out` in each holds that instance's debug log.

**Client.** Logs in to one network. Every request is followed by a `PING` to ZNC, and ZNC's `PONG` marks the point where all output caused by the request has arrived, so no assertion depends on a sleep. The lines received while attaching, which include buffer playback, are kept separately.

**Markers.** Scenario traffic carries a short marker such as `M1` or `A3` in each message. Assertions compare the ordered list of markers the client received, which makes a dropped or leaked message show up by name.

---

## Coverage

### `lv_*` — live filtering

With a client attached, always-scope rules only:

- **Message kinds** (`lv_kinds`). A nick mask drops channel and private `PRIVMSG`, `NOTICE` and `ACTION` from the sender while other senders pass. Other CTCP requests, `PART` and `JOIN` from the same sender are not filtered, because the module hooks messages only; this is recorded so that a change shows up.
- **Masks** (`lv_masks`). Nick-only masks look at the nick and never at ident or host; masks with `!` or `@` match the full `nick!ident@host`. `*` and `?` wildcards, anchoring at both ends, a literal `~` in an ident, and `Test` agreeing with live filtering for every sample.
- **Case folding** (`lv_casefold`). RFC 1459 folding on both sides: `A-Z`/`a-z`, `[`/`{`, `]`/`}`, `\`/`|`, `~`/`^`, in nicks and hosts, and `List` showing the folded masks.

### `sc_*` — scopes, attach state and playback

- **Detached scope** (`sc_detached_live`). Detached-scope rules let traffic through while a client is attached and drop it while none is; always-scope rules drop it in both states. Lines dropped while detached never enter the buffer, so they are absent from playback.
- **Playback** (`sc_playback`). `AutoClearChanBuffer` and `AutoClearQueryBuffer` are turned off so that lines are buffered while a client is attached, before any rule exists. A second client attaching then sees always-scope senders hidden from channel and query playback, detached-scope senders shown with `SetDetachedPlayback off` and hidden with `on`.
- **`SetScope`** (`sc_setscope`). Moving an entry between scopes changes live behaviour accordingly, and `List` shows the resulting order.
- **Several clients** (`sc_multi_client`). The network counts as attached while any client is connected.

### `cmd_*` — commands

- **Command surface** (`cmd_surface`). 41 commands, valid and invalid, covering every command and its error paths. Output is compared line by line with the baseline; only the version number may differ. `Version` must report the build's version, and `Help` must list every command.
- **Index edge cases** (`cmd_index_edge`). `SetScope 0` and `SetScope 4294967296` must be rejected with ZNC still running; `SetScope 4294967297` and `Del 4294967297`, which `CString::ToUInt` wraps to 1, must be rejected rather than act on entry #1; `Del 01` still works. Against a baseline older than 1.1.1, the suite also asserts that the first two crash ZNC and the last two act on entry #1, so the scenario is known to discriminate.

### `ps_*` — persistence

- **Restart** (`ps_restart`). The NV store holds folded masks with their `A|`/`D|` prefixes and the playback flag. Rules survive a graceful restart and a `SIGKILL`, and are active afterwards.
- **Legacy storage** (`ps_legacy`). An NV store written by older versions: unprefixed rows (1.0.0) load as always and are re-folded; rows with an unknown prefix, and empty ones, are skipped; loading does not rewrite the store; the next change rewrites it normalised.

### `ty_*` — module type

- **Runtime load** (`ty_runtime_load`). `LoadMod` without `--type` and with `--type=network` gives a working network instance; `--type=user` and `--type=global` are refused. Against a baseline older than 1.1.1, the suite asserts that plain `LoadMod` and `--type=user` produced a user instance.
- **User-level line in `znc.conf`** (`ty_conf_user_line`). Two networks, a user-level `LoadModule = ignore_drop`, user-level NV with one rule, and the second network with NV of its own. ZNC must start, log that it is loading a network module on every network, give the first network the user-level rules, leave the second network's own rules alone, and, after `SaveConfig`, record network-level lines only. The migrated config must restart cleanly. Against an older baseline, the suite asserts the single shared user instance instead.
- **Isolation** (`ty_isolation`). Two networks with their own instances: each one's rules apply to that network only.

### `up_*` — `/znc updatemod`

- **In place** (`up_inplace`). The build under test, replaced by the probe and updated, reports the probe's version, keeps its rules and filters with them, and is still a network module.
- **From the baseline** (`up_from_baseline`). Updating a running baseline. If the baseline defines an `STB_GNU_UNIQUE` symbol the `znc` binary lacks, glibc never unloads it, and `UpdateMod` reports `Done` while the baseline keeps running; a restart then loads the new build. Which outcome to expect is decided per build with `nm`, not assumed: 1.1.0 built with GCC 11 is pinned, built with GCC 13 it is not.
- **Pinned by a later module** (`up_pinned_by_later_module`). ZNC loads modules into the global symbol scope, and every module built against ZNC's headers defines the same weak template code, such as the `std::set` insertion behind `Info.AddType`. A module loaded later binds to the first copy loaded and thereby keeps that library from being unloaded. With `ignore_drop` followed by ZNC's own `keepnick` and no global module, `UpdateMod` reports `Done` and the old code keeps running. With `corecaps` loaded globally first, which provides the copy instead, the update takes effect. Unloading `keepnick`, updating, and loading it again also works.
- **User-scope instance** (`up_user_instance`). Only with a baseline older than 1.1.1. ZNC reloads each instance with its existing type, and the fixed build refuses the user type: an unpinned baseline instance is removed and ZNC reports that the module could not be reloaded everywhere; a pinned one keeps running as before. ZNC must survive either way.

### `sym_*` — symbol hygiene

`sym_unique` asserts that the build under test defines no `STB_GNU_UNIQUE` symbol the `znc` binary lacks, that `__to_chars_10_impl` in particular is not among its unique symbols, and that it is no worse than the baseline. `ZNC_BIN` must point at the ZNC the module will be loaded into. The property depends on the compiler: `std::to_string` reaches such a symbol on GCC 11 but not on GCC 13, so a GCC 13 run alone cannot show that the fix works.

---

## Conventions

Scenarios are plain functions that call `check(scenario, assertion, ok, detail)`. They must clean up their ZNC instances through `finish()`, which also performs the sanitizer scan.

Assertions state an expected value rather than a property that happens to hold: the exact ordered list of markers delivered, the exact reply lines, the exact stored NV text.

Expectations that depend on the baseline gate on its version (`BASE_TYPE_FIXED`) or on a property measured from the build (`pinned()`), never on an assumed version.

Before 1.1.1 was released, the scenarios were run against mutated builds to confirm they discriminate: the type reverted to `USERMODULEDEFS`, case folding disabled, playback filtering disabled, the detached-scope attach check inverted, and `std::to_string` restored in a GCC 11 build. Each one failed the scenarios aimed at it.

---

## Known gaps

- **Single user.** Several networks are exercised, several users are not.
- **IRCv3 beyond `server-time`.** `TAGMSG`, `message-tags`, `echo-message` and the like are not exercised. The module does not hook `TAGMSG`, so typing notifications and reactions from ignored senders reach clients that negotiated `message-tags`; the suite does not demonstrate this.
- **Fallback playback-line methods.** The module's `OnChanBufferPlayLine(CString&)` and `OnPrivBufferPlayLine(CString&)` do not match the signatures of ZNC's hooks, which also take the channel and client, so ZNC never calls them, in 1.9 or 1.10. Playback filtering runs entirely through `OnChanBufferPlayMessage` and `OnPrivBufferPlayMessage`, which `sc_playback` covers. The fallback methods and their IRCv3 tag skipping are not exercised.
- **Input the IRC protocol cannot carry.** `Add` rejects masks containing CR, LF or NUL, but a client command cannot deliver those bytes, so that path is not tested end to end.
- **`highlightctx` interaction.** Ordering between the two modules is tested in the [`highlightctx` suite](../highlightctx/TESTING.md), not here.
- **Timing.** Nothing is measured. The fake servers feed lines serially, so any timing figure would describe the harness.
