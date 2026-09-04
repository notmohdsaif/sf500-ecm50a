# Offline Autonomy + Data Retention Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `sf500_107888` run auto-dosing, water-in detection and refill cutoff through any connectivity loss (including a cold power-cut with no network), and buffer all telemetry to its microSD card for in-order replay to Supabase on recovery.

**Architecture:** Split the control plane from the connectivity plane. Control tasks run every loop iteration as long as a configuration (cloud or local) is loaded; connectivity becomes a non-blocking background service behind a single debounced `haveUplink()` signal. Last-known config persists to NVS (dosing scalars) and SD (schedules + mirror); a coarse clock persists to SD. Telemetry is written to an append-only NDJSON journal on SD and drained by one bounded backfill task when the uplink returns.

**Tech Stack:** C++ / Arduino / PlatformIO, ESP32-S3 (Arduino-ESP32 core 2.0.17, `espressif32@6.13.0`), `greiman/SdFat`, `bblanchon/ArduinoJson@^6.21.5`, ESP32 `Preferences` (NVS), Unity (`[env:native]` host tests for pure logic).

**Spec:** `docs/superpowers/specs/2026-09-02-offline-autonomy-design.md`

## Implementation status — 2026-09-03

Rebased onto `cellular-fallback` (tip `80f56a9`) and executed inline. Branch
`offline-autonomy`, ~15 commits past the base. `pio run -e esp32-s3-devkitm-1`
SUCCESS (Flash 31.5% -> 33.4%, RAM +1%); `pio test -e native` 15/15.

**Done:**
- Phase 0 (0.1-0.3): SdFat dep + `[env:native]` (needs `test_build_src`,
  `build_src_filter`, `test/native_shim/Arduino.h`); `src/sdcard.{h,cpp}` on
  FSPI/SPI2 with `sdAtomicWrite`/`sdAppendLine`/`sdReadRange`/
  `sdStreamDropPrefix`; boot self-test.
- Phase 1 (1.1, 1.2, 1.4, 1.6): `src/netstate.{h,cpp}` (RunState, debounced
  `haveUplink`); every cloud.cpp REST fn gated + `noteUplinkResult`;
  `reconnectMQTT` non-blocking; control plane (readSensors / checkRefillCutoff /
  checkAutoDosing / checkSchedules / sensor-upload / clock-snapshot) moved
  ahead of the no-uplink / not-registered early-returns, gated on
  `configLoaded()`. **1.3 deferred** — the cellular down-switch keeps its
  `3x10s` blocking WiFi reconnect; harmless on `sf500_107888` (no WiFi creds =>
  the inner loop is skipped, cellular fallback fires immediately).
  **1.5 satisfied by the rebase** — cellular already made `handlePortalLoop()`
  not early-return.
- Phase 2 (2.2-2.5): `src/persist.{h,cpp}` `#ifndef UNIT_TEST` device section —
  NVS scalars + `/config/device.json`, `/config/schedules.json`, coarse clock
  (`persistClock` NVS-every-call + SD-hourly, `seedClockFromStore`,
  `noteNtpSynced`). `fetchDeviceConfig`/`fetchSchedules` mirror on change;
  `setup()`+`loop()` fall back to local, never touch `startupTime`.
- Phase 3 (3.1-3.5): `src/journal.{h,cpp}` NDJSON encode/decode + host-tested
  `journalDropPrefix` / `journalEvictSensorMetrics`; device-side bounded
  `journalNextBatch` (8KB window), streaming `journalCompact`. All four emit
  points buffer-then-drain when a card is present, `recorded_at` on every row.
  Retention eviction guarded to <=2MB (streaming two-pass = a TODO).
- Phase 4 (4.1-4.3): `docs/migrations/offline-recorded-at.sql` **applied to prod
  2026-09-03** (migration `offline_recorded_at`; nullable `recorded_at` on
  sensor_metrics / activity_log / relay_metrics + PostgREST cache reload);
  `src/backfill.{h,cpp}` bounded transport-aware drain, wired into `loop()`.
- **Task 0.4 (SD bench verification) — DONE 2026-09-03 on sf500_107888.** Pin
  map SCK10/MISO9/MOSI46/CS1/CD3 correct; mounts first try at 20 MHz
  SHARED_SPI; CD polarity confirmed (GPIO3 LOW = card seated). The bench card
  shipped exFAT (cardBegin OK, volumeBegin fails, sdErrorCode 0) — added
  `sdFormatFat32()` + `SDFORMAT CONFIRM` serial command (reuses the probe's
  card object; deinits the task-WDT around the multi-minute blocking format).
  Post-format: FAT32, 15185 MB free, clean reboot mounts first try, and rows
  buffered pre-cellular drained to Supabase on reconnect (recorded_at path
  verified live). Firmware flashed to the unit is `dee3c20`.

**Phase 5 — in progress (Supabase-observed; bench CH340 serial link is
unreliable so fault-injection results are read from `activity_log` +
`device_management` heartbeat instead of a serial console):**
- **Stack high-water — PASS (2026-09-03).** loopTask floor 13.9 KB free of
  20480 across every TLS/SD/JSON path this cellular device runs (cellular
  software-mbedTLS every 10s, MQTT-over-cell, SdFat journal read + JSON decode
  in backfill). Peak use ~6.5 KB — nowhere near the v1.2.5 crash class. OTA
  path is `WiFi.status()`-gated (`ota.cpp:36`) so it never runs here; that
  combination (OTA-over-WiFi + SD/JSON) is unverified on this branch and
  should be checked before any WiFi device gets it.
- **Card-pull fault injection — PASS (2026-09-04).** Pulled 01:26:44 UTC,
  reinserted 01:27:19. `activity_log` got `microSD removed — buffering paused`
  (direct POST, journal skipped since `mounted` went false) then `microSD
  reinserted — remounted` (buffered + drained in 4s). Zero "Device booted"
  lines in the window = no crash/reboot/watchdog; heartbeat 30s cadence
  unbroken, `status` stayed `online`. `sdTick()` hot-remove/reinsert works.
- **Boot summary instrumentation verified live (2026-09-04).** `activity_log`
  row `boot summary: cfg=loaded ecTarget=1.50 autoDosing=0 mixing=0
  dosingTime=60 schedules=0 journalPendingB=194 clock=ntp` matched
  `device_management` exactly — the NVS/SD mirror holds real persisted config,
  not code defaults (dosingTime=60 != the 30 default). Arrived via the journal
  buffer-then-drain path with a correct `recorded_at`.

**Not done (Phase 5 remainder):**
- Power-cut-during-write proof (atomic write via temp+rename survives; journal
  last-line torn-write rejected by `journalDecode`, replay continues).
- Cold-start fully offline: provision online, power off, pull 4G antenna,
  power on — confirm it runs on real persisted config with no network, then
  restore and confirm the buffered boot summary drains with `clock=approx`.
- 24-48h induced-outage soak (pull 4G antenna), then reconnect and confirm
  in-order drain, nothing lost, no reboot.
- Streaming two-pass retention eviction at the real 256MB cap.
- Task 1.3 (fully non-blocking WiFi reconnect) if this ever goes fleet-wide.

**Firmware currently on sf500_107888:** branch tip (commit after "Move boot
summary into bringOnline()"). Still `FIRMWARE_VERSION` 1.2.5 (bench build, not
a release).

## Global Constraints

- **Device scope: `sf500_107888` only.** No fleet rollout, no dashboard UI, no second device in any test.
- **Base branch:** `offline-autonomy`, cut from `main` @ v1.2.5. **Before any code task**, rebase onto `cellular-fallback` once that branch's in-flight work is committed and stable, and re-read `docs/superpowers/specs/2026-09-02-cellular-fallback-design.md` plus its `src/cellular.*` code. `RunState` must compose with the cellular branch's `activeTransport` / `NetworkTransport`, not replace it. `haveUplink()` must fold in cellular data state.
- **All line numbers below are as of v1.2.5 (`main` @ `96b2773`).** Re-locate against the rebased base.
- **Stack budget:** `loopTask` is 20480 bytes (`getArduinoLoopTaskStackSize()` in `main.cpp:160`, plus `board_build.arduino.loopTaskStackSize = 16384` in `platformio.ini` — the function wins). SD + JSON + mbedTLS all run on this task. Measure high-water before claiming any phase done. This is the v1.2.5 crash class.
- **No blocking call in `loop()` may approach the 60 s task watchdog** (`esp_task_wdt_init(60, true)` in `main.cpp:294`). Backfill batches stay small with short HTTP timeouts.
- **Firmware version:** bump `FIRMWARE_VERSION` in `config.h:113` only when the requester asks for a release build; this is bench work on a branch.
- **Card:** 16 GB microSD, FAT32.
- **Commit discipline:** one commit per task minimum, TDD order (failing test → run → implement → pass → commit) for host-testable tasks; bench-procedure tasks commit the code then record the serial transcript in the task's verification note.
- **Attribution:** end commit messages with the two trailer lines from the session attribution notice.

---

## File Structure

**New files:**
- `src/sdcard.h` / `src/sdcard.cpp` — SPI2 microSD mount, presence (CD pin), free space, atomic file write, line append. One responsibility: raw SD I/O with power-loss-safe primitives.
- `src/netstate.h` / `src/netstate.cpp` — `RunState`, `g_uplinkOk`, `haveUplink()`, `noteUplinkResult(bool)`, `noteMqttState(bool)`. One responsibility: the single connectivity/run-state signal.
- `src/persist.h` / `src/persist.cpp` — config → NVS scalars, config + schedules → SD JSON, load-order resolver, coarse-clock persist/seed. One responsibility: local persistence of configuration and clock.
- `src/journal.h` / `src/journal.cpp` — NDJSON record encode/decode, append, read-batch-from-offset, offset advance, compaction/rotation, retention eviction. One responsibility: the telemetry journal.
- `src/backfill.h` / `src/backfill.cpp` — the bounded drain task that POSTs journalled rows to Supabase. One responsibility: replay.
- `test/test_journal/test_journal.cpp` — Unity host tests for `journal` encode/decode, offset math, compaction, retention.
- `test/test_persist/test_persist.cpp` — Unity host tests for config JSON round-trip and clock-seed logic.
- `docs/migrations/offline-recorded-at.sql` — the `recorded_at` column migration record.

