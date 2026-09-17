# Changelog

All notable changes to the `highlightctx` ZNC module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.11.1] — 2026-09-17

Fixes a long-standing performance defect in journal compaction.

Compaction was attempted whenever the journal exceeded a fixed 512 lines. That test compares against a threshold a rewrite may be unable to get below: once live state alone exceeded 512 lines, each compaction finished still over the limit, so the next appended line triggered another full rewrite. Every eligible channel line then cost a complete journal rewrite with two `fsync` calls.

The defect predates 0.11.0, but the `max_events` default of 100 introduced there allows a steady state above the threshold — a compacted default-sized event is about 10 journal lines — so it became reachable in ordinary use rather than only in extreme ones.

### Fixed

* **Compaction is now triggered relative to what the last compaction achieved**, rather than against a fixed constant. After each successful compaction the next trigger point becomes `max(kCompactThresholdLines, 2 x lines written)`, so the journal must at least double before being rewritten again. A compaction costing *L* line-writes is therefore always preceded by at least *L* appends, making total rewrite work proportional to the number of appends rather than to the size of the pending queue.

  Measured on a live instance at stock defaults with 70 pending events: 10 journal rewrites per 10 channel lines before, 0 after. Appending 160 events across roughly 1000 journal lines triggers a single compaction.

### Changed

* **The journal may sit at up to twice the size of live state** between compactions, instead of tracking it closely. With the default growth caps that peak is under 1 MB. Journals below the 512-line floor are unaffected and compact exactly as before.
* **Version marker bumped** from `highlightctx 0.11.0` to `highlightctx 0.11.1`.

### Security

* **No change to what is stored or for how long.** The fix affects only how often the journal file is rewritten. Retention is still governed by `max_events`, `max_event_age`, and replay.
* **Durability is unchanged.** Every append is still `fsync`ed along with its parent directory, and compaction still writes through a temporary file replaced by `rename()`. A crash between compactions recovers from a larger, less-compacted journal, which is covered by the test suite.
* **Reduced write amplification** is the practical benefit: far fewer full-file rewrites and `fsync` pairs on storage where that matters.

### Compatibility

* **No format, setting, or command changes.** The trigger point is internal derived state, not persisted anywhere.
* **Downgrading to 0.11.0** restores the previous rewrite behaviour; journals remain readable in both directions.
* **ZNC compatibility.** No new ZNC API is used. Built with `znc-buildmod` and run against ZNC 1.9.0 (`znc-dev 1.9.0-2build3`) and ZNC 1.9.1 built from the upstream `znc-1.9.1` tag.

---

## [0.11.0] — 2026-09-17

This release bounds how much state the module accumulates, and makes shedding visible.

Before it, three quantities were unbounded: the number of pending events awaiting replay (`max_events` existed but defaulted to disabled), the size of a single event (extension could grow one event indefinitely), and how long events were kept. Because the journal after compaction holds exactly the records needed to rebuild open and pending events, all three showed up directly as on-disk growth.

`max_events` now defaults to 100, a new `max_event_lines` caps a single event at 100 total lines by limiting extension, and an optional `max_event_age` expires old pending events. Dropped events are now reported instead of disappearing silently.

Verified by a live-ZNC test harness on ZNC 1.9.0 and ZNC 1.9.1, including a full run under AddressSanitizer and UndefinedBehaviorSanitizer; see [TESTING.md](./TESTING.md).

### Added

