# Auto-dosing stall fix + console noise cleanup + MET fixes — bench-validated, NOT SHIPPED

Branch `dev`, 4 local commits ahead of `origin/dev` (`f0a282f` tip, over `ec21d19`/v1.2.9).
Nothing pushed, no PR, no tag. Full narrative in project memory
(`project_auto_dosing_stall_fix_2026_09.md`) — this file is the per-session checklist +
result per CLAUDE.md's Task Management convention.

## Original report

`sf500_1078bc` (farm unit, known flaky/uncalibrated EC probe — see
`project_ec_probe_calibration_overdue.md`) repeatedly got stuck in `AUTO_STABILISING`,
recoverable only via a manual dashboard reset. Also asked to quiet the `[stack] loopTask
free...` console line, and asked separately why `wl_sensor_missing`/`ec_sensor_missing`
alarms kept re-firing after ack even for unconnected sensors.

## Checklist

- [x] Root-caused the stall: `ecReadingCount`/`stabiliseSkipCount` only advance on a
      successful, plausible EC read (`ecReadOk` gate in `readSensors()`) — a probe that never
      produces a plausible reading stalls both counters with no timeout anywhere in the path.
- [x] Fixed `AUTO_STABILISING`: folded a timeout into the *existing* ineffective-dose
      accounting (not a bare bypass — first attempt at this was a critical regression caught
      by code review, see below) — same escalation to `ALARM_REASON_NO_EC_RESPONSE` as a real
      failed dose response.
- [x] Added `AUTO_SAMPLING` stall guard (new, beyond the literal bug report — user
      pushed back on scope once, then confirmed keep-it after clarifying the actual
      requirement is "recover fast," not "add features"): new self-clearing
      `ALARM_REASON_EC_DATA_UNAVAILABLE`, exempted while `r3State` (refill) is active,
      mirroring `AUTO_STABILISING`'s existing refill exemption.
- [x] Timeout tuning, iterated live with user: 600s (bench-validated) → 90s (user's explicit
      urgency call) → 180s (2nd code review caught 90s violating the pre-existing
      `EC_IMPLAUSIBLE_CONFIRM_MS` = 120s floor — a 90s alarm could fire *during* a real
      legitimate dilution/refill event, before the system's own grace window even closes).
      Final: `STABILISE_TIMEOUT_MS` = `SAMPLING_STALL_TIMEOUT_MS` = 180000UL.
- [x] Console noise: `[stack]` heartbeat 60s → 300s (kept the instant-log-on-new-low-water-
      mark trigger — removing it was my own regression, caught by 1st code review, since it
      could lose crash evidence right before an actual crash). Auto-dosing debug heartbeat
      rewritten from a fixed 10s timer to log only on state change / sample-window-fill / 5min
      fallback — this was the actual bulk of the console noise.
- [x] `wl_sensor_missing`/`ec_sensor_missing` alarm design: confirmed by user as
      intentional (no opt-out for unconnected sensors) — left as-is. The *frequency* bug
      (re-firing on nearly every page load, wiping acks) was a Dashboard-side race between
      MQTT `sensorStatus` and an async Supabase `sensorPresence` fetch — separate repo.
- [x] MET sensor fixes bundled in from a prior local commit's review: register read merged
      into one Modbus transaction (`MET_REG_COUNT` 9→12, `MET_REG_LUX_OFFSET`), boot detection
      wrapped in a `MET_DETECT_RETRIES` (3x) loop, new `ambDataFromAmbient` global fixes a
      mislabeling bug in `cloud.cpp`'s upload tagging (was using boot-time presence
      `ambSensorFound` instead of per-tick actual source).
- [x] Two full `/code-review` passes, all correctness findings fixed (STABILISING bypass
      regression, sub-120s false-alarm conflict, alarm message math bug — `/60000UL` was
      integer-dividing to a constant `1`, MET/Ambient mislabeling). Style/architecture findings
      deliberately deferred (see Known limitations).