**Modified files:**
- `platformio.ini` — add `greiman/SdFat`, add `[env:native]` with Unity.
- `src/globals.h` — include `netstate.h`; no new globals here (they live in the new modules).
- `src/main.cpp` — `setup()` SD mount + boot self-test + load-order; `loop()` decoupling (portal co-task, non-blocking WiFi reconnect, run control tasks unconditionally, call backfill drain), clock persist tick.
- `src/mqtt_handler.cpp` — `reconnectMQTT()` non-blocking; `noteMqttState()` calls; `haveUplink()` gate before the `relay_metrics` / `logR3Transition` POSTs.
- `src/cloud.cpp` — `haveUplink()` gate + `noteUplinkResult()` on every REST call; `fetchDeviceConfig()` / `fetchSchedules()` persist-on-change; `logDeviceActivity()` / `uploadSensorReadings()` route through the journal; `recorded_at` in inserts.
- `src/relay.cpp` — `writeRelay()` `relay_metrics` POST routes through the journal + `haveUplink()` gate.
- `src/sensors.cpp` — no logic change; `logDeviceActivity()` calls inside `checkAutoDosing()` now journal transparently (no edit needed if the function signature is unchanged).

---

## PHASE 0 — SD card bring-up

### Task 0.1: Add SdFat dependency and a native test env

**Files:**
- Modify: `platformio.ini`

**Interfaces:**
- Produces: buildable `[env:esp32-s3-devkitm-1]` with `SdFat` available; runnable `[env:native]` with Unity.

- [ ] **Step 1: Add the dependency and native env**

Edit `platformio.ini`:

```ini
[env:esp32-s3-devkitm-1]
platform = espressif32@6.13.0
board = esp32-s3-devkitm-1
framework = arduino
board_build.arduino.loopTaskStackSize = 16384
board_upload.flash_size = 8MB
board_build.partitions = default_8MB.csv
lib_deps =
    knolleary/PubSubClient
    bblanchon/ArduinoJson @ ^6.21.5
    4-20ma/ModbusMaster
    greiman/SdFat @ ^2.2.2

[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -DUNIT_TEST
lib_deps =
    bblanchon/ArduinoJson @ ^6.21.5
```

- [ ] **Step 2: Verify the firmware env still builds**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`, `SdFat` fetched.

- [ ] **Step 3: Verify the native env runs (no tests yet)**

Run: `~/.platformio/penv/bin/pio test -e native`
Expected: `No tests found` or an empty pass — the env resolves and compiles.

- [ ] **Step 4: Commit**

```bash
git add platformio.ini
git commit -m "Add SdFat dependency and native Unity test env"
```

### Task 0.2: `sdcard` module — mount, presence, atomic write, append

**Files:**
- Create: `src/sdcard.h`, `src/sdcard.cpp`

**Interfaces:**
- Produces:
  - `bool sdInit();` — mount microSD on SPI2 (SCK=10, MISO=9, MOSI=46, CS=1) at 20 MHz; `false` if no card or mount fails. Idempotent.
  - `bool sdMounted();`
  - `bool sdCardDetect();` — raw read of GPIO3 (polarity confirmed in Task 0.4).
  - `uint64_t sdFreeBytes();`
  - `bool sdAtomicWrite(const char* path, const uint8_t* data, size_t len);` — writes `path + ".tmp"`, flush + sync, then `rename` over `path`. `false` on any failure (original untouched).
  - `bool sdAppendLine(const char* path, const String& line);` — single `O_APPEND` write of `line` + `'\n'`. `false` on failure.
  - `bool sdReadFile(const char* path, String& out);`
  - `size_t sdFileSize(const char* path);`

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include <Arduino.h>

#define SD_SPI_SCK   10
#define SD_SPI_MISO  9
#define SD_SPI_MOSI  46
#define SD_CS_PIN    1
#define SD_CD_PIN    3
#define SD_SPI_HZ    (20UL * 1000UL * 1000UL)

bool     sdInit();
bool     sdMounted();
bool     sdCardDetect();
uint64_t sdFreeBytes();
bool     sdAtomicWrite(const char* path, const uint8_t* data, size_t len);
bool     sdAppendLine(const char* path, const String& line);
bool     sdReadFile(const char* path, String& out);
size_t   sdFileSize(const char* path);
```

- [ ] **Step 2: Implement `sdcard.cpp`**

```cpp
#include "sdcard.h"
#include "logger.h"
#include <SPI.h>
#include <SdFat.h>

static SdFat32   sd;
static SPIClass  sdSpi(HSPI);
static bool      mounted = false;

bool sdInit() {
  if (mounted) return true;
  pinMode(SD_CD_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_CS_PIN);
  SdSpiConfig cfg(SD_CS_PIN, SHARED_SPI, SD_SPI_HZ, &sdSpi);
  mounted = sd.begin(cfg);
  if (mounted) {
    sd.mkdir("/config"); sd.mkdir("/state"); sd.mkdir("/buffer");
    LOGF("[SD] mounted, free %llu MB\n", sdFreeBytes() / (1024ULL * 1024ULL));
  } else {
    LOGLN("[SD] mount failed / no card");
  }
  return mounted;
}

bool sdMounted()      { return mounted; }
bool sdCardDetect()   { return digitalRead(SD_CD_PIN) == LOW; }  // polarity re-checked in 0.4

uint64_t sdFreeBytes() {
  if (!mounted) return 0;
  return (uint64_t)sd.vol()->freeClusterCount() * sd.vol()->sectorsPerCluster() * 512ULL;
}

bool sdAtomicWrite(const char* path, const uint8_t* data, size_t len) {
  if (!mounted) return false;
  String tmp = String(path) + ".tmp";
  File32 f = sd.open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  bool ok = f.write(data, len) == (int)len;
  f.sync();
  f.close();
  if (!ok) { sd.remove(tmp.c_str()); return false; }
  sd.remove(path);
  return sd.rename(tmp.c_str(), path);
}

bool sdAppendLine(const char* path, const String& line) {
  if (!mounted) return false;
  File32 f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)line.c_str(), line.length());
  n += f.write((const uint8_t*)"\n", 1);
  f.sync();
  f.close();
  return n == line.length() + 1;
}

bool sdReadFile(const char* path, String& out) {
  if (!mounted) return false;
  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;
  out = "";
  while (f.available()) out += (char)f.read();
  f.close();
  return true;
}

size_t sdFileSize(const char* path) {
  if (!mounted) return 0;
  File32 f = sd.open(path, O_RDONLY);
  if (!f) return 0;
  size_t s = f.fileSize();
  f.close();
  return s;
}
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/sdcard.h src/sdcard.cpp
git commit -m "Add sdcard module: SPI2 mount + power-loss-safe write/append"
```

### Task 0.3: Boot self-test wiring

**Files:**
- Modify: `src/main.cpp` (`#include "sdcard.h"` near line 12; call in `setup()` after `Serial.begin` block, before WiFi init ~line 181)

**Interfaces:**
- Consumes: `sdInit()`, `sdFreeBytes()`, `sdCardDetect()` from Task 0.2.

- [ ] **Step 1: Add the include and self-test**

In `setup()`, immediately after the `LOGF("[DIAG] Reset: ...` line (`main.cpp:169`):

```cpp
  // --- microSD (offline buffering + config persistence) ---
  if (sdInit()) {
    LOGF("[SD] card present (CD=%d), free %llu MB\n",
         sdCardDetect(), sdFreeBytes() / (1024ULL * 1024ULL));
  } else {
    LOGLN("[SD] unavailable — offline buffering + schedule persistence disabled");
  }
```

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Mount microSD at boot with a self-test log line"
```

### Task 0.4: Bench verification — SD hardware

**Files:** none (verification only; record the transcript in this task's checkbox note).

- [ ] **Step 1: Flash and monitor**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1 -t upload && ~/.platformio/penv/bin/pio device monitor -b 9600`

- [ ] **Step 2: Confirm mount**

Expected serial: `[SD] mounted, free <N> MB` where N ≈ 15000 for a 16 GB card, then `[SD] card present (CD=...)`.

- [ ] **Step 3: Confirm CD polarity**

Eject the card, press reset. Expected: `[SD] mount failed / no card` and `[SD] unavailable — ...`. Note the `CD=` value with card in vs out; if `sdCardDetect()` reported backwards, flip the comparison in `sdcard.cpp` and re-flash. Record the correct polarity here.

- [ ] **Step 4: Confirm reseat recovery**

Re-insert card, press reset. Expected: mount succeeds again. (`sdInit()` is only called at boot for now — hot-insert without reboot is not required.)

---

## PHASE 1 — Control / connectivity decoupling

> Rebase onto `cellular-fallback` first (Global Constraints). Every task below composes with `activeTransport`.

### Task 1.1: `netstate` module — run-state and the uplink signal

**Files:**
- Create: `src/netstate.h`, `src/netstate.cpp`
- Modify: `src/globals.h` (add `#include "netstate.h"` after the other project includes ~line 20)

**Interfaces:**
- Produces:
  - `enum RunState { RS_BOOT, RS_ONLINE, RS_OFFLINE_AUTONOMOUS, RS_PROVISIONING };`
  - `extern RunState g_runState;`
  - `void setRunState(RunState s);` — logs the transition once.
  - `bool haveUplink();` — returns the debounced `g_uplinkOk`.
  - `void noteUplinkResult(bool ok);` — call after every Supabase REST attempt. `ok` immediately sets `g_uplinkOk=true`; `!ok` sets it false only after `UPLINK_FAIL_DEBOUNCE` (2) consecutive failures.
  - `void noteMqttState(bool connected);` — a connect sets `g_uplinkOk=true`; a disconnect is advisory only (does not by itself clear the signal — a Supabase call result is authoritative).
  - `bool configLoaded();` / `void setConfigLoaded();` — true once cloud or local config has populated the dosing globals.

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include <Arduino.h>

