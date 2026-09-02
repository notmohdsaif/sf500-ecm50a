# Offline Autonomy — session handoff (2026-09-03)

Resume point for `sf500_107888` offline-autonomy + SD data retention.

## Where things are

- **Branch:** `offline-autonomy`, worktree `.claude/worktrees/offline-autonomy`.
  Rebased onto `cellular-fallback` (base tip `80f56a9`). **16 commits, NOT pushed.**
- **Build:** `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1` → SUCCESS
  (Flash 33.4%, RAM 20.8%). `~/.platformio/penv/bin/pio test -e native` → 15/15.
- **Working tree clean.** `src/config_secrets.h` is present (gitignored) for builds.
- The `cellular-fallback` worktree was left untouched.

## What's done (Phases 0–4 of `docs/superpowers/plans/2026-09-02-offline-autonomy.md`)

Full detail in the plan's "Implementation status — 2026-09-03" block and in the
spec `docs/superpowers/specs/2026-09-02-offline-autonomy-design.md`.

New modules: `src/{sdcard,netstate,persist,journal,backfill}.{h,cpp}`,
`test/native_shim/Arduino.h`, `test/test_{persist,journal}/`,
`docs/migrations/offline-recorded-at.sql`.

Behaviour now: on `sf500_107888`, auto-dosing + WL detection + refill cutoff run
every loop regardless of WiFi/4G state (control plane moved ahead of the
no-uplink / not-registered early-returns, gated on `configLoaded()`); config +
schedules + a coarse clock persist to NVS/SD for cold-start; telemetry
buffer-then-drains through an NDJSON journal on SD with `recorded_at`; one
bounded `backfillTick()` replays it on recovery.

## Pick up here (in order)

1. **Apply the Supabase migration FIRST.** `docs/migrations/offline-recorded-at.sql`
   → prod project `qkqeysggrqhxizkdmbhx` (nullable `recorded_at timestamptz` on
   `sensor_metrics` / `activity_log` / `relay_metrics`, then
   `notify pgrst, 'reload schema'`). The firmware now sends `recorded_at` on every
   insert — until the column exists, PostgREST rejects the whole insert, so this
   branch must not run against live Supabase before the migration.
2. **Phase 5 bench soak** on `sf500_107888` (it's on cellular spike fw + in use —
   needs hardware time): flash, watch serial for the `[SD]` mount line + CD-pin
   polarity (Task 0.4), then stack high-water (`uxTaskGetStackHighWaterMark`),
   card-pull, power-cut-during-write, 24–48 h induced-outage soak. Stack is the
   v1.2.5 crash class — SdFat + ArduinoJson + mbedTLS all on `loopTask@20480`.
3. **Streaming two-pass retention** — `journalEnforceRetention()` currently
   skips+logs above 2 MB, so the 256 MB cap is inert on real hardware. The pure
   `journalEvictSensorMetrics()` + its host tests are ready for the wrapper.
4. Task 1.3 (fully non-blocking WiFi reconnect) — deferred; only matters if this
   goes fleet-wide. Harmless on 107888 (no WiFi creds → the 3×10 s loop is
   skipped and cellular fallback fires immediately).
5. Version bump + PR only after a clean Phase 5.

## Notes / gotchas

- `[env:native]` needs `test_build_src=true`, `build_src_filter`, and
  `-Itest/native_shim` (a `String`=`std::string` + `strlcpy` shim). Pure logic in
  `persist.cpp` / `journal.cpp` is always-compiled; device code is behind
  `#ifndef UNIT_TEST`.
- `journalEncode` must use `snprintf` for the number, not `s += (long)` — the
  latter stringifies on Arduino `String` but appends a raw byte on `std::string`.
- `StaticJsonDocument` for the config round-trip is sized 1024 (64-bit host slots
  are bigger than the ESP32's; safe on both).
- Do not push, tag, or PR anything without an explicit go-ahead.
