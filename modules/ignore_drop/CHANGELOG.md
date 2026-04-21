# Changelog

All notable changes to the `ignore_drop` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.0] — 2026-04-21

This release addresses four issues identified in a security review of 1.0.0: a trivial RFC 1459-based mask bypass, an IRCv3-tag blind spot in the fallback playback parser, a per-message heap allocation in the wildcard matcher, and a fragile on-disk storage format. Built and runtime-tested against ZNC 1.9.1.

### Added

* **RFC 1459 case folding** (`fold_char`, `fold`). Folds `A-Z` ⇄ `a-z` and the IRC-specific pairs `[] \ ~` ⇄ `{} | ^` before any comparison. Applied to masks at `Add` and `Load` time, and to the live `nick` / `nick!ident@host` sample on every incoming message and on every `Test` invocation. For ASCII-only workloads this is a no-op.
* **Allocation-free iterative wildcard matcher** (`wildmatch_folded`). Classic star-backtracking matcher that operates on pre-folded inputs. No `std::vector<std::vector<bool>>` DP table is allocated per match. Typical IRC patterns run at O(P + T); adversarial patterns fall back to O(P·T), but with a small constant factor and no heap traffic.
* **Input-side mask validation** (`ValidateMask`). `Add` now rejects masks that are empty, contain `\n` / `\r` / `\0`, or begin with the two bytes `A|` or `D|` (reserved for the on-disk scope prefix). On rejection the module prints `Rejected: <reason>.` and nothing is stored.
* **IRCv3 message-tag handling in the fallback playback parser.** `PlaybackMaybeDropFromLine` now skips a leading `@tag=value;...` block up to the first space before looking for the `:prefix`, so tagged playback lines no longer bypass filtering on the legacy `OnChanBufferPlayLine` / `OnPrivBufferPlayLine` code path.
* **`Version` command.** `/msg *ignore_drop Version` replies with `ignore_drop version <x.y.z>`. The version string is also embedded in `TModInfo`'s description and in the `USERMODULEDEFS` label, so it appears in `/msg *status ListMods` output.
* **Module-internal version constant.** `IGNORE_DROP_VERSION` macro and `kIgnoreDropVersion` constant for consistent reporting across the code.

### Changed

* **All case-folded comparisons route through `fold()` instead of the old `lc()`.** Stored masks, live samples, `Del <mask>` lookups, and `Test` inputs all pass through the same RFC 1459-aware normaliser, so the relationship between stored and incoming text is uniform everywhere in the module.
* **`LoadMasks` no longer silently coerces unknown scope prefixes to ALWAYS.** Entries whose first byte is neither `A` nor `D` (but whose second byte is `|`) are now skipped rather than absorbed as `always`, and a single `[ignore_drop] Skipped N malformed mask entry/entries during load.` line is emitted summarising the count. Entries with no prefix at all are still treated as `always` to preserve 1.0.0 backwards compatibility.
* **`Entry::mask_lc` now holds the RFC 1459-folded form.** The field name is kept for diff continuity; its meaning has been updated and a comment in the source documents the change.

### Fixed

* **Trivial mask bypass via `rfc1459` casemapping.** Previously, a mask like `[bot]*` would not match a sender appearing as `{bot}something`, even though most IRCds treat those nicks as the same identity. Both sides of the comparison now pass through the same fold, closing the bypass. Cross-checked against the 1.0.0 DP matcher on a 20,000-trial randomized corpus with 0 disagreements on ASCII inputs, plus explicit positive tests for each of the four RFC 1459 pairs.
* **IRCv3-tagged playback lines silently skipped the legacy filter.** Any buffered line starting with `@` was ignored by `PlaybackMaybeDropFromLine` and fell through to `CONTINUE`. The tag block is now consumed before prefix extraction.
* **Per-message heap allocation in the matcher.** The previous `std::vector<std::vector<bool>>` DP table was allocated afresh for every (entry, incoming message) pair. The new matcher performs the match in place with two index cursors.
* **NV load path was too lenient about storage corruption.** A byte sequence like `X|foo` would silently become an `always` entry for mask `foo`; now it is skipped with a visible warning, making corruption diagnosable rather than invisible.

