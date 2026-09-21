# Offline-autonomy Phase 2d — fix D1/D2/D3, bench-validate, code-review

Branch `offline-autonomy` (tip `64d2489`, 42 commits over v1.2.6 `main`), worktree
`.claude/worktrees/offline-autonomy`. Not pushed. Full background/history in project memory
(`project_offline_autonomy.md`) — this file covers this session's checklist + result per
CLAUDE.md's Task Management convention.

## Checklist

- [x] D1 — gate the offline captive portal behind sustained downtime (180s) + a completed
      radio power-cycle, instead of opening almost immediately (~1s). `main.cpp`.
- [x] D2 — replace `isoFromEpochUtc()` (`gmtime_r`) with `isoFromEpoch()` (`localtime_r`,
      mirrors `isoNow()`). `cloud.cpp`/`cloud.h`/`backfill.cpp`.
- [x] D3 — move the `[stack]` high-water log above the offline early-return so it doesn't go
      silent for the whole duration of an outage. `main.cpp`.
- [x] Build (`pio run -e esp32-s3-devkitm-1`) + host tests (`pio test -e native`, 16/16).
- [x] Bench-test on real hardware (`sf500_107888`, tethered USB, 2026-09-21):
  - [x] Multi-transport handoff (WiFi down, cellular rescues) — clean, no premature portal.
  - [x] Found + fixed a 4th bug live: WiFi-ladder state (`wifiDownSince` etc.) wasn't reset on
        a successful cellular handoff, so a later cellular->WiFi drop resumed against a stale
        timer and could reopen the D1 bug through a transport bounce instead of a plain outage.
        Fixed via `clearWifiReconnect()` in `tryCellularFallback()` on success.
  - [x] No-cellular last-resort test (antenna pulled) — portal correctly opened at 216s down,
        proving the gate isn't "never opens".
  - [x] Timely-recovery test (antenna still out) — clean reassociation, zero portal opens.
  - [x] D3 confirmed live — `[stack]` kept logging throughout every outage window.
  - [ ] D2 not live-repro'd — this bench unit's boot sequence structurally can't expose the
        bug through its only two loggable offline events (both fire from `bringOnline()`,
        which only runs post-sync). Confidence is code-level (see next item), not bench-proven.
- [x] `/code-review high` pass on the full diff. 10 findings, all verified individually (not
      taken on faith):
  - [x] **[CRITICAL]** `seedClockFromStore()` (`persist.cpp`) restored the epoch via
        `settimeofday()` but never set TZ — on a cold offline boot (no NTP/cellular yet),
        `localtime_r()` renders unshifted UTC digits while `isoNow()`/`isoFromEpoch()` still
        tag them `+08:00`, reproducing D2's exact skew through a path the D2 fix never
        touched. Verified against the actual ESP32 Arduino core source
        (`esp32-hal-time.c`) that `configTime()` sets TZ via `setenv`, not a shifted epoch —
        this overturned my own earlier assumption about how the mechanism worked. Fixed by
        adding the same two lines (`setenv("TZ","UTC-8",1); tzset();`) `cellular.cpp`'s NITZ
        path already uses for the identical problem. Not live-repro'd (see above) — confidence
        is source-verified + exact-pattern-match, not bench-proven.
  - [x] Portal's last-resort gate could be starved indefinitely by a flapping AP (any momentary
        `WL_CONNECTED` blip reset `wifiDownSince`/`wifiHadCycle` before the 120s cycle or 180s
        gate ever completed) — fixed with a 15s stability debounce before declaring the outage
        over, mirroring the existing `wifiUpSince` cellular up-switch pattern. Not live-tested
        (would need a real flapping AP).
  - [x] `clearWifiReconnect()` unconditionally logged "[WiFi] Re-associated: <ip>" even when
        called from a successful cellular handoff (where WiFi never reconnected, IP is
        0.0.0.0) — moved the log to the one call site where it's actually true.
  - [x] `shouldOpenPortalOffline()` paid an NVS (flash) read on every loop (~100/s) even when
        cellular was active or WiFi was up, i.e. in the common case — short-circuited before
        the read.
  - [x] Stale comment in `backfill.cpp` still named the pre-rename `isoFromEpochUtc()`.
  - [x] Redundant `wifiHadCycle` bool — removed, derived inline as `wifiLastCycle !=
        wifiDownSince` (same two variables already tracked it; a future edit to one without
        the other could have silently desynced them).
  - [x] Corrected a `cloud.cpp` comment that had the wrong theory of why `configTime()` makes
        `isoNow()` correct (my TZ misunderstanding, see above) — now describes the real
        mechanism and cross-references `seedClockFromStore()`.
  - [ ] Not fixed, logged as future work, out of scope tonight: 5 pre-existing call sites
        elsewhere (`mqtt_handler.cpp`, `sensors.cpp`, `cloud.cpp`'s `updateDeviceStatus`) hand-roll
        the same `localtime_r`+`sprintf` block `isoFromEpoch()` now canonicalizes — could be
        collapsed onto it, but touches currently-working, untested-tonight code paths.
  - [ ] Not fixed, noted only: the transport-bounce fix (`clearWifiReconnect()` in
        `tryCellularFallback()`) is a manually-placed call at the one current call site, not
        structurally guaranteed — a future second `activeTransport = TRANSPORT_CELLULAR` site
        would need the same call added by hand.
- [x] Rebuilt + re-ran host tests after every fix batch — still SUCCESS / 16/16 throughout.
- [x] Diff still uncommitted by design — holding for explicit go-ahead (this branch's Phase 3
      — version bump, PR, staged OTA — was already deliberately held pending user sign-off,
      see project memory / [[feedback_check_deferred_conditions_before_release]]).

## Review / summary

D1 and D3 are now bench-proven on real hardware, including a bug (#4, the transport-bounce
stale timer) that only surfaced through live testing — code review alone would very likely
have missed it, and bench testing alone would have missed the TZ bug the review caught. Doing
both was the right call.

D2's fix is sound in its own right, but the code review revealed the field-observed 8h skew's
real root cause is one level deeper (`seedClockFromStore()` never establishing TZ) — now also
fixed, verified against the actual framework source, but not exercised on real hardware tonight
because this specific bench unit's boot sequence can't reach the buggy window through its only
two loggable offline events. That's the one item in this diff resting on code-level confidence
rather than a live repro.

Net: 6 files changed (`backfill.cpp`, `cloud.cpp`, `cloud.h`, `config.h`, `main.cpp`,
`persist.cpp`), build clean, 16/16 host tests, still uncommitted pending user go-ahead.