#define UPLINK_FAIL_DEBOUNCE 2

enum RunState { RS_BOOT, RS_ONLINE, RS_OFFLINE_AUTONOMOUS, RS_PROVISIONING };
extern RunState g_runState;

void setRunState(RunState s);
bool haveUplink();
void noteUplinkResult(bool ok);
void noteMqttState(bool connected);
bool configLoaded();
void setConfigLoaded();
```

- [ ] **Step 2: Implement**

```cpp
#include "netstate.h"
#include "logger.h"

RunState g_runState = RS_BOOT;
static bool     uplinkOk    = false;
static uint8_t  failStreak  = 0;
static bool     cfgLoaded   = false;

static const char* rsName(RunState s) {
  switch (s) {
    case RS_BOOT: return "BOOT";
    case RS_ONLINE: return "ONLINE";
    case RS_OFFLINE_AUTONOMOUS: return "OFFLINE_AUTONOMOUS";
    case RS_PROVISIONING: return "PROVISIONING";
  }
  return "?";
}

void setRunState(RunState s) {
  if (s == g_runState) return;
  LOGF("[RUN] %s -> %s\n", rsName(g_runState), rsName(s));
  g_runState = s;
}

bool haveUplink() { return uplinkOk; }

void noteUplinkResult(bool ok) {
  if (ok) { uplinkOk = true; failStreak = 0; return; }
  if (failStreak < 255) failStreak++;
  if (failStreak >= UPLINK_FAIL_DEBOUNCE) uplinkOk = false;
}

void noteMqttState(bool connected) {
  if (connected) { uplinkOk = true; failStreak = 0; }
}

bool configLoaded()    { return cfgLoaded; }
void setConfigLoaded() { cfgLoaded = true; }
```

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/netstate.h src/netstate.cpp src/globals.h
git commit -m "Add netstate module: RunState + debounced haveUplink signal"
```

### Task 1.2: Gate every Supabase call behind `haveUplink()`

**Files:**
- Modify: `src/cloud.cpp` — `registerDevice`, `uploadSensorConfig`, `uploadSensorReadings`, `updateDeviceStatus`, `fetchRefillTankMax`, `fetchDeviceConfig`, `fetchSchedules`, `logDeviceActivity`
- Modify: `src/relay.cpp:28` — the `relay_metrics` POST in `writeRelay()`
- Modify: `src/mqtt_handler.cpp` — `logR3Transition` (`mqtt_handler.cpp:404`)

**Interfaces:**
- Consumes: `haveUplink()`, `noteUplinkResult(bool)` from Task 1.1.

- [ ] **Step 1: Add the guard + result note to each REST function**

Pattern for a *fetch* (early-return keeps stale globals):

```cpp
void fetchDeviceConfig() {
  if (!haveUplink()) return;
  HTTPClient http;
  ...
  int code = http.GET();
  noteUplinkResult(code == 200);
  if (code != 200) { http.end(); return; }
  ...
}
```

Pattern for a *push* that Task 3 will re-route (leave the direct POST for now, just guard):

```cpp
void logDeviceActivity(const char *category, const char *action) {
  if (!isRegistered || deviceName.isEmpty()) return;
  if (!haveUplink()) return;   // Task 3.3 replaces this line with a journal append
  ...
  int code = http.POST(payload);
  noteUplinkResult(code >= 200 && code < 300);
  http.end();
}
```

Apply the same `if (!haveUplink()) return;` + `noteUplinkResult(...)` to every function listed under Files. For `writeRelay()` the existing `if (WiFi.status() == WL_CONNECTED)` block becomes `if (haveUplink())`.

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Bench check — no TLS stall when offline**

Flash, let it go online, then pull the WiFi antenna / `CELLAPN -` + disconnect. Watch serial: after `haveUplink` clears, `[CONFIG]` / activity lines should stop attempting HTTP (no multi-second gaps between `loop` sensor reads). Record loop cadence before/after.

- [ ] **Step 4: Commit**

```bash
git add src/cloud.cpp src/relay.cpp src/mqtt_handler.cpp
git commit -m "Gate all Supabase REST calls behind haveUplink()"
```

### Task 1.3: Non-blocking WiFi reconnect

**Files:**
- Modify: `src/main.cpp:343-387` (the `if (WiFi.status() != WL_CONNECTED)` block in `loop()`)

**Interfaces:**
- Consumes: `setRunState`, `haveUplink`.
- Produces: no new symbols; behavioural change only.

- [ ] **Step 1: Replace the blocking reconnect with a timed single attempt**

```cpp
  // --- WiFi reconnect (non-blocking) ---
  static unsigned long lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED
      && activeTransport != TRANSPORT_CELLULAR
      && now - lastWifiTry >= 15000UL) {
    lastWifiTry = now;
    wifiPrefs.begin("wifi", true);
    String s = wifiPrefs.getString("ssid", "");
    String p = wifiPrefs.getString("pass", "");
    wifiPrefs.end();
    if (s.length()) {
      LOGLN("[WiFi] link down — single reconnect attempt");
      WiFi.disconnect(false);
      WiFi.begin(s.c_str(), p.c_str());
    }
  }
```

Delete the old 3×10s loop and the `startWiFiPortal(); return;` on failure. Do **not** open the portal here (Task 1.5 governs portal entry).

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Bench check — loop keeps ticking during WiFi loss**

Flash, online, then kill the AP. Expected: serial keeps printing sensor reads every ~1 s with no 30 s freeze; a `[WiFi] link down — single reconnect attempt` line every 15 s.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Make WiFi reconnect non-blocking (one timed attempt per pass)"
```

### Task 1.4: Non-blocking MQTT reconnect

**Files:**
- Modify: `src/mqtt_handler.cpp:16-59` (`reconnectMQTT`), `src/main.cpp:414-416` (the `if (!mqttClient.connected()) reconnectMQTT();` call site)

**Interfaces:**
- Consumes: `noteMqttState(bool)`.
- Produces: `reconnectMQTT()` returns after at most one `connect()` attempt.

- [ ] **Step 1: Rewrite `reconnectMQTT()` as a single timed attempt**

```cpp
void reconnectMQTT() {
  static unsigned long lastTry = 0;
  if (mqttClient.connected()) return;
  if (millis() - lastTry < 5000UL) return;
  lastTry = millis();

  String clientId = "SF500_" + lastSix;
  if (!mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    noteMqttState(false);
    return;
  }
  noteMqttState(true);
  mqttClient.subscribe(topicRelayUpdate.c_str());
  mqttClient.subscribe(topicWifiCmd.c_str());
  mqttClient.subscribe(topicDeviceCmd.c_str());
  if (!plugUseHttp && tasmotaPlugEnabled && tasmotaPlugTopic.length() > 0) {
    mqttClient.subscribe(("stat/" + tasmotaPlugTopic + "/POWER").c_str());
    mqttClient.publish(("cmnd/" + tasmotaPlugTopic + "/Power").c_str(), "");
  }
  publishRelayStatus();
  if (WiFi.status() == WL_CONNECTED) { /* existing wifi-info publish block, unchanged */ }
  LOGLN("MQTT connected");
}
```

Remove the `for (int i = 0; i < 5 ...)` wrapper and the inner `for (w...) { checkRelayTimers(); delay(200); }` busy-wait.

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Bench check**

Kill only the broker route (keep LAN). Expected: no `loop()` stall; a reconnect attempt every 5 s; control tasks unaffected.

- [ ] **Step 4: Commit**

```bash
git add src/mqtt_handler.cpp src/main.cpp
git commit -m "Make MQTT reconnect non-blocking (one timed attempt per call)"
```

### Task 1.5: Portal no longer gates control; not auto-opened on outage

**Files:**
- Modify: `src/main.cpp:310-341` (portal branch + one-time init), `src/main.cpp:389-393` (`if (!isRegistered) { delay(1000); return; }`)

**Interfaces:**
- Consumes: `setRunState`, `configLoaded`.
- Produces: `loop()` reaches the periodic-task block in `RS_OFFLINE_AUTONOMOUS` and while the portal is open.

- [ ] **Step 1: Portal becomes a co-task**

Replace the portal branch:

```cpp
  if (portalMode) {
    handlePortalLoop();
    setRunState(RS_PROVISIONING);
    tryCellularFallback();               // from the cellular-fallback branch
    if (!configLoaded()) return;         // nothing to run yet — wait for config
    // else fall through: run control tasks even while the portal AP is up
  }
```

- [ ] **Step 2: Drop the `!isRegistered` hard return**

Replace `main.cpp:389-393` with:

```cpp
  if (!isRegistered && !configLoaded()) {
    delay(200);
    return;
  }
```

- [ ] **Step 3: Confirm no other code path calls `startWiFiPortal()` on connectivity loss**

`grep -n startWiFiPortal src/`. Allowed callers: no-saved-creds boot path, `pendingWifiPortal` (MQTT `wifi_cmd: portal`), and a `KEY_BOOT` long-press (add in Task 1.6 if not already present on the rebased base). Remove any outage-triggered call.

- [ ] **Step 4: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Portal becomes a co-task; never auto-opened on connectivity loss"
```

### Task 1.6: Control tasks run unconditionally when config is loaded

**Files:**
- Modify: `src/main.cpp:459-467` (the `SENSOR_READ_INTERVAL` block) and the surrounding periodic-task section

**Interfaces:**
- Consumes: `configLoaded()`, `haveUplink()`, `g_runState`.

- [ ] **Step 1: Split periodic tasks into control vs connectivity**

Ensure this block runs regardless of `haveUplink()` / `isRegistered`, gated only on `configLoaded()`:

```cpp
  if (configLoaded() && now - lastSensorRead >= SENSOR_READ_INTERVAL) {
    readSensors();
    lastSensorRead = now;
    checkRefillCutoff();
    if (autoDosing && ecSensorFound) checkAutoDosing();
  }
  if (configLoaded() && now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
    checkSchedules();
    lastScheduleCheck = now;
  }
```

