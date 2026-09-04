# Offline Autonomy — session handoff (updated 2026-09-04)

Resume point for `sf500_107888` offline-autonomy + SD data retention.

## Where things are

- **Branch:** `offline-autonomy`, worktree `.claude/worktrees/offline-autonomy`.
  Rebased onto `cellular-fallback` (base tip `80f56a9`). **27 commits, NOT pushed.**
  Working tree clean.
- **Build:** `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1` → SUCCESS
  (Flash 33.6%, RAM 20.8%). `~/.platformio/penv/bin/pio test -e native` → 16/16.
- **Firmware currently flashed to `sf500_107888`:** branch tip (commit `82bb1a0`
  code). Still `FIRMWARE_VERSION` 1.2.5 — bench build, not a release.
- **`recorded_at` migration:** APPLIED to prod (`qkqeysggrqhxizkdmbhx`,
  migration `offline_recorded_at`) 2026-09-03.
- The `cellular-fallback` worktree was left untouched.

## Implementation — COMPLETE

Phases 0-4 (see the plan's "Implementation status" block). Phase 5:

| Test | Result |
|---|---|
| 0.4 SD hardware (pins, 20MHz mount, CD polarity) | PASS |
| Stack high-water (loopTask, v1.2.5 crash class) | PASS — 13.9 KB free of 20480 |
| Card-pull hot-remove / reinsert | PASS — `sdTick()`, no crash, no reboot |
| Power-cut during write (x3 hard cuts) | PASS — config + journal intact every time |
| Cold-start offline + induced outage (~30 min) | PASS — resumed on real config, buffered rows replayed in order with real `recorded_at` |
| Streaming 256MB retention eviction | DONE (code + host tests; not HW-exercised — needs a 256-day outage) |

Fault-injection tests were **Supabase-observed** (bench CH340 serial link is
unreliable): `activity_log` boot-summary + `microSD removed/reinserted` rows,
`device_management` heartbeat continuity.

## Optional / deferred

- **24-48h induced-outage soak** — pull the SIM, leave it, reconnect, check the
  drain. Low value on this 0-sensor unit (journal barely grows); all mechanisms
  already proven individually. Do it if you want multi-day heap-leak coverage.
- **Task 1.3** (fully non-blocking WiFi reconnect) — only if this goes
  fleet-wide. Harmless on 107888 (no WiFi creds).
- **OTA-over-WiFi + SD/JSON stack** — unreachable here (OTA is `WiFi.status()`
  gated); check before any WiFi device gets this branch.

## To ship

1. Bench soak (optional, above).
2. Bump `FIRMWARE_VERSION` in `config.h` — same commit as the merge.
3. PR `offline-autonomy` → (after `cellular-fallback` lands) → main.
   Do NOT push/tag/PR without an explicit go-ahead.

## Notes / gotchas

- `[env:native]` needs `test_build_src=true`, `build_src_filter`, and
  `-Itest/native_shim` (a `String`=`std::string` + `strlcpy` shim). Pure logic
  in `persist.cpp` / `journal.cpp` is always-compiled; device code is behind
  `#ifndef UNIT_TEST`.
- Serial capture on this bench: use `scratchpad/readonly.py <port> <secs>`
  (passive, no board reset). `cap.py` toggles RTS = resets the board. The CH340
  link de-enumerates every ~60-90s under load — a different USB cable is the
  likely fix; the device itself is fine throughout.
- `SDFORMAT CONFIRM` serial command reformats the card to FAT32 in place (the
  bench card shipped exFAT). Deinits the task-WDT around the multi-minute
  blocking format.
- `journalEncode` must use `snprintf` for the number, not `s += (long)`.
- `StaticJsonDocument` for the config round-trip is sized 1024.
- Do not push, tag, or PR anything without an explicit go-ahead.