* **`max_event_lines=<count|0|off>` load argument and `SetMaxEventLines` command.** Caps the total captured lines in one event (`before` + trigger + `after`), default `100`. It limits [extension](./README.md#overlapping-highlights-extension) only: an event always collects the full window it started with, so no cap value can reduce configured context. A cap below `before + 1 + after` disables extension and says so. Evaluated live rather than captured per event, so it also governs events rebuilt from the journal.
* **`capped` field in the replay header** for events whose extension was suppressed, e.g. `(complete, before=8, after=24/24, triggers=4, capped)`. The suppressed trigger is still recorded, still marked `>>>`, and still counted in `triggers=`.
* **New `C` journal record** marking an event as capped, written after the `A`/`X` records it relates to and preserved by compaction. Older builds ignore unknown record types.
* **`max_event_age=<duration|off>` load argument and `SetMaxEventAge` command.** Drops pending events older than the given age without replaying them. Accepts `30d`, `12h`, `90m`, `3600s`, `2w`; a bare number is seconds. Default `off`. Expiry runs at load, as traffic arrives, and immediately before replay.
* **Drop reporting.** Events shed by `max_events` or `max_event_age` are counted and reported at the top of the next replay, naming the cause and the limit. Counters persist across restarts in NV, appear in `Status`, and reset once reported.
* **`Status` lines** for `max_event_lines`, `max_event_age`, and the drop counters.
* **`Overview` paragraph** covering the growth controls.

### Changed

* **`max_events` now defaults to `100`** instead of disabled. Existing installs that set it explicitly, including to `0`, keep their value; only installs that never set it pick up the new default. The value was chosen on replay volume rather than resources: 100 default-sized events is roughly 1700 lines delivered at attach, while costing about 0.7 MB of journal.
* **Dropping is no longer silent.** `max_events` previously discarded the oldest pending event with no indication at all.
* **`Reset` restores the new defaults** (`max_events=100`, `max_event_lines=100`, `max_event_age=disabled`) and names them in its reply. The journal setting is still deliberately left unchanged.
* **Load message** reports `max_event_lines` and `max_event_age` alongside the existing settings.
* **Version marker bumped** from `highlightctx 0.10.0` to `highlightctx 0.11.0`.
* **Module-header comment block** gains a "Growth controls (since 0.11.0)" section.

### Fixed

* **Journal recovery now reproduces a capped event's target exactly.** Recovery replays `X` records through the same cap-aware code path as live capture, rather than re-growing a target the cap had suppressed. A cap lowered between sessions is applied to recovered open events as well.
* No other bug fixes. With `max_events=off max_event_lines=off max_event_age=off`, capture, replay, and journal contents are unchanged from 0.10.0.

### Security

* **Bounded state is the point.** Worst-case journal size is now `(open_events + max_events) × max_event_lines × ~0.4 KB`, with `open_events` bounded by channel count, since at most one event per channel is open at a time. With the defaults that is roughly 4 MB, against unbounded before.
* **`max_event_age` is a retention control.** With journaling on, pending events are message text at rest; expiry puts a ceiling on how long it is kept. It defaults to off because discarding missed highlights by default would defeat the module's purpose.
* **Shedding is visible.** Silent data loss is now reported, so a cap cannot quietly discard highlights you were relying on.
* **No new untrusted input.** The `C` record is read from the module's own `0600` journal; its single numeric field is validated with full-string parsing and its event id is checked against already-loaded events. Duration parsing rejects unknown suffixes and overflow (capped at 100 years).

### Compatibility

* **Journal format stays backward and forward compatible.** The `C` record is additive, and 0.10.0 and earlier ignore unknown record types: a 0.11.0 journal loads there, losing only the capped flag.
* **The `max_events` default change alters behavior on upgrade** for installs that never set it. Anyone relying on unbounded retention must set `max_events=off` explicitly.
* **The new `max_events` default interacts with journal compaction.** A compacted default-sized event is about 10 journal lines, so the internal 512-line compaction threshold is crossed at roughly 51 pending events. Above it, every eligible channel line triggered a full journal rewrite — pre-existing behaviour, unchanged in this release, but reachable at the new default. Fixed in 0.11.1.
* **Downgrading to 0.10.0** restores unbounded growth: `max_event_lines`, `max_event_age_secs`, and the drop counters are unknown NV keys there and are ignored.
* **All other commands and load arguments are unchanged.**
* **ZNC compatibility.** No new ZNC API is used. Built with `znc-buildmod` and run against ZNC 1.9.0 (`znc-dev 1.9.0-2build3`) and ZNC 1.9.1 built from the upstream `znc-1.9.1` tag.

---

## [0.10.0] — 2026-09-17

This release adds a way to turn the durable journal off.

Until now, journaling was unconditional: every highlight event was written to `highlightctx.journal` under the module save path, and stayed there until compaction removed it after delivery. That is what lets active captures survive a crash, but it also means real message text — channel names, nicks, and full line content — persists on disk, where it can reach backups and VPS snapshots. There was no setting, command, or load argument to prevent it.

The new `journal=off` load argument makes the module memory-only: the journal is neither read at load nor written during operation. Capture, extension, exclusions, and replay behave exactly as before within a session. The trade-off is that open and pending events no longer survive an unload, restart, or crash.

### Added

* **`journal=<off|on>` load argument.** Default `on`, preserving existing behavior. Accepts the same spellings as the other boolean-ish settings (`on`/`off`, `1`/`0`, `yes`/`no`, `true`/`false`, `enable`/`disable`, `enabled`/`disabled`); any other value fails the load with `Invalid value for journal; use on or off.` The setting is stored in NV as `journal_enabled`, so a reload without arguments keeps it.
* **Leftover-journal reporting.** Loading with `journal=off` while a journal file from an earlier session still exists appends a note to the load message, and `Status` reports the file and its size. The file is left untouched: not read, not written, not deleted.
* **`Compact` removes a leftover journal file when journaling is disabled**, reporting the number of bytes deleted, and says so plainly when there is nothing to remove. This is the only path that deletes the file, so removal is always an explicit request.
* **`Status` line showing the active mode**, alongside the existing journal path line.
* **`Overview` paragraph** describing the opt-out and its trade-off.

### Changed

* **`Reset` no longer claims to reset every setting.** It now states which journal mode is in effect and that the mode was left unchanged. Resetting settings deliberately cannot re-enable writing highlight context to disk.
* **`ClearPending` reply wording** when journaling is disabled, to say that nothing was written to disk.
* **`Compact` help text** notes the disabled-mode behavior.
* **Load message** includes `journal=on|off`.
* **Version marker bumped** from `highlightctx 0.9.0` to `highlightctx 0.10.0`.
* **Module-header comment block** gains a "Journaling opt-out (since 0.10.0)" section.

### Fixed

* No bug fixes in this release. With the default `journal=on`, capture, replay, journal contents, and command output are unchanged from 0.9.0, apart from the `Status`, `Reset`, `Compact`, and `Overview` text noted above.

### Security

* **This is the point of the release.** With `journal=off`, captured highlight context never reaches persistent storage, which removes it from filesystem backups, VPS snapshots, and offline disk access. The journal file is `0600` under a `0700` directory either way, so this addresses data-at-rest exposure, not local access control.
* **Disabling is not destructive.** Turning journaling off never deletes existing data on its own; an old journal is ignored until you remove it with `Compact`. This avoids silent data loss for anyone flipping the setting to try it.
* **The opt-out cannot be undone accidentally.** `Reset` leaves it alone, and re-enabling requires an explicit `journal=on` load argument.
* **No new parsing of untrusted input.** The new value is a load argument supplied by the ZNC admin. Journal record parsing is unchanged.
* **The durability guarantee is genuinely given up.** With journaling off, an unclean shutdown loses open and pending events. That is the intended trade and is stated in the load message, `Status`, `Overview`, and the README.

### Compatibility

* **Default behavior is unchanged.** Without `journal=on|off`, existing installs keep journaling exactly as in 0.9.0, including the NV keys and journal format.
* **The journal format is untouched.** A journal written while enabled remains valid; re-enabling loads it normally unless `Compact` removed it while disabled.
* **Downgrade to 0.9.0 resumes journaling**, because the `journal_enabled` NV key is unknown to it. Anyone relying on the opt-out should not downgrade.
* **All other commands and load arguments are unchanged.**
* **ZNC compatibility.** No new ZNC API is used. `stat()` and `unlink()` are used through the existing `<sys/stat.h>` and `<unistd.h>` includes. Built with `znc-buildmod` and run against ZNC 1.9.0 (`znc-dev 1.9.0-2build3`) and ZNC 1.9.1 built from the upstream `znc-1.9.1` tag.

---

## [0.9.0] — 2026-09-11

This release changes how overlapping highlights are captured.

Until 0.8.0, every qualifying highlight started its own event, even when it arrived while an earlier event in the same channel was still collecting its trailing `after` lines. Both events then replayed every line inside both windows. With the defaults (`before=8 after=8`), a second highlight four lines after the first produced two events sharing 13 lines, and a burst of highlights produced one overlapping event per highlight.

A highlight that lands inside an open event's `after` window now **extends** that event instead. The line is marked as an additional trigger, and the event keeps collecting until a full `after` window has followed the latest trigger. The same input now yields one event with no duplicated lines, and the on-disk journal stays backward compatible with 0.8.0 in both directions.

Verified by a live-ZNC test harness on ZNC 1.9.0 and ZNC 1.9.1, including a full run under AddressSanitizer and UndefinedBehaviorSanitizer.

### Added

* **Extension of open events by later highlights.** When a line qualifies as a trigger and the channel already has an open event, that event is extended instead of a new one being started. The trigger line is recorded in the event's `after` list and marked as an additional trigger. The event's trailing-line target becomes that line's position in `after` plus the event's `after` cap. Repeated highlights keep extending the same event. See the README's *Overlapping highlights (extension)* section for the full rules and a worked example.
* **New `X` journal record** (`X <event id> <after index>`), written immediately after the `A` record it refers to. It records that the given `after` line extended the event. The extended target is not stored; on load it is recomputed as index + 1 + the event's `after` cap from its `B` record. Compaction writes `X` records back in the same position, so extensions survive journal compaction, `/znc restart`, and crash recovery.
* **`triggers=<n>` field in the replay header** for events that were extended, e.g. `(complete, before=8, after=12/12, triggers=2)`.
* **`>>>` marker on additional triggers** inside the replayed `after` lines, in both native server-time replay and inline-timestamp fallback replay.
* **`Overview` paragraph** describing overlapping-highlight handling.

### Changed

* **Trigger classification now happens before open events are fed.** `HandleIncoming` decides whether a line is a trigger (highlights your current nick, not from your own nick, sender not nick/mask-excluded) *before* calling `FeedOpenEvents`. This lets a trigger that arrives as the last line of an open event's window extend that event instead of letting it complete and starting an overlapping one. For lines that are not triggers, the processing order and results are unchanged.
* **Completion check uses the event's target instead of its cap.** An event finalizes as complete once `after.size() >= after_target`. `after_target` starts equal to `after_cap` and only grows through extension, so unextended events complete exactly as before.
* **Replay header `after=<collected>/<target>`** now shows the event's target. For unextended events the target equals the `after` cap, so their header text is byte-identical to 0.8.0. The `triggers=` field is appended only for extended events.
* **`SetAfter` help text** notes that an extension adds a fresh window of the cap the event started with. As before, `SetAfter` affects only new events; extensions of an already-open event also use that event's original cap.
* **Version marker bumped** from `highlightctx 0.8.0` to `highlightctx 0.9.0`.
* **Module-header comment block** gains an "Overlapping highlights (extension semantics, since 0.9.0)" section.

### Fixed

* No bug fixes in this release. For traffic without overlapping highlights, capture, replay, and journal output are unchanged; this was verified by byte-for-byte comparison against 0.8.0 (see *Testing*).

### Security

* **No new externally reachable input.** The only new parser input is the `X` journal record, read from the module's own `0600` journal file. Both numeric fields must be non-empty and parse with `strtoull` to the end of the field. The index is then bounds-checked against the `after` lines already loaded for that event before it is used, which also rejects values such as `-1` that `strtoull` wraps to a huge number. Records for unknown events, out-of-range indices, and malformed or extra-field records are ignored. The extended target is computed in `size_t` from a 32-bit cap, so it cannot overflow on 64-bit builds.
* **Lower resource cost under highlight spam.** Someone repeating your nick while you are detached previously created one event per message. Each later channel line was then fed into, and journaled (with `fsync`) for, every overlapping open event. Now the burst extends a single event, so each line is appended once, plus one small `X` record per extending highlight. In the live spam test, 30 consecutive highlights produced 300 journal appends and 510 replay lines on 0.8.0, versus 68 journal appends and 46 replay lines on 0.9.0.
* **Extension is unbounded by design.** An event keeps growing for as long as highlights keep arriving inside its window. For any given traffic, the lines it stores and replays never exceed what 0.8.0 stored across its overlapping events.
* **Exclusions and `ignore_drop` keep their protective meaning.** Lines from nick/mask-excluded senders cannot extend an event, just as they cannot start one. Lines dropped by [`ignore_drop`](../ignore_drop/README.md) never reach `highlightctx`.

### Compatibility

* **Upgrade from 0.8.0 (and earlier journal formats) requires no migration.** Existing `B`/`A`/`F`/`D` records load unchanged, and NV settings keys are unchanged.
* **A 0.8.0 journal may contain several overlapping open events on the same channel.** After upgrading, later triggers extend only the newest of them; the older ones finish their original windows without being extended. The channel converges back to a single open event.
* **Downgrade to 0.8.0 is safe but lossy for extensions.** 0.8.0 ignores unknown record types, so it loads a 0.9.0 journal without error. Extension marks and extended targets are lost, and open events finish at their original cap.
* **Replay output for events with a single trigger is unchanged.** Anything that parses the replay header should tolerate the optional `, triggers=<n>` field before the closing parenthesis on extended events.
* **Behavior change for overlapping highlights.** Where 0.8.0 replayed several overlapping events, 0.9.0 replays one extended event. No setting restores the old behavior.
* **All commands and load arguments are unchanged.** The only command-surface differences are the `SetAfter` help description and the added `Overview` paragraph.
* **ZNC compatibility.** No new ZNC API is used. Built with `znc-buildmod` and run against ZNC 1.9.0 (`znc-dev 1.9.0-2build3`) and ZNC 1.9.1 built from the upstream `znc-1.9.1` tag.

---

## [0.8.0] — 2026-04-23

This release extends the exclusion system to cover nicknames and `nick!ident@host` masks in addition to channels. The main operational need: suppressing a specific noisy user as a trigger source, without losing their surrounding messages as context around real triggers from other users. This is the key semantic difference from a channel exclusion (which drops the channel entirely): a nick/mask exclusion only prevents that sender from *starting* a new event; their messages still appear in before/after context for other users' triggers, which matches how an operator usually thinks about "this person spams my highlights but I still want to see what they said around a legitimate mention." Syntax and case-folding rules mirror [`ignore_drop`](../ignore_drop/README.md) so both modules interpret masks consistently, and the feature is verified by an automated test harness (83/83 passing against ZNC 1.9.0).

### Added

* **Nick and hostmask exclusions via `AddExclude` / `DelExclude` / `ListExcludes`.** The existing commands now accept three kinds of tokens: channel names (starting with `#`, `&`, `+`, or `!`), bare nicknames, and full `nick!ident@host` masks. Classification is automatic: a token starting with a channel-prefix character is treated as a channel; a token containing `!` or `@` is treated as a full mask; anything else is treated as a nick-only mask.
* **`*` and `?` wildcards in nick/mask exclusions.** Implemented via an allocation-free iterative star-backtracking matcher (`wildmatch_folded`). Patterns like `bot*`, `?roll`, or `*!*@evil.example` work as expected.
* **RFC 1459 case folding for nick/mask exclusions.** Matches the folding used by [`ignore_drop`](../ignore_drop/README.md), so `[bot]`, `{bot}`, `[BOT]`, and `{BOT}` all fold to the same canonical form and cannot be used to defeat each other. ASCII nicks are unaffected (it is a strict superset of ASCII case folding).
* **New NV storage key `excluded_nicks`.** Stored as newline-separated folded masks. Separate from the existing `excluded_channels` key so channel exclusions keep their on-disk format unchanged.
* **`excluded_nicks` load-arg support on the existing `excludes=` list.** Mixed channels and nicks are allowed, e.g. `excludes=#noise,#bots,BadNick,*!*@evil.example`. Each token is classified the same way as interactive `AddExclude`.
* **`excluded nicks/masks: N` line in the `Status` output** next to the existing `excluded channels: N`.
* **Duplicate detection** for nick/mask exclusions (compared on folded form). Duplicates are reported rather than silently added.
* **Mask validation at `AddExclude` time.** Empty masks are rejected, and masks containing embedded `\r`, `\n`, or `\0` bytes are rejected (protects the newline-delimited NV storage format). Rejection reason is included in the response.
* **`DelExclude <index>` for numeric removal.** The index refers to the unified numbered listing from `ListExcludes` — channels first (alphabetically sorted), then nicks/masks in insertion order. Removes the corresponding entry from whichever bucket it belongs to.
* **New test scenario `nick_exclude` in the test harness** with 28 assertions covering command surface, RFC 1459 folding (including the `[bot]` vs `{bot}` bypass case), duplicate detection, unified listing/indexing, the "excluded nick still appears as context but cannot trigger" behavior, and persistence across `UpdateMod`.

### Changed

* **`AddExclude` help and argument description** now read `<#channel|nick|mask>` and explain the classification and semantic difference. Similarly for `DelExclude` (`<#channel|nick|mask|index>`) and `ListExcludes`.
* **`ListExcludes` output format** is now a single numbered list with each entry tagged `[channel]`, `[nick]`, or `[mask]`. The empty-state message changed from `"No excluded channels configured."` to `"No exclusions configured."` to reflect the broader scope.
* **`OnLoad` summary message** reports exclusion counts as `excludes=<C>ch/<N>nick` instead of a single number.
* **`CmdReset` clears both channel and nick/mask exclusions**, and the reset-confirmation message explicitly notes "all exclusions (channel and nick/mask) cleared."
* **`Overview` command output** includes a paragraph describing the exclusion semantics (channels drop entirely, nick/masks only block triggering), the RFC 1459 folding, and the wildcard syntax.
* **`TModInfo` description string** now mentions "channel and nick/hostmask exclusions."
* **Version marker bumped** from `highlightctx 0.7.0` to `highlightctx 0.8.0`.
* **Module-header comment block** gains a new "Exclusion semantics" section explicitly contrasting channel exclusions from nick/mask exclusions.

### Fixed

* No bug fixes in this release; functionality previously shipped continues to work as before (verified by 55/55 regression assertions from the 0.7.0 suite still passing).

### Security

* **No new attack surface.** The new matcher is allocation-free and operates on RFC 1459-folded strings of bounded length (folded output length equals input length). Mask validation at add-time rejects control characters that could corrupt the NV storage line format. Wildcard matching degenerates to O(P·T) on adversarial patterns like `*a*a*a...X`, but the constant factor is small, no heap is allocated, and masks are authenticated operator input, not attacker-controlled.
* **The new feature is an exclusion, not an inclusion.** It only narrows what triggers an event; it does not expand capture scope. Excluded-nick messages are still preserved in context (the intentional semantic difference vs channel exclusion) but that is the same data that would have been captured anyway had the excluded nick not been listed.

### Compatibility

* **Storage format for channel exclusions is unchanged.** The `excluded_channels` NV key uses the same representation as 0.7.0 / 0.6.0. 0.7.0 and 0.6.0 datadirs load cleanly into 0.8.0 with no migration.
* **Journal format (`B`/`A`/`F`/`D` records) unchanged.** The verified 0.6.0 → 0.7.0 upgrade path from the test harness continues to work; a 0.6.0 journal replays correctly under 0.8.0 as well.
* **All 0.7.0 commands continue to work** with unchanged arguments and behavior. The three affected commands (`AddExclude`, `DelExclude`, `ListExcludes`) are supersets: all 0.7.0 usage (channel names as tokens) produces identical results.
* **Load-arg syntax** is a superset of 0.7.0's. `excludes=#chan1,#chan2` still works; `excludes=#chan1,BadNick,*!*@evil.example` also works.
* **ZNC compatibility.** Built and run against ZNC 1.9.0 with `znc-dev 1.9.0-2build3` in the development container. No new ZNC API is used; the feature is implemented entirely in the existing `OnChan*` hook paths and standard `CNick` accessors. Expected to build unchanged against ZNC 1.9.1 as with 0.7.0.

### Testing

Full automated test run on ZNC 1.9.0:

- `smoke` — 8/8 (now includes the `excluded nicks/masks` status key check and a `0.8.0` version-marker check)
- `load_order_both` — 5/5 (unchanged)
- `load_order_bad` — 5/5 (unchanged)
- `commands` — 11/11 (unchanged)
- `rearm_after_load` — 5/5 (unchanged)
- `unload_ignore` — 7/7 (unchanged)
- `replay` — 8/8 (unchanged)
- `upgrade` — 6/6 (unchanged; 0.6.0-format journal replays correctly under 0.8.0)
- `nick_exclude` — 28/28 (new)

Total: **83/83 passing**. Key new assertions include:

- excluded nick cannot start an event, even when they mention the target nick directly
- excluded nick's messages DO appear as before/after context around a triggering event started by a non-excluded user
- excluded nick's message is never marked with the `>>>` trigger prefix
- RFC 1459 folding correctly dedupes `[bot]` vs `{bot}`
- bad masks (empty, CR/LF/NUL embedded) are rejected; good masks are accepted
- channels, nick exclusions, and hostmask exclusions coexist and each is tagged correctly in `ListExcludes`
- `DelExclude` by mask and by numeric index both work
- `Reset` clears both kinds of exclusions
- nick exclusions persist across `UpdateMod highlightctx` via the new `excluded_nicks` NV key

### Known limitations (documented post-release)

The following section was added to the documentation after 0.8.0 was released. No code changed; this is documentation of pre-existing behavior that an operator can usefully be aware of.

* **Highlights at the moment of disconnect.** A highlight that arrives during the brief window between your client's socket closing and ZNC processing the close may not produce a `highlightctx` event. ZNC's `IsUserAttached()` still returns `true` during that window because the client object has not yet been removed from the network's client list, so `ShouldCaptureNow()` treats the message as having arrived while you were attached and skips capture. This is a property of how ZNC processes events, not a `highlightctx` bug — the same race exists for any module that gates on `IsUserAttached()`. The information itself is not lost: the highlight is still stored in ZNC's normal channel buffer and is replayed there on next attach. Only the focused `*highlightctx` replay event is missing. The window is short for clean disconnects (typically milliseconds to a few hundred milliseconds) and can be substantially longer for half-open TCP connections — process hard-kill, sudden network drop, or any case where TCP keepalives have not yet timed out. See the README's "Known limitation: highlights at the moment of disconnect" section in *Operational notes and caveats* for a fuller explanation.

---

## [0.7.0] — 2026-04-21

This release fixes a long-standing usability footgun where `require_ignore_drop=auto` could stay unarmed after `/znc restart` even when `ignore_drop` was loaded, because the original arming logic only looked at `ignore_drop`'s presence at `OnLoad` time. If the ZNC config happened to load `highlightctx` first (e.g. alphabetical ordering), the armed flag was locked to `false` for the rest of the process's lifetime. The primary fix is a new `OnBoot` hook that re-checks `ignore_drop`'s actual position in the module list after all `znc.conf` modules have loaded, plus a manual `Rearm` command for runtime re-checks without reloading the module. Built against ZNC 1.9.0 headers in the development container and verified end-to-end through an automated test harness with fifty-five assertions covering module load, command surface, load-order scenarios, detached capture with replay, and 0.6.0→0.7.0 upgrade of on-disk state.

### Added

* **`OnBoot()` override.** Fires after all `znc.conf` modules have been loaded. At this point the module inspects actual module-list positions to determine whether `ignore_drop`'s hooks will dispatch before ours. This is the primary fix for the `/znc restart` case: `OnLoad`'s `HasIgnoreDropLoaded()` can return `false` even when `ignore_drop` is about to load right after us; `OnBoot` re-checks once everything is in the list. For dynamically loaded modules not present in `znc.conf`, `OnBoot` is not called by ZNC; the `Rearm` command below covers that case.
* **`Rearm` command.** Manual re-check of `ignore_drop` presence and hook-order position without reloading the module. Reports a clear diagnosis — not armed because not loaded, not armed because positioned at or after us in the module list, or armed because positioned ahead — and flags arm/disarm transitions relative to the previous state. Outside of `auto` mode, `Rearm` updates the diagnostic state but does not change behavior.
* **`IsIgnoreDropAheadOfUs()` helper.** Iterates the network's module list once and returns `true` only when `ignore_drop` is present AND positioned at a lower index than `highlightctx`, so its hooks dispatch before ours.
* **`RecheckIgnoreDropHookOrder()` helper.** Single re-check routine shared by `OnBoot` and `Rearm`.

### Changed

* **Semantics of the auto-mode armed flag.** The internal arming heuristic was previously "was `ignore_drop` loaded at my `OnLoad` time" (captured as `m_ignore_drop_present_on_module_load`). The updated semantic is "was `ignore_drop` found ahead of us in hook order at last check," where "last check" is one of `OnLoad`, `OnBoot`, or a `Rearm` invocation. The variable name is preserved for minimal diff; the meaning is now the stricter, position-aware version.
* **Status output label updated.** `ignore_drop was present when highlightctx loaded: yes/no` is replaced with `ignore_drop ahead of highlightctx in hook order: yes/no` to reflect the new semantic. Other `Status` lines are unchanged.
* **Status/setmode warning text updated.** The warning shown when auto mode is unarmed but `ignore_drop` is loaded now correctly attributes the cause to "positioned at or after highlightctx in the module list" rather than "was not present when highlightctx loaded," and directs the user to `Rearm` and the module-list reordering workarounds.
* **`SetRequireIgnoreDrop` help text updated** to reference the new arming semantics and mention `Rearm`.
* **`Overview` help text updated** to describe the actual re-evaluation points (`OnLoad`, `OnBoot`, `Rearm`), to note that runtime load/unload of `ignore_drop` does not auto-refresh the armed flag, and to explain the independent runtime safety net.
* **Primary-commands summary line** in `Overview` now includes `Rearm`.
* **Version marker bumped** from `highlightctx 0.6.0` to `highlightctx 0.7.0`. Stored in the `version_marker` NV entry on successful load.
* **Header comment block updated** with the new arming semantics, the complete list of re-evaluation points, and the ZNC 1.9.x dispatch limitation on module-lifecycle hooks.

### Fixed

* **Auto mode staying unarmed after `/znc restart` despite `ignore_drop` being loaded.** When the ZNC config load order placed `highlightctx` ahead of `ignore_drop` (the common alphabetical case, since `h` precedes `i`), `OnLoad`'s `HasIgnoreDropLoaded()` returned `false`, `m_ignore_drop_present_on_module_load` was set to `false`, and no subsequent event re-evaluated it. The new `OnBoot` hook re-checks the actual module-list positions after all `znc.conf` modules have loaded; the `Rearm` command provides a manual fallback for dynamically loaded configurations. Note: neither `OnBoot` nor `Rearm` can arm auto mode if `highlightctx` is still positioned ahead of `ignore_drop` in the module list; the user must also reorder `znc.conf` or reload the module to move `highlightctx` to the tail of the list. This is now clearly surfaced by `Status` and `Rearm`.
* **Stale "reload highlightctx after ignore_drop" guidance.** Previous wording implied that simply loading `ignore_drop` later and then reloading `highlightctx` would arm auto mode. That guidance was incomplete — it only worked because it happened to re-run `OnLoad` after `ignore_drop` was already present. The updated guidance now makes the actual requirement explicit: `ignore_drop` must sit ahead of `highlightctx` in the module list so its hooks dispatch first.

### Security

* **No new attack surface.** The additions are purely diagnostic and state-tracking. They do not read untrusted data, do not touch the journal, and emit only module-output messages whose content is derived from module names already known to the module loader. The capture/replay hot path is unchanged.
* **Capture correctness is still runtime-checked.** `ShouldCaptureNow()` calls `HasIgnoreDropLoaded()` live on every incoming message, so capture fails closed the moment `ignore_drop` is unloaded, independent of the armed flag. The armed flag is a setup-time guarantee about hook order; the runtime presence check is the safety net.
* **Armed state is deliberately sticky.** Once `auto` is armed, unloading `ignore_drop` at runtime does not disarm automatically — the strict requirement stays effective so capture pauses (rather than silently resuming without the protection the user asked for). Explicit disarm is available via `Rearm` or by changing the mode with `SetRequireIgnoreDrop`.

### Compatibility

* **Storage format unchanged.** NV entries (`before_max`, `after_max`, `max_events`, `require_ignore_drop_mode`, legacy-compat `require_ignore_drop`, `excluded_channels`, `version_marker`) use the same on-disk representation as 0.6.0. Journal format (`B`/`A`/`F`/`D` records, double-hex encoded fields) unchanged. 0.6.0 journals load cleanly into 0.7.0 with no migration step — verified by the test harness, which seeds a 0.6.0-format journal and confirms the event replays on next attach with the correct channel, nick, trigger marker, and compaction post-replay.
* **ZNC compatibility.** Built against ZNC 1.9.0 headers (`znc-dev 1.9.0-2build3`) in the development container and run end-to-end against the ZNC 1.9.0 binary. The hook signatures used (`OnBoot`, plus the pre-existing capture hooks) have been stable across ZNC 1.9.x, so this is expected to build and run unchanged against ZNC 1.9.1. A local `znc-buildmod` against 1.9.1 on the target host is still recommended before production deployment.
* **User-visible commands and command arguments unchanged** except for the addition of `Rearm`. All 0.6.0 commands take the same arguments and behave the same way.
* **Load-arg syntax unchanged.** `before`, `after`, `require_ignore_drop`, `max_events`, `excludes` are all parsed identically to 0.6.0.
* **No operator action required** when upgrading a running 0.6.0 deployment. On the next load of 0.7.0, the armed flag is re-evaluated with the improved logic. If `znc.conf` currently has `LoadModule = highlightctx` before `LoadModule = ignore_drop`, auto mode will remain unarmed until the order is fixed or until the module is reloaded manually — the same remediation that already applied under 0.6.0, just now clearly surfaced by `Status` and `Rearm`.

### Not added (honesty note)

An earlier draft of 0.7.0 included `OnModuleLoading` and `OnModuleUnloading` overrides that were intended to track runtime load/unload of `ignore_drop` and emit user notices. End-to-end testing revealed that ZNC 1.9.x dispatches these two hooks only to global-scope modules (they are declared in the `// Global Modules` section of `CModules` in `/usr/include/znc/Modules.h` and invoked via `GLOBALMODULECALL`), so a network-scope module that overrides them never receives the callback. The overrides were removed before release because they were dead code that misrepresented the module's actual tracking behavior. Runtime load/unload of `ignore_drop` is instead covered by:

- the independent live `HasIgnoreDropLoaded()` check inside `ShouldCaptureNow()`, which protects capture correctness on every incoming message, and
- the `Rearm` command for refreshing the armed flag on demand after an operator-initiated runtime change.

### Testing

An automated test harness was written for this release (not shipped with the module). It runs against ZNC 1.9.0 as a non-root user via `runuser` and a fake IRC server for the upstream connection. Coverage:

- module load + version + default settings + all expected `Status` keys (including the new label) — 8 assertions
- `znc.conf` load order with `ignore_drop` first: correct armed state, effective requirement active — 4 assertions
- `znc.conf` load order with `highlightctx` first: correctly unarmed, `Rearm` reports position problem with remediation — 5 assertions
- full command surface: `SetBefore`/`SetAfter`/`SetMaxEvents` (including numeric and `off`), `AddExclude`/`DelExclude`/`ListExcludes`, `Reset`, `SetRequireIgnoreDrop on` rejected when absent, mode preservation on rejection, `Rearm` diagnostic output — 11 assertions
- runtime `LoadMod ignore_drop` after highlightctx: `Rearm` correctly reports wrong order, subsequent `UpdateMod highlightctx` moves it to the tail and `Rearm` arms — 5 assertions
- `UnloadMod ignore_drop` at runtime: sticky armed remains `yes`, effective requirement remains `yes`, subsequent `Rearm` explicitly disarms and reports the transition — 7 assertions
- detached-capture + on-attach replay: disconnect client, inject channel traffic with a highlight, reconnect, verify replay contains the channel, the event header, the `>>>` trigger marker, the triggering speaker, `pending=0`, and journal compaction — 7 assertions
- 0.6.0 → 0.7.0 upgrade: seed a 0.6.0-format journal, attach, verify on-attach replay delivers the event correctly, `pending=0` afterward, and journal is compacted — 6 assertions

Result: **55/55 passing** against ZNC 1.9.0.

---

## [0.6.0] — Initial release

### Added

* Detached-only highlight context capture as a ZNC network module for ZNC 1.9.1+.
* Independent per-channel in-memory ring buffer for pre-highlight context, decoupled from normal ZNC playback buffer length.
* Durable on-disk journal (`highlightctx.journal`) recording only real highlight events (`B` begin, `A` after-line, `F` finalize, `D` delivered) with `fsync` + parent-directory fsync for write durability, and atomic tmp+rename compaction.
* Automatic replay into `*highlightctx` on `OnClientAttached`, sorted by channel then event time/ID, followed by compaction and clearing of delivered events.
* Native IRCv3 `server-time` / `@time=` replay for clients that advertise support, with an inline UTC-prefixed fallback for clients that do not.
* Nick-boundary-aware highlight detection (case-insensitive) against the current network nick.
* `ignore_drop` integration with `off` / `on` / `auto` modes, gated on presence at `OnLoad` time.
* Configurable `before` and `after` context caps, per-channel exclusion list, and optional `max_events` cap on the pending-event list.
* Runtime commands: `Help`, `Overview`, `Version`, `Status`, `ReplayNow`, `SetBefore`, `SetAfter`, `SetMaxEvents`, `AddExclude`, `DelExclude`, `ListExcludes`, `SetRequireIgnoreDrop`, `Reset`, `Compact`, `ClearPending`.
* Persistent settings via module NV entries, with a legacy-compatibility alias for the old `require_ignore_drop` boolean.