The `uploadSensorReadings`, `updateDeviceStatus`, `fetchDeviceConfig`, `fetchSchedules`, `checkForOTAUpdate` calls stay as-is — they already no-op offline via Task 1.2, and become journal-backed in Phase 3.

Set `RS_ONLINE` / `RS_OFFLINE_AUTONOMOUS` at the top of the periodic block based on `haveUplink()`.

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Bench check — dosing survives a full blackout**

Flash, let auto-dosing reach `AUTO_SAMPLING` (serial), then kill WiFi and cellular. Expected: state machine keeps advancing; `[RUN] ONLINE -> OFFLINE_AUTONOMOUS`; R1/R2 fire on EC demand; `checkRefillCutoff` still reacts to WL. Restore connectivity → `[RUN] OFFLINE_AUTONOMOUS -> ONLINE`.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Run control tasks whenever a config is loaded, online or not"
```

---

## PHASE 2 — Local configuration persistence

### Task 2.1: Config JSON round-trip (host-tested)

**Files:**
- Create: `src/persist.h`, `src/persist.cpp`
- Create: `test/test_persist/test_persist.cpp`

**Interfaces:**
- Produces:
  - `struct LocalConfig { bool autoDosing; float ecTarget; uint32_t dosingTime; bool autoMixing; bool smartDosing; uint32_t minWlDosing; char plugMode[12]; float refillCutoffMm; bool plugEnabled; char plugTopic[48]; char plugHost[40]; uint8_t ecSensorId, wlSensorId, ambSensorId, rainSensorId; bool ecFound, wlFound, ambFound, rainFound; int lastRainResetDay; };`
  - `String configToJson(const LocalConfig& c);`
  - `bool configFromJson(const String& json, LocalConfig& c);` — `false` on parse error or missing required key (`ecTarget`, `dosingTime`).

- [ ] **Step 1: Write the failing test**

```cpp
#include <unity.h>
#include "persist.h"

void test_config_roundtrip() {
  LocalConfig a{};
  a.autoDosing = true; a.ecTarget = 1.85f; a.dosingTime = 45;
  a.autoMixing = true; a.smartDosing = false; a.minWlDosing = 120;
  strcpy(a.plugMode, "refill"); a.refillCutoffMm = 380.0f;
  a.plugEnabled = true; strcpy(a.plugTopic, "sf500_107888_plug");
  a.ecSensorId = 3; a.wlSensorId = 13; a.ecFound = true; a.wlFound = true;
  a.lastRainResetDay = 2;

  LocalConfig b{};
  TEST_ASSERT_TRUE(configFromJson(configToJson(a), b));
  TEST_ASSERT_EQUAL(a.autoDosing, b.autoDosing);
  TEST_ASSERT_FLOAT_WITHIN(0.001, a.ecTarget, b.ecTarget);
  TEST_ASSERT_EQUAL(a.dosingTime, b.dosingTime);
  TEST_ASSERT_EQUAL_STRING(a.plugMode, b.plugMode);
  TEST_ASSERT_FLOAT_WITHIN(0.01, a.refillCutoffMm, b.refillCutoffMm);
  TEST_ASSERT_EQUAL(a.wlSensorId, b.wlSensorId);
  TEST_ASSERT_EQUAL(a.lastRainResetDay, b.lastRainResetDay);
}

void test_config_rejects_missing_required() {
  LocalConfig b{};
  TEST_ASSERT_FALSE(configFromJson("{\"autoDosing\":true}", b));
}

void test_config_rejects_garbage() {
  LocalConfig b{};
  TEST_ASSERT_FALSE(configFromJson("not json", b));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_config_roundtrip);
  RUN_TEST(test_config_rejects_missing_required);
  RUN_TEST(test_config_rejects_garbage);
  return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL (link error, `persist.h` missing)**

Run: `~/.platformio/penv/bin/pio test -e native -f test_persist`
Expected: FAIL.

- [ ] **Step 3: Implement `persist.h` / `persist.cpp` (this task: only the struct + the two JSON functions)**

```cpp
// persist.cpp — configToJson / configFromJson
#include "persist.h"
#include <ArduinoJson.h>

String configToJson(const LocalConfig& c) {
  StaticJsonDocument<640> d;
  d["autoDosing"]=c.autoDosing; d["ecTarget"]=c.ecTarget; d["dosingTime"]=c.dosingTime;
  d["autoMixing"]=c.autoMixing; d["smartDosing"]=c.smartDosing; d["minWlDosing"]=c.minWlDosing;
  d["plugMode"]=c.plugMode; d["refillCutoffMm"]=c.refillCutoffMm; d["plugEnabled"]=c.plugEnabled;
  d["plugTopic"]=c.plugTopic; d["plugHost"]=c.plugHost;
  d["ecSensorId"]=c.ecSensorId; d["wlSensorId"]=c.wlSensorId;
  d["ambSensorId"]=c.ambSensorId; d["rainSensorId"]=c.rainSensorId;
  d["ecFound"]=c.ecFound; d["wlFound"]=c.wlFound; d["ambFound"]=c.ambFound; d["rainFound"]=c.rainFound;
  d["lastRainResetDay"]=c.lastRainResetDay;
  String s; serializeJson(d, s); return s;
}

bool configFromJson(const String& json, LocalConfig& c) {
  StaticJsonDocument<640> d;
  if (deserializeJson(d, json) != DeserializationError::Ok) return false;
  if (!d.containsKey("ecTarget") || !d.containsKey("dosingTime")) return false;
  c.autoDosing=d["autoDosing"]|false; c.ecTarget=d["ecTarget"]|1.5f;
  c.dosingTime=d["dosingTime"]|30; c.autoMixing=d["autoMixing"]|false;
  c.smartDosing=d["smartDosing"]|false; c.minWlDosing=d["minWlDosing"]|0;
  strlcpy(c.plugMode, d["plugMode"]|"custom", sizeof(c.plugMode));
  c.refillCutoffMm=d["refillCutoffMm"]|0.0f; c.plugEnabled=d["plugEnabled"]|false;
  strlcpy(c.plugTopic, d["plugTopic"]|"", sizeof(c.plugTopic));
  strlcpy(c.plugHost, d["plugHost"]|"", sizeof(c.plugHost));
  c.ecSensorId=d["ecSensorId"]|0; c.wlSensorId=d["wlSensorId"]|0;
  c.ambSensorId=d["ambSensorId"]|0; c.rainSensorId=d["rainSensorId"]|0;
  c.ecFound=d["ecFound"]|false; c.wlFound=d["wlFound"]|false;
  c.ambFound=d["ambFound"]|false; c.rainFound=d["rainFound"]|false;
  c.lastRainResetDay=d["lastRainResetDay"]|-1;
  return true;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `~/.platformio/penv/bin/pio test -e native -f test_persist`
Expected: 3 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/persist.h src/persist.cpp test/test_persist/test_persist.cpp
git commit -m "Add LocalConfig struct + JSON round-trip (host-tested)"
```

### Task 2.2: Persist config to NVS + SD on change

**Files:**
- Modify: `src/persist.cpp` / `src/persist.h` — add `persistConfig`, `loadConfigLocal`
- Modify: `src/cloud.cpp` `fetchDeviceConfig()` — call `persistConfig()` when `changed`

**Interfaces:**
- Consumes: `sdAtomicWrite`, `sdReadFile` (Task 0.2); `configToJson`/`configFromJson` (Task 2.1).
- Produces:
  - `void snapshotGlobalsToConfig(LocalConfig& c);` — fill from the live dosing globals.
  - `void applyConfigToGlobals(const LocalConfig& c);`
  - `bool persistConfig();` — snapshot globals → write NVS scalars (namespace `"cfg"`) + `sdAtomicWrite("/config/device.json", ...)`. Returns `true` if at least NVS succeeded.
  - `bool loadConfigLocal();` — read NVS scalars (fallback to `/config/device.json`), `applyConfigToGlobals`, `setConfigLoaded()`. `false` if neither source has a usable `ecTarget`.

- [ ] **Step 1: Implement `persistConfig` / `loadConfigLocal`**