- [x] Build clean (`pio run -e esp32-s3-devkitm-1`) after every change.
- [x] Bench-tested on `sf500_3a387c` (office unit — `sf500_1078bc` is farm-deployed, no
      physical access, and this codebase has no per-device OTA targeting so any release
      reaches the whole fleet):
  - [x] `AUTO_SAMPLING` stall guard + self-clearing auto-recovery — confirmed via live serial
        capture and Supabase `activity_log`, tested at the 90s value.
  - [x] `AUTO_STABILISING` timeout folded into ineffective-dose accounting, including its
        chaining into the `SAMPLING` guard — confirmed at the 600s and 90s values.
  - [x] Flashed the final `f0a282f` build (180s + refill exemption + MET/ambient fixes) fresh
        — clean `POWERON` boot, WiFi/MQTT/NTP/registration all fine, OTA version-guard
        correctly refused to downgrade against fleet's `v1.2.8`, MET retry loop confirmed live
        (`"Weather station: not found (ID 35, 3 attempts)"`), state machine ran
        `STARTUP_WAIT → SAMPLING` cleanly with 30/30 samples.
- [x] Confirmed `FIRMWARE_VERSION` = "1.2.9" (not "1.2.10" — 1.2.9 itself was never tagged
      or released, only ever committed locally before tonight).

## Known limitations / concerns — circle back before or shortly after shipping

- [ ] **The `r3State` refill exemption on `AUTO_SAMPLING` has never executed under a real
      refill event** — every bench test tonight had the plug off throughout, so the alarm
      firing was tested, but the exemption suppressing it during an actual refill was not.
      Bounded risk: it's a boolean gate on the same `r3State` flag already used everywhere
      else for relay control, not new instrumentation. Worst case if wrong: the guard simply
      doesn't help during a real refill and the alarm fires anyway — not worse than today's
      pre-fix behavior, just doesn't add the intended grace. Cannot cause a *new* failure mode.
- [ ] **The exact 180s timeout values are not themselves stress-tested in real time** — only
      90s and 600s were empirically run to completion. 180s is the same proven logic at a
      different constant; low risk, but not literally bench-timed.
- [ ] **MET/weather-station fixes (register merge, retry loop) unverified on real hardware** —
      this bench unit has no MET sensor. Narrow blast radius: this MET code has never shipped
      to the fleet before (still local-only prior to tonight per `project_weather_station_met_sensor.md`),
      so this isn't a new regression for any currently-deployed device — only matters once a
      MET-equipped unit gets this release.
- [ ] **Known, unfixed: MET lux 32-bit truncation.** Register 511 is documented as only the
      low 16 bits of a 32-bit lux value; the high-word register address is undocumented
      anywhere in this codebase. Flagged, not guessed at — needs the sensor's actual datasheet
      or a bench sweep of adjacent registers.
- [ ] **Deferred style/architecture findings from the 2nd code review** (not correctness bugs,
      explicitly out of scope per user's own scope-discipline feedback this session): MET
      register constants could be named better, MET polling cadence duplicates the main sensor
      loop's, the ineffective-dose accounting logic is now duplicated between the `DOSING`
      failure path and the new `STABILISING` timeout fold rather than shared, MET/Ambient
      field-sharing (`sensors.ambTemp` etc.) is a slightly awkward shared-field architecture
      that works but isn't obviously the cleanest shape long-term.
- [ ] **No per-device OTA targeting exists in this codebase at all** (`ota.cpp` pulls
      `releases/latest` unconditionally for the whole fleet) — any tag reaches every device,
      not just the one that was actually reported stuck. Pre-existing constraint, not new
      tonight, but directly shapes how cautiously this specific release should be rolled out.
- [ ] **Dashboard-side `wl_sensor_missing` frequency fix is a separate repo/deploy** —
      `use-alarms.ts`'s `dataReady` gating now also waits on `waterLevelSensorsReady`;
      typecheck (`tsc --noEmit`) and `npm run build` both clean, but not manually re-verified
      live in a browser this session.
- [ ] **Nothing pushed** — still 4 local commits on `dev`, `origin/dev` has none of this.

## Review / summary

Root cause (unbounded stall on a probe that never produces a plausible EC read) is fixed at
both places it can occur (`STABILISING` and `SAMPLING`), with self-clearing recovery so no
manual dashboard reset is needed for this specific fault class going forward. Two rounds of
code review closed every correctness-level finding, including two regressions I introduced
myself along the way (the STABILISING bypass, and briefly dropping the stack log's crash-
evidence trigger) — both caught before shipping, not after. The remaining open items above
are genuine gaps, not hedging: the refill-exemption path is analytically bounded-safe but
not live-proven, and MET hardware fixes have zero bench coverage since no MET-equipped unit
was available tonight. Held at user's request pending a decision on timing rather than
pushed immediately.
