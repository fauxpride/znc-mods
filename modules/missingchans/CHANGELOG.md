# Changelog — missingchans

All notable changes to the `missingchans` ZNC module are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This module uses sequential build revisions (`rN`) rather than [Semantic Versioning](https://semver.org/spec/v2.0.0.html); revision numbers increase monotonically with each release.

---

## r9 — 2026-05-21

### Changed

- **The module's own self-WHOIS round-trip is no longer surfaced to attached IRC clients.** Previously, every verification cycle caused a burst of `311` / `312` / `313` / `317` / `319` / `330` / `338` / `671` / `318` (or `401`) numerics for the bouncer's own nick to arrive in each attached client's IRC window, which is noisy and confusing because the user did not ask for that WHOIS. Under r9 those replies are intercepted before they reach any attached client. The module still processes them internally, so the verification table (expected / actual / verified / missing) is produced exactly as before — only the underlying WHOIS exchange is hidden.
- **User-initiated `/whois` is unaffected.** WHOIS commands sent from an attached client (regardless of target — yourself, someone else, anyone) still propagate their reply numerics back to clients exactly as in r8 and earlier. The suppression applies only to the WHOIS that this module itself originates.
- **Build marker updated** from `2026-05-21+r8 (... voiced-channel parser fix)` to `2026-05-21+r9 (... voiced-channel parser fix; hidden self-WHOIS)`.

### Implementation notes

- Distinguishing module-initiated from user-initiated WHOIS is done with a small FIFO queue (`m_whoisOrigins`) of request origins, since IRC servers serialize WHOIS replies in request order on a single connection. When the module sends WHOIS via `PutIRC`, it pushes `true` onto the queue; when an attached client sends WHOIS (caught via the new `OnUserRawMessage` hook), it pushes `false`. The front of the queue identifies whose replies are currently arriving; we pop on `318` or `401` (end of WHOIS). The queue is cleared on IRC disconnect to avoid stale state.
- The intercept itself happens in `OnNumericMessage`, which returns `HALT` for WHOIS-related numerics whose origin at the front of the queue is `true` (ours), and `CONTINUE` otherwise. The set of intercepted numerics is `311`, `312`, `313`, `317`, `319`, `330`, `338`, `671`, `318`, `401` — the standard WHOIS reply repertoire.
- No new ZNC API surface is used beyond `OnUserRawMessage` (available since ZNC 1.7.0) and the existing `OnNumericMessage` return-value semantics. Built and runtime-tested against ZNC 1.9.1 (built from source).

### Compatibility

- **NV storage format is unchanged.** Settings persisted under r8 are read back identically by r9; no migration is performed or required. Verified by setting non-default values for `delay`, `joinmissing`, `expectedmode`, `retries`, `retrystep`, and `retryperform`, unloading the module, replacing the `.so`, and reloading — all values reappear correctly.
- **Command-line behavior is byte-identical to r8** — `HELP`, `VERSION`, `STATUS`, `SHOW`, `RUN`, every `SET` valid/invalid path, unknown commands, and HELP aliases (`h`, `?`) produce exactly the same output. Verified by 22 captured commands run under both r8 and r9 on the same ZNC 1.9.1 instance with NV state reset between runs.
- **The r8 voiced-channel parser fix is preserved.** Voiced (`+#chan`), op (`@#chan`), admin (`&#chan`), and compound user-mode prefix tokens (`~&@#chan`, `~&@%+#chan`) in WHOIS `319` replies are still correctly stripped down to the underlying channel name. End-to-end test confirms that all 17 channels from the original bug report (including the two voiced ones) are correctly classified as joined.
- **No new settings.** The suppression is unconditional; if you ever want to inspect the module's WHOIS exchange for debugging, you can do so via ZNC's traffic log rather than via an attached client.

### Testing

- Built cleanly against ZNC 1.9.1 with no compiler warnings.
- **End-to-end test** with a Python mock IRC server, replicating the bug-fix scenario from r8 (17 expected channels, 2 of them voiced):
  - **Test A1:** The client sees zero WHOIS reply numerics for the bouncer's nick when the module runs its verification. Previously (r8) the client would see five numerics (`311`, `312`, `317`, `319`, `318`) per cycle.
  - **Test A2:** The module's table output (the `PRIVMSG` from `*missingchans` listing expected / actual / missing) is still delivered to the client — 40 module lines received during a `RUN`, confirming the module is processing the WHOIS internally even though the raw numerics are hidden.
  - **Test A3:** The r8 voiced-channel parser fix continues to work — all 17 channels appear in `actual` (no `+#`-prefixed tokens), `missing` is empty.
  - **Test B:** The user manually issuing `/WHOIS testuser` after the verification cycle receives all 5 reply numerics normally, confirming user-initiated WHOIS is unaffected.
- **Command-level regression test against r8:** 22 representative commands captured under both versions on the same ZNC 1.9.1 instance, NV state reset between runs. All 22 produce byte-identical output.
- **NV persistence test:** `delay`, `joinmissing`, `expectedmode`, `retries`, `retrystep` round-trip correctly across module unload/reload under r9.

---

## r8 — 2026-05-21

### Fixed

- **Voiced (and admin-mode) channels were misclassified as "missing".** Tokens in the WHOIS `319` reply that carry a user-mode prefix — for example `+#phat` for "voiced in #phat" or `&#chan` for "admin in #chan" on networks where `&` is a user-mode prefix — were being stored verbatim in the module's `actual` set instead of having the leading user-mode prefix stripped. Because `actual` then contained `+#phat` while `expected` contained `#phat`, the channel was reported as missing and the module would schedule a retry join even though the user was already present in the channel. Reported by a user running on Undernet (PREFIX=`(ov)@+`) where some channels appeared in the WHOIS reply with the `+` voice prefix.
- **Root cause:** `StripPrefix` stopped at the first occurrence of any character in `{#, &, +, !}`. Three of those four (`#`, `!`, and unambiguously `&` on most networks) are channel-type prefixes, but `+` and `&` are *also* valid user-mode prefix characters (voice and admin, respectively). The function had no way to distinguish "leading `+` is a voice prefix to strip" from "leading `+` is a modeless-channel type prefix to keep".
- **The fix:** `StripPrefix` now strips unambiguous user-mode prefix characters (`~`, `@`, `%`) freely, and treats `+`/`&` as user-mode prefixes *only if* the next character is itself prefix-like (`~`, `@`, `%`, `+`, `&`, `#`, or `!`). Otherwise the `+`/`&` is treated as the channel-type prefix and parsing stops there. This correctly handles every realistic IRC prefix combination including `#chan`, `+#chan`, `@#chan`, `~&@#chan`, `+chan` (modeless channel), `&local` (local channel), `@+modeless` (op in modeless channel), etc.
- **Defense-in-depth:** `StripPrefix` now also returns the empty string when the stripped result does not start with a recognized channel-type prefix, instead of returning arbitrary garbage. This means malformed WHOIS tokens like `junk#chan` are now refused rather than being silently mangled and inserted into `m_actual`. Under r7 (and earlier), the parser would have munched the leading `junk` and inserted `#chan` regardless. Under r8 it returns empty and the caller filters it out.

### Compatibility

- **Strict superset of r7 behavior for well-formed input.** Every IRC client / IRCd / network that produced correct module behavior under r7 continues to produce the same correct behavior under r8. The fix is purely in the parser path, and only changes results for previously-broken inputs (tokens with a user-mode `+` or `&` prefix, or malformed garbage tokens that should never have parsed in the first place).
- **No new ZNC API surface** is used. Built and runtime-tested against ZNC 1.9.1 (built from source).
- **NV storage format is unchanged.** Settings persisted under r7 are read back identically by r8; no migration is performed or required.
- **Command-line behavior is byte-identical to r7** — `HELP`, `VERSION`, `STATUS`, `SHOW`, `RUN`, all `SET` paths (valid and invalid), unknown commands, and HELP aliases (`h`, `?`) all produce exactly the same output.
- **The build marker string is updated** from `2026-05-21+r7 (... expanded HELP)` to `2026-05-21+r8 (... voiced-channel parser fix)`. The `Build:` line printed by `HELP` and `VERSION` reflects the new marker.

### Testing

- **Unit test of the new `StripPrefix`** in isolation across 32 cases covering every realistic IRC prefix combination: plain channels (`#`, `&`, `!`, `+`), single user-mode prefixes (`~`, `@`, `%`, `+`, `&`), compound user-mode prefixes (`~&@#chan`, `~&@%+#chan`, `@+#chan`), user-mode prefixes on local channels (`@&local`, `+&local`), user-mode prefixes on safe channels (`@!safe`, `+!safe`), the ambiguous case of an op in a modeless channel (`@+modeless`), edge cases (empty string, lone prefix chars), and malformed garbage (`junk#chan`, `chan`, `hello world`). All 32 cases pass. Specifically: `+#phat` → `#phat` and `+#political` → `#political`, the two cases from the original bug report.
- **Command-level regression test against r7**: 22 representative commands captured under both versions running on the same ZNC 1.9.1 instance, NV state reset between runs. All 22 produce byte-identical output. The fix is parser-only and doesn't touch any command-handler path.
- **End-to-end reproduction with mock IRC server.** A Python TCP server speaking just enough IRC for ZNC to register and process a self-WHOIS was used to replay the user's exact failure scenario:
  - 17 expected channels in znc.conf;
  - 15 of them returned in `319` without a user-mode prefix;
  - 2 of them returned with the `+` voice prefix (`+#phat` and `+#political`);
  - Module set to `joinmissing on`;
  - `/msg *missingchans RUN`.
- **Under r6** (and equivalently r7), the test reproduced the bug bit-for-bit identically to the user's report: `actual` contained `+#phat` and `+#political`, `missing` contained `#phat` and `#political`, and the module scheduled a retry. **Under r8**, all 17 channels appeared correctly in `actual` (without the voice prefix), `missing` was empty, and the module reported "✔ All expected channels appear joined on the server."
- Built cleanly against ZNC 1.9.1 with no compiler warnings.

---

## r7 — 2026-05-21

### Changed

- **`HELP` output expanded.** The previous build listed only a subset of settings under a "Key settings" heading and used parenthetical one-line descriptions. The new help lists every persisted setting under a "Settings (configure via: SET &lt;key&gt; &lt;value&gt;):" heading, each with a multi-line description that covers what the setting does, the accepted value range, and the default. The seven settings now documented are: `delay`, `joinmissing`, `expectedmode`, `retryperform`, `retries`, `retrystep`, `stopperformon`. The `delay` key was not listed in earlier help output.
- **Build marker updated** from `2026-02-11+r6` to `2026-05-21+r7 (robust 319 + case-insensitive chans + 443 fallback; expanded HELP)`.

### Fixed

- None. This release contains no functional fixes.

### Compatibility

- **Behavior is otherwise unchanged.** All commands (`RUN`, `SHOW`, `STATUS`, `VERSION`, `HELP`, `SET`) and every `SET` validation path (`delay >= 1`, `retries >= 1`, `expectedmode` normalization to `all|config|enabled`, `stopperformon` clear values `off`/`none`/`-`/empty, etc.) behave identically to r6 for well-formed and ill-formed input alike.
- **No new ZNC API surface** is used relative to r6. Built and runtime-tested against ZNC 1.9.1 (built from source). The compiled `.so` is ABI-compatible with the same range of ZNC versions r6 was.
- **NV storage format is unchanged.** Settings persisted under r6 are read back identically by r7; no migration is performed or required. Verified by setting non-default values under r6, unloading the module, replacing the `.so`, and reloading — all values reappear correctly.
- **Help aliases (`h`, `?`, empty command) all show the new expanded help**, same as in r6 where they all showed the previous (subset) help.

### Testing

- Built cleanly against ZNC 1.9.1 with no compiler warnings.
- Loaded into a running ZNC 1.9.1 and exercised end-to-end via a Python IRC test client:
  - `HELP` confirmed to list all seven settings with their descriptions and the `r7` build marker.
  - `VERSION`, `STATUS`, `SHOW` confirmed working.
  - All `SET` paths exercised: every valid value accepted with the same confirmation strings as r6; every invalid value rejected with the same error strings as r6 (`Delay must be >= 1.`, `Retries must be >= 1 (total attempts).`, `Unknown SET key. Try: HELP`).
  - Unknown command path produces the same `Unknown command. Try: HELP` reply.
  - `HELP` aliases (`h`, `?`) confirmed to invoke the same path.
- Regression comparison against r6: 19 representative commands captured under both versions running on the same ZNC 1.9.1 instance, with NV state reset between runs to ensure identical initial conditions. All 19 produced byte-identical output. The only intended difference is `HELP` itself: 16 lines under r6, 35 lines under r7.
- NV persistence across unload/reload verified for `delay`, `joinmissing`, `expectedmode`, `retries`, `retrystep`, and `retryperform`.

---

## r6 — 2026-02-11

### Changed

- **Channel name comparisons are case-insensitive.** A `CStringCI` comparator using `AsLower()` is used for the `expected`, `actual`, `verifiedJoined`, `missing`, and `lastAttemptMissing` sets, so `#Chan` and `#chan` are no longer treated as different channels (which previously produced false "missing" reports).
- **319 (RPL_WHOISCHANNELS) parsing is more robust.** Parameters from index 2 onward are concatenated and then split by spaces, so implementations that split the channel list across multiple params parse correctly. Each token is passed through `StripPrefix` to remove user-mode prefixes before insertion.

### Compatibility

- **443 fallback ("already on channel") is retained** as an additional server-truth signal. When the server emits 443 for a channel in the expected or last-attempt-missing set, the channel is treated as joined (helps when self-WHOIS omits `+s` / `+p` channels on some networks).
- **Build marker** updated from r5 to `2026-02-11+r6 (robust 319 + case-insensitive chans + 443 fallback)`.

---

## Earlier revisions (r1 – r5)

Not documented in this changelog. r5 predates the case-insensitive comparison and robust 319 parsing introduced in r6.