```cpp
#include "globals.h"
#include "sdcard.h"
#include "netstate.h"
#include <Preferences.h>
static Preferences cfgnvs;

void snapshotGlobalsToConfig(LocalConfig& c) {
  c.autoDosing=autoDosing; c.ecTarget=ecTarget; c.dosingTime=dosingTime;
  c.autoMixing=autoMixing; c.smartDosing=smartDosing; c.minWlDosing=minWlDosing;
  strlcpy(c.plugMode, plugMode.c_str(), sizeof(c.plugMode));
  c.refillCutoffMm=refillCutoffMm; c.plugEnabled=tasmotaPlugEnabled;
  strlcpy(c.plugTopic, tasmotaPlugTopic.c_str(), sizeof(c.plugTopic));
  strlcpy(c.plugHost, plugHttpHost.c_str(), sizeof(c.plugHost));
  c.ecSensorId=ecSensorId; c.wlSensorId=wlSensorId;
  c.ambSensorId=ambSensorId; c.rainSensorId=rainSensorId;
  c.ecFound=ecSensorFound; c.wlFound=wlSensorFound;
  c.ambFound=ambSensorFound; c.rainFound=rainSensorFound;
  c.lastRainResetDay=lastRainResetDay;
}

void applyConfigToGlobals(const LocalConfig& c) {
  autoDosing=c.autoDosing; ecTarget=c.ecTarget; ecMinusHys=ecTarget-EC_HYSTERESIS;
  dosingTime=c.dosingTime; autoMixing=c.autoMixing; smartDosing=c.smartDosing;
  minWlDosing=c.minWlDosing; plugMode=c.plugMode; refillCutoffMm=c.refillCutoffMm;
  tasmotaPlugEnabled=c.plugEnabled; tasmotaPlugTopic=c.plugTopic; plugHttpHost=c.plugHost;
  ecSensorId=c.ecSensorId; wlSensorId=c.wlSensorId;
  ambSensorId=c.ambSensorId; rainSensorId=c.rainSensorId;
  ecSensorFound=c.ecFound; wlSensorFound=c.wlFound;
  ambSensorFound=c.ambFound; rainSensorFound=c.rainFound;
  lastRainResetDay=c.lastRainResetDay;
  plugUseHttp = tasmotaPlugEnabled && plugHttpHost.length() > 0;
}

bool persistConfig() {
  LocalConfig c{}; snapshotGlobalsToConfig(c);
  cfgnvs.begin("cfg", false);
  cfgnvs.putBool("autoDosing", c.autoDosing);
  cfgnvs.putFloat("ecTarget", c.ecTarget);
  cfgnvs.putUInt("dosingTime", c.dosingTime);
  cfgnvs.putBool("autoMixing", c.autoMixing);
  cfgnvs.putBool("smartDosing", c.smartDosing);
  cfgnvs.putUInt("minWlDosing", c.minWlDosing);
  cfgnvs.putString("plugMode", c.plugMode);
  cfgnvs.putFloat("refillCut", c.refillCutoffMm);
  cfgnvs.putBool("plugEnabled", c.plugEnabled);
  cfgnvs.putString("plugTopic", c.plugTopic);
  cfgnvs.putString("plugHost", c.plugHost);
  cfgnvs.putUChar("ecId", c.ecSensorId); cfgnvs.putUChar("wlId", c.wlSensorId);
  cfgnvs.putUChar("ambId", c.ambSensorId); cfgnvs.putUChar("rainId", c.rainSensorId);
  cfgnvs.putUChar("found", (c.ecFound?1:0)|(c.wlFound?2:0)|(c.ambFound?4:0)|(c.rainFound?8:0));
  cfgnvs.putInt("rainDay", c.lastRainResetDay);
  cfgnvs.end();
  String js = configToJson(c);
  sdAtomicWrite("/config/device.json", (const uint8_t*)js.c_str(), js.length());
  return true;
}

bool loadConfigLocal() {
  LocalConfig c{};
  cfgnvs.begin("cfg", true);
  bool haveNvs = cfgnvs.isKey("ecTarget");
  if (haveNvs) {
    c.autoDosing=cfgnvs.getBool("autoDosing", false);
    c.ecTarget=cfgnvs.getFloat("ecTarget", 1.5f);
    c.dosingTime=cfgnvs.getUInt("dosingTime", 30);
    c.autoMixing=cfgnvs.getBool("autoMixing", false);
    c.smartDosing=cfgnvs.getBool("smartDosing", false);
    c.minWlDosing=cfgnvs.getUInt("minWlDosing", 0);
    cfgnvs.getString("plugMode", c.plugMode, sizeof(c.plugMode));
    c.refillCutoffMm=cfgnvs.getFloat("refillCut", 0.0f);
    c.plugEnabled=cfgnvs.getBool("plugEnabled", false);
    cfgnvs.getString("plugTopic", c.plugTopic, sizeof(c.plugTopic));
    cfgnvs.getString("plugHost", c.plugHost, sizeof(c.plugHost));
    c.ecSensorId=cfgnvs.getUChar("ecId", 0); c.wlSensorId=cfgnvs.getUChar("wlId", 0);
    c.ambSensorId=cfgnvs.getUChar("ambId", 0); c.rainSensorId=cfgnvs.getUChar("rainId", 0);
    uint8_t f=cfgnvs.getUChar("found", 0);
    c.ecFound=f&1; c.wlFound=f&2; c.ambFound=f&4; c.rainFound=f&8;
    c.lastRainResetDay=cfgnvs.getInt("rainDay", -1);
  }
  cfgnvs.end();
  if (!haveNvs) {
    String js;
    if (!sdReadFile("/config/device.json", js) || !configFromJson(js, c)) return false;
  }
  applyConfigToGlobals(c);
  setConfigLoaded();
  LOGF("[persist] local config loaded (ecTarget %.2f, autoDosing %d)\n", c.ecTarget, c.autoDosing);
  return true;
}
```

- [ ] **Step 2: Call `persistConfig()` from `fetchDeviceConfig()`**

At the end of `fetchDeviceConfig()` in `cloud.cpp` (after the `if (plugMode == "refill") fetchRefillTankMax();` line ~`cloud.cpp:560`), and inside `fetchRefillTankMax()` after `refillCutoffMm` is set:

```cpp
  if (changed) { persistConfig(); setConfigLoaded(); }
```

Also call `setConfigLoaded()` unconditionally at the end of the first successful `fetchDeviceConfig()` (config now mirrors cloud).

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 4: Bench check**

Change `ec_target` on the dashboard. Expected serial: `[CONFIG] EC Target: ...` then a `/config/device.json` write. `cat` the file over a second boot to confirm contents.

- [ ] **Step 5: Commit**

```bash
git add src/persist.h src/persist.cpp src/cloud.cpp
git commit -m "Persist dosing config to NVS + SD on every confirmed change"
```

### Task 2.3: Persist schedules to SD on change

**Files:**
- Modify: `src/persist.cpp` / `.h` — `persistSchedules()`, `loadSchedulesLocal()`
- Modify: `src/cloud.cpp` `fetchSchedules()` — call `persistSchedules()` after the parse loop

**Interfaces:**
- Consumes: `sdAtomicWrite`, `sdReadFile`; `schedules[]`, `scheduleCount` globals.
- Produces:
  - `void persistSchedules();` — serialize `schedules[0..scheduleCount)` to `/config/schedules.json`.
  - `bool loadSchedulesLocal();` — parse the file back into `schedules[]` / `scheduleCount`. `false` if absent/unparseable.

- [ ] **Step 1: Implement**

```cpp
void persistSchedules() {
  DynamicJsonDocument d(8192);
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < scheduleCount; i++) {
    Schedule& s = schedules[i];
    JsonObject o = a.createNestedObject();
    o["id"]=s.id; o["name"]=s.name; o["relayNum"]=s.relayNum;
    o["hour"]=s.hour; o["minute"]=s.minute; o["duration"]=s.duration; o["enabled"]=s.enabled;
    JsonArray days = o.createNestedArray("days");
    for (int k = 0; k < 7; k++) days.add(s.days[k]);
  }
  String js; serializeJson(a, js);
  sdAtomicWrite("/config/schedules.json", (const uint8_t*)js.c_str(), js.length());
}

bool loadSchedulesLocal() {
  String js;
  if (!sdReadFile("/config/schedules.json", js)) return false;
  DynamicJsonDocument d(8192);
  if (deserializeJson(d, js) != DeserializationError::Ok) return false;
  scheduleCount = 0;
  for (JsonObject o : d.as<JsonArray>()) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    Schedule& s = schedules[scheduleCount++];
    s.id=o["id"]; s.name=o["name"].as<String>(); s.relayNum=o["relayNum"];
    s.hour=o["hour"]; s.minute=o["minute"]; s.duration=o["duration"]; s.enabled=o["enabled"];
    int k=0; for (JsonVariant v : o["days"].as<JsonArray>()) { if (k<7) s.days[k++]=v.as<bool>(); }
  }
  LOGF("[persist] %d schedule(s) loaded from SD\n", scheduleCount);
  return true;
}
```

- [ ] **Step 2: Call `persistSchedules()` from `fetchSchedules()`**

After the parse `for` loop and the debug print, before `syncR3TimersToTasmota()` (`cloud.cpp:718`):

```cpp
  persistSchedules();
```

(Every successful fetch rewrites the file; the atomic write makes an unchanged rewrite harmless. A content diff is a possible later optimisation, not required.)

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/persist.h src/persist.cpp src/cloud.cpp
git commit -m "Persist relay schedules to SD on every fetch"
```

### Task 2.4: Boot load-order — cloud, then local fallback

**Files:**
- Modify: `src/main.cpp` `setup()` (the `if (isRegistered) { ... }` init block ~`main.cpp:276-289`) and the one-time-init block in `loop()` (~`main.cpp:319-341`)

**Interfaces:**
- Consumes: `loadConfigLocal()`, `loadSchedulesLocal()`, `setRunState`, `haveUplink`.

- [ ] **Step 1: After the online init attempt, fall back to local**

At the end of `setup()`, after the `if (isRegistered) { ... fetchDeviceConfig(); }` block:

```cpp
  if (!configLoaded()) {
    LOGLN("[boot] no cloud config — loading local");
    if (loadConfigLocal()) {
      loadSchedulesLocal();
      setRunState(RS_OFFLINE_AUTONOMOUS);
    } else {
      LOGLN("[boot] no local config either — waiting for first uplink");
    }
  }
  startupTime = millis();
```

Mirror the same fallback in the `loop()` one-time-init block so a device that boots with no WiFi, then never gets it, still ends up autonomous.

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Bench check — cold-start offline**

Provision once online (so NVS + SD have config). Power off, disable WiFi + cellular, power on. Expected serial: `[boot] no cloud config — loading local`, `[persist] local config loaded`, `[persist] N schedule(s) loaded from SD`, `[RUN] BOOT -> OFFLINE_AUTONOMOUS`, then sensor reads + auto-dosing advancing. R1 fires on EC demand with no network present.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Boot load-order: cloud config, then local NVS/SD fallback"
```

### Task 2.5: Coarse clock persist + seed

**Files:**
- Modify: `src/persist.cpp` / `.h` — `persistClock()`, `seedClockFromStore()`, `noteNtpSynced()`
- Modify: `src/main.cpp` — call `seedClockFromStore()` in `setup()` before the control loop if NTP failed; add a 5-min `persistClock()` tick in `loop()`
- Modify: `src/cloud.cpp` `syncTimeWithNTP()` — call `noteNtpSynced()` on success
- Create test: add cases to `test/test_persist/test_persist.cpp`

**Interfaces:**
- Produces:
  - `void persistClock();` — write `time(nullptr)` (decimal string) to `/state/clock` and NVS key `cfg/epoch`.
  - `bool seedClockFromStore();` — if `time(nullptr) < 1000000000`, read the stored epoch, `settimeofday()` to it, mark the clock `approx`. `false` if no stored epoch.
  - `void noteNtpSynced();` — clears `approx`; if it was `approx` and a stored epoch existed, logs the correction delta via `logDeviceActivity("system", ...)`.
  - `bool clockIsApprox();`