### Security

* **Closes an `rfc1459` casemapping bypass affecting mask-based ignores.** For users whose targets have nicks containing `[ ] \ ~`, the 1.0.0 matcher could be defeated by the target re-registering with the equivalent `{ } | ^` form. Since 1.1.0 both sides of every mask comparison are folded through the same table, which is the same mapping most IRCds use for nick equivalence. Severity in the original audit: Medium.
* **Removes an injection vector into the on-disk storage format.** Although the command path never received LF / CR / NUL from a remote source under normal IRC operation (the protocol delimits on CR/LF), the `Add` path had no defence against such bytes arriving through a future API or a crafted command. These are now rejected at input time, as is the reserved `A|` / `D|` prefix that would be ambiguous on reload.
* **Eliminates a legacy-parser silent-pass on tagged playback.** Users on ZNC versions that dispatch to `OnChanBufferPlayLine` / `OnPrivBufferPlayLine` with IRCv3 tagging active would previously see tagged lines skip the module entirely. They are now filtered on the same decision path as untagged lines.

### Compatibility

* **Storage format is unchanged and fully backwards-compatible.** Entries written by 1.0.0 (`A|mask` / `D|mask` and legacy rows with no prefix) continue to load. Legacy entries are re-folded in memory on load; storage is re-normalised the next time `SaveMasks` runs (i.e. on the next `Add`, `Del`, `Clear`, or `SetScope`).
* **ZNC compatibility.** Built and tested against ZNC 1.9.1. Every module-facing header — `Modules.h`, `Nick.h`, `Message.h`, `IRCNetwork.h`, `User.h`, `Chan.h`, `znc.h` — is byte-identical between ZNC 1.9.0 and 1.9.1 in upstream, so a `.so` produced with `znc-buildmod` from either release is interchangeable.
* **Runtime behavior for existing users.** All pre-existing ignore rules continue to function. Rules against ASCII-only targets behave identically to 1.0.0. Rules against targets whose nicks include `[ ] \ ~` will now correctly match the `{ } | ^` counterparts as well — which is the point of the fix and is what the server already considered equivalent.
* **Command surface is unchanged except for one additive command** (`Version`). Existing scripts and workflows using `Add` / `Del` / `List` / `Clear` / `SetScope` / `SetDetachedPlayback` / `Test` continue to work with the same syntax and semantics.
* **Masks beginning with `A|` or `D|` can no longer be added.** This was never a useful configuration (the stored bytes would be ambiguous on reload in 1.0.0 as well), but the restriction is now explicit. Use a wildcard such as `?|*` if you need to match a nick starting with those bytes.

---

## [1.0.0] — Initial release

### Added

* Network-scoped ZNC module for server-side ignore filtering, dropping matching traffic before it reaches the client.
* Two scope semantics: `always` (live + playback, always) and `detached` (live only while the network is detached, with optional playback hiding).
* Sender-mask matching with both nick-only (`BadNick`) and full hostmask-style (`*!~bot@host.example`) forms, using `*` and `?` wildcards.
* Case-insensitive matching via ASCII lowercasing.
* Per-network persistence via ZNC NV storage (`masks`, `detached_playback`).
* Internal bucketing of masks by scope and nick-only/full-mask class, with the `always` path evaluated before attach-state lookup.
* Dynamic-programming wildcard matcher supporting `*` and `?`.
* Hooks covering channel and private messages, notices, and actions, plus modern and legacy buffer-playback hooks.
* `Add`, `Del`, `List`, `Clear`, `SetScope`, `SetDetachedPlayback`, and `Test` commands via `/msg *ignore_drop`.
* Built-in `Help` command.
