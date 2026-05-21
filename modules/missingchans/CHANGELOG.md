# Changelog — missingchans

All notable changes to the `missingchans` ZNC module are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This module uses sequential build revisions (`rN`) rather than [Semantic Versioning](https://semver.org/spec/v2.0.0.html); revision numbers increase monotonically with each release.

---

## [r7] — 2026-05-21

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

## [r6] — 2026-02-11

### Changed

- **Channel name comparisons are case-insensitive.** A `CStringCI` comparator using `AsLower()` is used for the `expected`, `actual`, `verifiedJoined`, `missing`, and `lastAttemptMissing` sets, so `#Chan` and `#chan` are no longer treated as different channels (which previously produced false "missing" reports).
- **319 (RPL_WHOISCHANNELS) parsing is more robust.** Parameters from index 2 onward are concatenated and then split by spaces, so implementations that split the channel list across multiple params parse correctly. Each token is passed through `StripPrefix` to remove user-mode prefixes before insertion.

### Compatibility

- **443 fallback ("already on channel") is retained** as an additional server-truth signal. When the server emits 443 for a channel in the expected or last-attempt-missing set, the channel is treated as joined (helps when self-WHOIS omits `+s` / `+p` channels on some networks).
- **Build marker** updated from r5 to `2026-02-11+r6 (robust 319 + case-insensitive chans + 443 fallback)`.

---

## Earlier revisions (r1 – r5)

Not documented in this changelog. r5 predates the case-insensitive comparison and robust 319 parsing introduced in r6.

[r7]: ./missingchansv7.cpp
[r6]: ./missingchansv6.cpp