- [ ] **Step 1: Write the failing test (pure logic — delta + approx flag)**

```cpp
void test_clock_delta_logged_once() {
  clockTestReset();                 // test hook: clears static state
  clockTestSetStoredEpoch(1000);
  clockTestSetNow(1000);
  TEST_ASSERT_TRUE(seedClockFromStore());
  TEST_ASSERT_TRUE(clockIsApprox());
  clockTestSetNow(5000);            // NTP now says real time is 5000
  long delta = noteNtpSyncedReturningDelta();  // test-only variant
  TEST_ASSERT_EQUAL(4000, delta);
  TEST_ASSERT_FALSE(clockIsApprox());
}
```

(Guard the test hooks behind `#ifdef UNIT_TEST` in `persist.cpp`.)

- [ ] **Step 2: Run — expect FAIL**

Run: `~/.platformio/penv/bin/pio test -e native -f test_persist`
Expected: FAIL.

- [ ] **Step 3: Implement**

```cpp
static bool approxClock = false;
static time_t storedEpochAtBoot = 0;

void persistClock() {
  time_t t = time(nullptr);
  if (t < 1000000000) return;
  char b[16]; snprintf(b, sizeof(b), "%ld", (long)t);
  sdAtomicWrite("/state/clock", (const uint8_t*)b, strlen(b));
  cfgnvs.begin("cfg", false); cfgnvs.putULong("epoch", (uint32_t)t); cfgnvs.end();
}

bool seedClockFromStore() {
  if (time(nullptr) >= 1000000000) return true;   // real time already
  uint32_t e = 0;
  cfgnvs.begin("cfg", true); e = cfgnvs.getULong("epoch", 0); cfgnvs.end();
  if (!e) {
    String s; if (sdReadFile("/state/clock", s)) e = (uint32_t)s.toInt();
  }
  if (e < 1000000000) return false;
  struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  storedEpochAtBoot = e; approxClock = true;
  LOGF("[clock] seeded from store: %u (approx)\n", e);
  return true;
}

void noteNtpSynced() {
  if (approxClock && storedEpochAtBoot) {
    long delta = (long)time(nullptr) - (long)storedEpochAtBoot;
    char m[80]; snprintf(m, sizeof(m), "Clock corrected after offline boot: +%lds", delta);
    logDeviceActivity("system", m);
  }
  approxClock = false;
}
bool clockIsApprox() { return approxClock; }
```

- [ ] **Step 4: Run — expect PASS**

Run: `~/.platformio/penv/bin/pio test -e native -f test_persist`
Expected: all PASS.

- [ ] **Step 5: Wire into `main.cpp` / `cloud.cpp`**

- `setup()`: after the NTP attempt, `if (time(nullptr) < 1000000000) seedClockFromStore();`
- `loop()`: `if (now - lastClockPersist >= 300000UL) { persistClock(); lastClockPersist = now; }` (add `static unsigned long lastClockPersist = 0;`)
- `syncTimeWithNTP()`: on the success `return`, call `noteNtpSynced();` first.

- [ ] **Step 6: Build + commit**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1` → `SUCCESS`

```bash
git add src/persist.h src/persist.cpp src/main.cpp src/cloud.cpp test/test_persist/test_persist.cpp
git commit -m "Persist + seed a coarse wall clock across offline reboots"
```

---

## PHASE 3 — SD telemetry buffering

### Task 3.1: NDJSON record encode/decode (host-tested)

**Files:**
- Create: `src/journal.h`, `src/journal.cpp`
- Create: `test/test_journal/test_journal.cpp`

**Interfaces:**
- Produces:
  - `String journalEncode(time_t t, bool approx, const char* tbl, const String& rowJson);` → `{"t":<t>,"approx":<0|1>,"tbl":"<tbl>","row":<rowJson>}` (one line, no newline).
  - `struct JournalRec { time_t t; bool approx; String tbl; String row; };`
  - `bool journalDecode(const String& line, JournalRec& out);` — `false` on any parse failure or unknown `tbl`.

- [ ] **Step 1: Write the failing test**

```cpp
#include <unity.h>
#include "journal.h"

void test_encode_decode_roundtrip() {
  String line = journalEncode(1725283200, false, "activity_log",
                              "{\"device\":\"sf500_107888\",\"category\":\"dosing\",\"action\":\"Dose 45s\"}");
  TEST_ASSERT_EQUAL(-1, line.indexOf('\n'));
  JournalRec r;
  TEST_ASSERT_TRUE(journalDecode(line, r));
  TEST_ASSERT_EQUAL(1725283200, r.t);
  TEST_ASSERT_FALSE(r.approx);
  TEST_ASSERT_EQUAL_STRING("activity_log", r.tbl.c_str());
  TEST_ASSERT_TRUE(r.row.indexOf("Dose 45s") > 0);
}

void test_decode_rejects_unknown_table() {
  String line = journalEncode(1, false, "activity_log", "{}");
  line.replace("activity_log", "evil_table");
  JournalRec r;
  TEST_ASSERT_FALSE(journalDecode(line, r));
}

void test_decode_rejects_torn_line() {
  JournalRec r;
  TEST_ASSERT_FALSE(journalDecode("{\"t\":1725283200,\"appro", r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_roundtrip);
  RUN_TEST(test_decode_rejects_unknown_table);
  RUN_TEST(test_decode_rejects_torn_line);
  return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL**

Run: `~/.platformio/penv/bin/pio test -e native -f test_journal`
Expected: FAIL.

- [ ] **Step 3: Implement encode/decode**

```cpp
#include "journal.h"
#include <ArduinoJson.h>

static bool knownTable(const String& t) {
  return t == "sensor_metrics" || t == "activity_log" || t == "relay_metrics";
}

String journalEncode(time_t t, bool approx, const char* tbl, const String& rowJson) {
  String s; s.reserve(rowJson.length() + 48);
  s += "{\"t\":"; s += (long)t;
  s += ",\"approx\":"; s += approx ? 1 : 0;
  s += ",\"tbl\":\""; s += tbl; s += "\",\"row\":";
  s += rowJson; s += "}";
  return s;
}

bool journalDecode(const String& line, JournalRec& out) {
  StaticJsonDocument<1024> d;
  if (deserializeJson(d, line) != DeserializationError::Ok) return false;
  if (!d.containsKey("t") || !d.containsKey("tbl") || !d.containsKey("row")) return false;
  out.t = (time_t)(long)d["t"];
  out.approx = (d["approx"] | 0) != 0;
  out.tbl = String((const char*)(d["tbl"] | ""));
  if (!knownTable(out.tbl)) return false;
  String r; serializeJson(d["row"], r); out.row = r;
  return true;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `~/.platformio/penv/bin/pio test -e native -f test_journal`
Expected: 3 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/journal.h src/journal.cpp test/test_journal/test_journal.cpp
git commit -m "Add NDJSON journal record encode/decode (host-tested)"
```

### Task 3.2: Journal append + offset read (device side)

**Files:**
- Modify: `src/journal.cpp` / `.h`

**Interfaces:**
- Consumes: `sdMounted`, `sdAppendLine`, `sdFileSize`, `sdReadFile`, `sdAtomicWrite` (Task 0.2); `clockIsApprox` (Task 2.5).
- Produces:
  - `#define JOURNAL_PATH "/buffer/pending.ndjson"` / `#define JOURNAL_OFFSET_PATH "/buffer/offset"`
  - `bool journalAppend(const char* tbl, const String& rowJson);` — encodes with `time(nullptr)` + `clockIsApprox()`, `sdAppendLine`. `false` if no SD.
  - `size_t journalReadOffset();` — parse `/buffer/offset` (decimal byte offset), `0` if absent.
  - `void journalWriteOffset(size_t off);` — `sdAtomicWrite` the decimal string.
  - `int journalNextBatch(size_t fromOffset, JournalRec* recs, size_t* lineEndOffsets, int maxRecs);` — read forward from `fromOffset`, decode up to `maxRecs` complete lines; return count; `lineEndOffsets[i]` = byte offset just past line `i` (for the caller to commit). A trailing partial line is ignored.

- [ ] **Step 1: Implement**

```cpp
bool journalAppend(const char* tbl, const String& rowJson) {
  if (!sdMounted()) return false;
  String line = journalEncode(time(nullptr), clockIsApprox(), tbl, rowJson);
  return sdAppendLine(JOURNAL_PATH, line);
}

size_t journalReadOffset() {
  String s;
  if (!sdReadFile(JOURNAL_OFFSET_PATH, s)) return 0;
  return (size_t)s.toInt();
}

void journalWriteOffset(size_t off) {
  char b[16]; snprintf(b, sizeof(b), "%u", (unsigned)off);
  sdAtomicWrite(JOURNAL_OFFSET_PATH, (const uint8_t*)b, strlen(b));
}

int journalNextBatch(size_t fromOffset, JournalRec* recs, size_t* ends, int maxRecs) {
  String all;
  if (!sdReadFile(JOURNAL_PATH, all)) return 0;
  int n = 0; size_t pos = fromOffset;
  while (n < maxRecs && pos < all.length()) {
    int nl = all.indexOf('\n', pos);
    if (nl < 0) break;                       // trailing partial line — stop
    String line = all.substring(pos, nl);
    if (journalDecode(line, recs[n])) { ends[n] = nl + 1; n++; }
    pos = nl + 1;
  }
  return n;
}
```

(Reading the whole file into a `String` is acceptable at the retention cap because the batch loop stops after `maxRecs`; for large files, Task 3.4's compaction keeps it small. If heap pressure shows in Phase 5, switch to a seek-based `File32` read.)

- [ ] **Step 2: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add src/journal.h src/journal.cpp
git commit -m "Add journal append + offset-based batch read"
```

### Task 3.3: Route the four emit points through the journal

**Files:**
- Modify: `src/cloud.cpp` — `logDeviceActivity()`, `uploadSensorReadings()`
- Modify: `src/relay.cpp` — `writeRelay()` `relay_metrics` block
- Modify: `src/mqtt_handler.cpp` — `logR3Transition()`

**Interfaces:**
- Consumes: `journalAppend(tbl, rowJson)`; `sdMounted()`.

- [ ] **Step 1: `logDeviceActivity()` → journal-first**

Replace the body after the payload is built:

```cpp
  String payload; serializeJson(doc, payload);
  if (sdMounted()) { journalAppend("activity_log", payload); return; }
  // no SD: fall back to direct POST when online
  if (!haveUplink()) return;
  HTTPClient http; ... http.POST(payload); noteUplinkResult(...); http.end();
```

- [ ] **Step 2: `uploadSensorReadings()` → journal the array as one record per row**

The function builds a JSON *array* of per-sensor rows. Append each element as its own `sensor_metrics` record so the backfill batch logic is uniform:

```cpp
  for (JsonObject row : arr) {
    String rj; serializeJson(row, rj);
    if (sdMounted()) journalAppend("sensor_metrics", rj);
  }
  if (sdMounted()) return;
  if (!haveUplink()) return;
  /* existing direct POST of the whole array */
```

- [ ] **Step 3: `writeRelay()` + `logR3Transition()` → journal the `relay_metrics` row**

Replace each `http.POST(payload)` for `relay_metrics` with:

```cpp
  if (sdMounted()) { journalAppend("relay_metrics", payload); }
  else if (haveUplink()) { /* existing direct POST */ }
```

- [ ] **Step 4: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 5: Bench check — offline accumulation**

Provision online, go fully offline, run auto-dosing for ~30 min. Expected: `/buffer/pending.ndjson` grows; line count ≈ (activity events) + (sensor rows every 5 min) + (relay transitions) seen on serial. `/buffer/offset` stays `0`.

- [ ] **Step 6: Commit**

```bash
git add src/cloud.cpp src/relay.cpp src/mqtt_handler.cpp
git commit -m "Route activity/sensor/relay telemetry through the SD journal"
```

### Task 3.4: Compaction + rotation (host-tested math)

**Files:**
- Modify: `src/journal.cpp` / `.h`
- Modify: `test/test_journal/test_journal.cpp`

**Interfaces:**
- Produces:
  - `#define JOURNAL_COMPACT_THRESHOLD (512UL * 1024UL)` — compact once the committed offset passes this.
  - `void journalCompact();` — drop the consumed prefix `[0, offset)`, rewrite the file, reset offset to 0. Called by the drain task after committing a batch when `offset >= JOURNAL_COMPACT_THRESHOLD`.
  - `size_t journalPendingBytes();` — `sdFileSize(JOURNAL_PATH) - journalReadOffset()`.

- [ ] **Step 1: Write the failing test (prefix-drop is pure string logic — extract it)**

```cpp
// journalDropPrefix(all, offset) -> remaining content after the offset,
// starting at the next line boundary. Exposed for testing.
void test_compact_drops_consumed_prefix() {
  String all = "line1\nline2\nline3\n";
  String out = journalDropPrefix(all, 6);   // offset at start of "line2"
  TEST_ASSERT_EQUAL_STRING("line2\nline3\n", out.c_str());
}
void test_compact_offset_mid_line_snaps_forward() {
  String all = "line1\nline2\nline3\n";
  String out = journalDropPrefix(all, 8);   // mid "line2"
  TEST_ASSERT_EQUAL_STRING("line3\n", out.c_str());
}
```

- [ ] **Step 2: Run — expect FAIL**, then implement:

```cpp
String journalDropPrefix(const String& all, size_t offset) {
  if (offset >= all.length()) return String("");
  int nl = all.indexOf('\n', offset);
  if (nl < 0) return String("");
  return all.substring(nl + 1);
}

void journalCompact() {
  String all;
  if (!sdReadFile(JOURNAL_PATH, all)) return;
  size_t off = journalReadOffset();
  String remaining = journalDropPrefix(all, off);
  if (sdAtomicWrite(JOURNAL_PATH, (const uint8_t*)remaining.c_str(), remaining.length()))
    journalWriteOffset(0);
}
```

- [ ] **Step 3: Run — expect PASS**

Run: `~/.platformio/penv/bin/pio test -e native -f test_journal`
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/journal.h src/journal.cpp test/test_journal/test_journal.cpp
git commit -m "Add journal compaction with host-tested prefix-drop"
```

### Task 3.5: Retention cap + eviction (host-tested)

**Files:**
- Modify: `src/journal.cpp` / `.h`
- Modify: `test/test_journal/test_journal.cpp`

**Interfaces:**
- Produces:
  - `#define JOURNAL_MAX_BYTES (256UL * 1024UL * 1024UL)`
  - `void journalEnforceRetention();` — if `sdFileSize(JOURNAL_PATH) > JOURNAL_MAX_BYTES`, rewrite the file keeping all `activity_log` + `relay_metrics` lines and only the newest `sensor_metrics` lines that fit under the cap. Called from the drain task (cheap size check first).
  - `String journalEvictSensorMetrics(const String& all, size_t targetBytes);` — pure, exposed for testing.

- [ ] **Step 1: Write the failing test**

```cpp
void test_eviction_keeps_audit_drops_oldest_sensor() {
  String all =
    "{\"t\":1,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n"
    "{\"t\":2,\"approx\":0,\"tbl\":\"activity_log\",\"row\":{}}\n"
    "{\"t\":3,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n"
    "{\"t\":4,\"approx\":0,\"tbl\":\"relay_metrics\",\"row\":{}}\n"
    "{\"t\":5,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n";
  String out = journalEvictSensorMetrics(all, 200);  // room for ~audit + 1 sensor
  TEST_ASSERT_TRUE(out.indexOf("\"t\":2") > 0);      // activity kept
  TEST_ASSERT_TRUE(out.indexOf("\"t\":4") > 0);      // relay kept
  TEST_ASSERT_TRUE(out.indexOf("\"t\":5") > 0);      // newest sensor kept
  TEST_ASSERT_EQUAL(-1, out.indexOf("\"t\":1"));     // oldest sensor dropped
}
```

- [ ] **Step 2: Run — expect FAIL**, then implement `journalEvictSensorMetrics` (walk lines, always keep non-`sensor_metrics`, keep `sensor_metrics` newest-first until `targetBytes` reached, re-emit in original order) and the `journalEnforceRetention()` wrapper.

- [ ] **Step 3: Run — expect PASS**

Run: `~/.platformio/penv/bin/pio test -e native -f test_journal`
Expected: all PASS.

- [ ] **Step 4: Commit**

```bash
git add src/journal.h src/journal.cpp test/test_journal/test_journal.cpp
git commit -m "Add journal retention cap: keep audit trail, evict oldest sensor rows"
```

---

## PHASE 4 — Backfill on recovery

### Task 4.1: `recorded_at` column migration

**Files:**
- Create: `docs/migrations/offline-recorded-at.sql`

**Interfaces:**
- Produces: nullable `recorded_at timestamptz` on `sensor_metrics`, `activity_log`, `relay_metrics`.

- [ ] **Step 1: Write the migration record**

```sql
-- offline-recorded-at: client-supplied capture time for backfilled offline rows.
-- Nullable; when null, treat created_at (server default now()) as the capture time.
alter table public.sensor_metrics add column if not exists recorded_at timestamptz;
alter table public.activity_log  add column if not exists recorded_at timestamptz;
alter table public.relay_metrics add column if not exists recorded_at timestamptz;

-- Firmware inserts with the anon/service key used today; no RLS policy change is
-- needed for an added nullable column, but confirm INSERT still succeeds with the
-- column present (PostgREST schema cache reload may be required).
```

- [ ] **Step 2: Apply to the prod project** (`qkqeysggrqhxizkdmbhx`) via the Supabase MCP `apply_migration` (name `offline_recorded_at`), or hand to the requester if they prefer to run it. Confirm with a probe insert including `recorded_at`.

- [ ] **Step 3: Commit**

```bash
git add docs/migrations/offline-recorded-at.sql
git commit -m "Add recorded_at column migration record for offline backfill"
```

### Task 4.2: Include `recorded_at` in live inserts

**Files:**
- Modify: `src/cloud.cpp` (`uploadSensorReadings`, `logDeviceActivity`), `src/relay.cpp` (`writeRelay` relay_metrics), `src/mqtt_handler.cpp` (`logR3Transition`)

**Interfaces:**
- Produces: an ISO-8601 `recorded_at` field in each row object, built from `time(nullptr)` (reuse the `sprintf(ts, "%04d-...+08:00", ...)` idiom already in `updateDeviceStatus()` / `publishRelayStatus()`).

- [ ] **Step 1: Add a helper**

In `cloud.cpp`:

```cpp
String isoNow() {
  time_t t = time(nullptr);
  if (t < 1000000000) return String();
  struct tm ti; localtime_r(&t, &ti);
  char b[30];
  sprintf(b, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
          ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(b);
}
```

Declare it in `cloud.h`. In each row builder, `if (isoNow().length()) row["recorded_at"] = isoNow();` before serialising. This means the journalled `row` JSON already carries `recorded_at` — the backfill in Task 4.3 does not need to add it.

- [ ] **Step 2: Build + commit**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1` → `SUCCESS`

```bash
git add src/cloud.cpp src/cloud.h src/relay.cpp src/mqtt_handler.cpp
git commit -m "Stamp recorded_at on every telemetry row at capture time"
```

### Task 4.3: The bounded drain task

**Files:**
- Create: `src/backfill.h`, `src/backfill.cpp`
- Modify: `src/main.cpp` `loop()` — call `backfillTick()` in the periodic block

**Interfaces:**
- Consumes: `journalNextBatch`, `journalReadOffset`, `journalWriteOffset`, `journalCompact`, `journalEnforceRetention`, `journalPendingBytes` (Phase 3); `haveUplink`, `noteUplinkResult` (Phase 1); `autoState` (`globals.h`).
- Produces:
  - `#define BACKFILL_BATCH 8` / `#define BACKFILL_MIN_INTERVAL_MS 3000UL`
  - `void backfillTick();` — no-op unless `haveUplink()`, SD mounted, journal has pending bytes, `BACKFILL_MIN_INTERVAL_MS` elapsed, and `autoState` is not `AUTO_DOSING`/`AUTO_PRE_MIX`/`AUTO_POST_MIX`. Reads one batch, POSTs each record to `SUPABASE_URL + "/rest/v1/" + rec.tbl` with a 4 s timeout, and on an all-success batch advances the offset to the last `lineEndOffsets` value; on the first failure it stops (leaves the offset at the last committed line) and calls `noteUplinkResult(false)`. After a committed batch, runs compaction/retention if thresholds are crossed.

- [ ] **Step 1: Implement**

```cpp
#include "backfill.h"
#include "journal.h"
#include "netstate.h"
#include "globals.h"
#include "logger.h"
#include <HTTPClient.h>

static bool doseCritical() {
  return autoState == AUTO_DOSING || autoState == AUTO_PRE_MIX || autoState == AUTO_POST_MIX;
}

static bool postRow(const String& tbl, const String& rowJson) {
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/" + tbl;
  if (!http.begin(secureClient, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(4000);
  int code = http.POST(rowJson);
  http.end();
  return code >= 200 && code < 300;
}

void backfillTick() {
  static unsigned long last = 0;
  if (!haveUplink() || !sdMounted() || doseCritical()) return;
  if (millis() - last < BACKFILL_MIN_INTERVAL_MS) return;
  if (journalPendingBytes() == 0) return;
  last = millis();

  size_t off = journalReadOffset();
  JournalRec recs[BACKFILL_BATCH];
  size_t ends[BACKFILL_BATCH];
  int n = journalNextBatch(off, recs, ends, BACKFILL_BATCH);
  if (n == 0) return;

  int committed = 0;
  for (int i = 0; i < n; i++) {
    if (!postRow(recs[i].tbl, recs[i].row)) { noteUplinkResult(false); break; }
    committed = i + 1;
  }
  if (committed > 0) {
    journalWriteOffset(ends[committed - 1]);
    noteUplinkResult(true);
    LOGF("[backfill] %d row(s) replayed, offset -> %u\n", committed, (unsigned)ends[committed - 1]);
    if (journalReadOffset() >= JOURNAL_COMPACT_THRESHOLD) journalCompact();
    journalEnforceRetention();
  }
}
```

- [ ] **Step 2: Call from `loop()`**

In the periodic-task section of `main.cpp` `loop()`, after the `fetchSchedules` block:

```cpp
  backfillTick();
```

(It self-rate-limits; no `lastXxx` gate needed at the call site.)

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/bin/pio run -e esp32-s3-devkitm-1`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/backfill.h src/backfill.cpp src/main.cpp
git commit -m "Add bounded journal drain task (backfill on uplink recovery)"
```

### Task 4.4: Bench verification — end-to-end backfill

**Files:** none (record transcript + a Supabase query result in the checkbox note).

- [ ] **Step 1** — Provision online, go fully offline for ~45 min with auto-dosing active. Note the serial timestamps of each dose / activity line.
- [ ] **Step 2** — Restore connectivity. Expected serial: `[RUN] OFFLINE_AUTONOMOUS -> ONLINE`, then `[backfill] N row(s) replayed` lines every ~3 s until `journalPendingBytes()` hits 0.
- [ ] **Step 3** — Query Supabase:
  ```sql
  select tbl, count(*), min(recorded_at), max(recorded_at)
  from (
    select 'activity_log' tbl, recorded_at from activity_log where device='sf500_107888'
    union all select 'sensor_metrics', recorded_at from sensor_metrics where device='sf500_107888'
    union all select 'relay_metrics', recorded_at from relay_metrics where device='sf500_107888'
  ) x where recorded_at > now() - interval '2 hours' group by 1;
  ```
  Expected: row counts match what serial showed; `recorded_at` spread covers the whole offline window (not collapsed to the reconnect minute); ordering by `recorded_at` is monotonic.
- [ ] **Step 4** — Confirm the live path was not stalled during backfill: `last_heartbeat_at` kept updating every 30 s throughout; no watchdog reset in the boot log.

---

## PHASE 5 — Hardening + soak

### Task 5.1: Stack high-water measurement

**Files:**
- Modify: `src/main.cpp` — add a 60 s telemetry line

- [ ] **Step 1: Add the probe**

```cpp
  if (now - lastStackLog >= 60000UL) {
    lastStackLog = now;
    LOGF("[diag] loop stack high-water: %u bytes free, heap %u\n",
         uxTaskGetStackHighWaterMark(nullptr), ESP.getFreeHeap());
  }
```

- [ ] **Step 2: Exercise the worst case** — trigger a dose (mbedTLS relay_metrics path) + a config write (SD) + a backfill batch in the same few seconds, offline→online transition. Watch the high-water value.
- [ ] **Step 3: Decide** — if free stack ever drops below ~3000 bytes, raise `getArduinoLoopTaskStackSize()` (currently 20480) in 4 KB steps and/or move `backfillTick()`'s POST loop into a short-lived pinned task. Record the final headroom.
- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Log loop-task stack high-water; size loop stack for SD+TLS+JSON"
```

### Task 5.2: Fault injection — card + heap

- [ ] **Step 1: Card absent from cold** — remove card, boot offline. Expected: `[SD] unavailable`, config still loads from NVS, auto-dosing runs, `journalAppend` returns false quietly, no crash. Telemetry falls back to direct-POST/drop.
- [ ] **Step 2: Card pulled at runtime** — pull card mid-operation. Expected: subsequent `sdAppendLine` / `sdAtomicWrite` fail and return false; no crash; a single `[SD] write failed` style log, not a spam loop.
- [ ] **Step 3: Low heap** — reduce free heap (leave MQTT buffer at 4096, run backfill during a dose). Expected: `DynamicJsonDocument` alloc guards (`doc.capacity()==0`) trip cleanly; no crash.
- [ ] **Step 4:** record all three transcripts in this task's note. No code changes expected; if any path crashes, fix and re-run.

### Task 5.3: Power-cut-during-write proof

- [ ] **Step 1** — Loop a script that toggles `ec_target` on the dashboard every ~3 s (forces repeated `persistConfig()` → `/config/device.json` atomic writes).
- [ ] **Step 2** — Cut power to the board at random ~20 times during that loop.
- [ ] **Step 3** — After each cut, boot offline and confirm `/config/device.json` is always valid JSON parseable by `configFromJson` (never a truncated/empty file) and `/config/device.json.tmp` is either absent or ignored. The seeded clock still loads.
- [ ] **Step 4** — Same for the journal: confirm a power cut mid-`sdAppendLine` leaves at most one torn trailing line that `journalNextBatch` skips, and no earlier record is lost.
- [ ] **Step 5** — record transcript. If any boot comes up with a corrupt config, add a `.bak` (keep the previous good file, fall back on parse failure) and re-test.

### Task 5.4: 24–48 h soak

- [ ] **Step 1** — Run `sf500_107888` on the bench with auto-dosing active and a WL sensor connected.
- [ ] **Step 2** — On a cron, induce outages: every ~2 h drop WiFi+cellular for a random 10–40 min, and 3–4 times over the soak also power-cycle during an outage.
- [ ] **Step 3** — Success criteria, all must hold:
  - Auto-dosing never stalls > 2 s across any outage or reboot (serial cadence).
  - Every offline dose / activity / sensor row appears in Supabase after the following recovery, with correct `recorded_at`, in order, no duplicates.
  - `journalPendingBytes()` returns to 0 after every recovery.
  - No watchdog reset, no `Guru Meditation`, no heap-exhaustion reboot in the whole soak (`resetReasonStr` in every boot log is `POWERON` or `SW_RESTART` only).
  - Loop stack high-water stays above the Task 5.1 floor throughout.
- [ ] **Step 4** — Write the soak result into `docs/superpowers/plans/2026-09-02-offline-autonomy.md` under a new `## Soak result` section and commit. This closes the work.

---

## Self-Review

**1. Spec coverage**

| Spec section | Task(s) |
|---|---|
| §1.1 control halts on loss | 1.3, 1.5, 1.6 |
| §1.2 no cold-start offline | 2.2, 2.4 |
| §1.3 no data retention / TLS stalls | 1.2, 3.3, 4.3 |
| §3 SD on SPI2 | 0.2, 0.3, 0.4 |
| §4.1 RunState | 1.1 |
| §4.2 portal no longer gates | 1.5 |
| §4.3 non-blocking reconnect | 1.3, 1.4 |
| §4.4 haveUplink() | 1.1, 1.2 |
| §5.1 NVS scalars | 2.2 |
| §5.2 SD schedules + mirror | 2.2, 2.3 |
| §5.3 boot/load order | 2.4 |
| §6 coarse clock | 2.5 |
| §7.1 NDJSON format | 3.1 |
| §7.2 always-buffer-then-drain | 3.3 |
| §7.3 drain task bounded + dose-critical guard | 4.3 |
| §7.4 retention cap, evict sensor first | 3.5 |
| §7.5 recorded_at column | 4.1, 4.2 |
| §8 stack / power-loss / card-pull / watchdog risks | 5.1, 5.2, 5.3, 5.4 |
| §9 rebase onto cellular-fallback, compose with activeTransport | Global Constraints; 1.1, 1.3, 1.5 |
| §10 testing plan | 0.4, 1.x bench checks, 2.4, 3.3, 4.4, 5.4 |

No gaps.

**2. Placeholder scan** — no "TBD"/"handle edge cases"/"similar to Task N"; every code step has a code block; bench-only tasks state exact commands + expected serial lines.

**3. Type consistency** — `LocalConfig` fields consistent across 2.1/2.2/2.3; `JournalRec {t,approx,tbl,row}` consistent across 3.1/3.2/3.4/4.3; `haveUplink()`/`noteUplinkResult()`/`noteMqttState()` consistent across 1.1/1.2/1.4/4.3; `journalNextBatch(fromOffset, recs, lineEndOffsets, maxRecs)` signature consistent 3.2 ↔ 4.3; `RunState` enum values consistent 1.1/1.5/1.6.
