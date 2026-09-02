# Cellular (4G) Fallback Networking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give ECM50-A09 units with the onboard Quectel EC801E-CN modem an automatic WiFi→cellular fallback (and back), without touching behavior on the rest of the fleet.

**Architecture:** Auto-detect the modem at boot (mirrors the existing sensor-scan pattern). A `Client*`/`Client&` abstraction lets MQTT and Supabase HTTPS calls run over either `WiFiClient`/`WiFiClientSecure` or `TinyGsmClient`/`SSLClient` (software TLS, since the modem's own SSL AT commands are unsupported on this firmware build). A small state machine extends the existing `WiFiState` logic: try WiFi, fall back to cellular when WiFi's retry budget is exhausted (and an APN is configured), keep retrying WiFi in the background, switch back automatically.

**Tech Stack:** Arduino-ESP32 2.0.17 (pinned), TinyGSM (`TINY_GSM_MODEM_BG96` profile), SSLClientMbedTLS, PubSubClient, HTTPClient (generic `Client&` overload).

**Spec:** `docs/superpowers/specs/2026-09-02-cellular-fallback-design.md`

## Global Constraints

- Pinned platform: `espressif32@6.13.0` (Arduino-ESP32 core 2.0.17) — do not bump.
- `loopTask` stack is 20480 bytes (`getArduinoLoopTaskStackSize()` in `main.cpp`, from the v1.2.5 fix) — this feature adds more code to the same task; watch stack margin, don't reduce that value.
- Boards without the 4G module must see zero behavior change. Every new code path is gated behind runtime hardware detection (§ below), never a compile-time flag — same firmware image ships to the whole fleet.
- This repo has no automated test framework. "Test" in every task below means: `pio run` builds clean, then flash to the tethered bench unit (`sf500_107888`, `/dev/cu.usbserial-110`, 9600 baud debug console) and verify the exact serial output described in that task's Verify step.
- `FIRMWARE_VERSION` bump happens once, in the final task, per [[feedback_bump_version_with_firmware_push]] (bump in the same commit as the last real change, not a separate follow-up).
- Confirmed working facts from the spike (do not re-derive): modem pins PWR=GPIO38 (HIGH=on), UART2 TX=GPIO39/RX=GPIO40 @115200; APN for the current test SIM is `ansar`; `TINY_GSM_MODEM_BG96` profile's plain `TinyGsmClient` and `SSLClientMbedTLS` (with `setInsecure()` during bench testing) both work end-to-end against the real Supabase project.

---

## Phase 1: Hardware auto-detection

Goal of this phase: the firmware knows, at boot, whether a working cellular modem is present — without touching networking logic yet.

### Task 1.1: Modem pins + detection function

**Files:**
- Create: `src/cellular.h`
- Create: `src/cellular.cpp`
- Modify: `src/config.h` (add pin/baud defines)
- Modify: `src/globals.h` (add `extern bool cellularCapable;`)
- Modify: `src/main.cpp` (define `cellularCapable`, call detection in `setup()`)

**Interfaces:**
- Produces: `bool detectCellularModem()` — powers the modem, sends up to 3 `AT` probes (matches the retry pattern already used for WiFi/AT-style liveness checks), returns true/false. Also produces the module-scope `HardwareSerial modemSerial` (UART2) that later phases reuse.
- Produces: global `bool cellularCapable` (globals.h/main.cpp) — set once in `setup()`, read everywhere else.

- [ ] **Step 1: Add the pin/baud defines**

In `src/config.h`, add near the other pin defines (`RX_PIN`/`TX_PIN`/`RELAY1_PIN`/`RELAY2_PIN`):

```cpp
// 4G modem (Quectel EC801E-CN, present only on the ECM50-A09 4G board variant)
#define MODEM_PWR_PIN 38
#define MODEM_TX_PIN  39   // ESP32 TX -> modem RXD
#define MODEM_RX_PIN  40   // ESP32 RX <- modem TXD
#define MODEM_BAUD    115200
```

- [ ] **Step 2: Create `src/cellular.h`**

```cpp
#pragma once
#include <Arduino.h>

extern HardwareSerial modemSerial;

// Powers the modem and probes it with AT. Safe to call on boards without
// the modem populated — just returns false after the probes time out.
bool detectCellularModem();
```

- [ ] **Step 3: Create `src/cellular.cpp`**

```cpp
#include "cellular.h"
#include "config.h"
#include "logger.h"

HardwareSerial modemSerial(2); // UART2 — shared with LoRa on non-4G boards, but those never call this file

static bool sendModemAT(const char* cmd, unsigned long timeoutMs = 2000)
{
  while (modemSerial.available()) modemSerial.read();
  modemSerial.print(cmd);
  modemSerial.print("\r\n");

  String resp;
  unsigned long start = millis();
  while (millis() - start < timeoutMs)
  {
    while (modemSerial.available())
      resp += (char)modemSerial.read();
    if (resp.indexOf("OK") >= 0) return true;
    if (resp.indexOf("ERROR") >= 0) return false;
    delay(20);
  }
  return false;
}

bool detectCellularModem()
{
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(5000); // matches the boot delay proven necessary during the spike

  for (int i = 0; i < 3; i++)
    if (sendModemAT("AT")) return true;

  digitalWrite(MODEM_PWR_PIN, LOW); // no modem present — don't leave the pin driven high
  return false;
}
```

- [ ] **Step 4: Wire into globals + main.cpp**

In `src/globals.h`, alongside the other `extern` globals (near `extern WiFiState wifiState;`):

```cpp
extern bool cellularCapable;
```

In `src/main.cpp`, alongside the other global variable definitions (near `WiFiState wifiState = STATE_PORTAL;`):

```cpp
bool cellularCapable = false;
```

Add `#include "cellular.h"` to `main.cpp`'s includes. In `setup()`, immediately after the existing `initSensors()` call, add:

```cpp
cellularCapable = detectCellularModem();
LOGF("[Cellular] Modem %s\n", cellularCapable ? "detected" : "not present");
```

- [ ] **Step 5: Build and verify on the tethered unit**

Run: `pio run` (in this worktree) — expect a clean build.
Flash: `pio run --target upload --upload-port /dev/cu.usbserial-110`
Verify: connect serial at 9600 baud (`stty -f /dev/cu.usbserial-110 9600 cs8 -cstopb -parenb raw && cat /dev/cu.usbserial-110`). Expect `[Cellular] Modem detected` in the boot log, shortly after the sensor-init lines.

- [ ] **Step 6: Commit**

```bash
git add src/cellular.h src/cellular.cpp src/config.h src/globals.h src/main.cpp
git commit -m "Add cellular modem hardware auto-detection"
```

---

## Phase 2: Cellular data connection + MQTT over cellular

Goal: bring up a cellular data session and prove MQTT (plain TCP, no TLS needed) works over it — manually triggered via a serial debug command for now, not yet wired into automatic fallback.

### Task 2.1: Add TinyGSM and the data-connect function

**Files:**
- Modify: `platformio.ini` (lib_deps, build_flags)
- Modify: `src/cellular.h` / `src/cellular.cpp`
- Modify: `src/globals.h` (add `extern String cellularApn;` placeholder — real config wiring is Task 4.2)

**Interfaces:**
- Consumes: `HardwareSerial modemSerial` (from Task 1.1), `cellularCapable`.
- Produces: `TinyGsm modem` and `TinyGsmClient cellularClient` (module-scope in `cellular.cpp`, declared `extern` in `cellular.h` for later tasks). Produces `bool connectCellularData(const char* apn)`.

- [ ] **Step 1: Add the library dependency**

In `platformio.ini`, add to `lib_deps`:

```ini
    vshymanskyy/TinyGSM @ ^0.11.7
```

Add a new `build_flags` section (or extend it if one exists):

```ini
build_flags =
    -D TINY_GSM_MODEM_BG96
    -D TINY_GSM_RX_BUFFER=1024
```

- [ ] **Step 2: Extend `src/cellular.h`**

```cpp
#pragma once
#include <Arduino.h>
#define TINY_GSM_MODEM_BG96
#include <TinyGsmClient.h>

extern HardwareSerial modemSerial;
extern TinyGsm modem;
extern TinyGsmClient cellularClient;

bool detectCellularModem();
bool connectCellularData(const char* apn); // brings up the data session; call after detectCellularModem() succeeds
```

- [ ] **Step 3: Extend `src/cellular.cpp`**

Add after the existing includes:

```cpp
TinyGsm modem(modemSerial);
TinyGsmClient cellularClient(modem);
```

Add the new function:

```cpp
bool connectCellularData(const char* apn)
{
  if (!modem.waitForNetwork(30000))
  {
    LOGLN("[Cellular] FAIL: no network registration");
    return false;
  }
  if (!modem.gprsConnect(apn, "", ""))
  {
    LOGLN("[Cellular] FAIL: data connect failed");
    return false;
  }
  LOGF("[Cellular] Data connected, APN=%s\n", apn);
  return true;
}
```

- [ ] **Step 4: Add a manual serial debug trigger for bench testing**

In `src/main.cpp`'s `handleSerialCommands()` (the existing function backing `R1ON`/`WIFIINFO`/`HELP` etc.), add a new command `CELLTEST`:

```cpp
else if (cmd == "CELLTEST")
{
  if (!cellularCapable) { LOGLN("[Cellular] No modem detected"); }
  else if (connectCellularData("ansar"))
  {
    mqttClient.setClient(cellularClient);
    if (mqttClient.connect(("SF500_" + lastSix + "_celltest").c_str()))
    {
      LOGLN("[Cellular] MQTT connected over cellular");
      mqttClient.publish((mqttTopicData).c_str(), "{\"celltest\":true}");
      LOGLN("[Cellular] Test publish sent");
    }
    else
    {
      LOGF("[Cellular] MQTT connect failed, state=%d\n", mqttClient.state());
    }
  }
}
```

Add `CELLTEST` to the `HELP` command's printed list alongside the existing commands.

- [ ] **Step 5: Build, flash, verify on the tethered unit**

Run: `pio run` — clean build.
Flash and open serial monitor as in Task 1.1 Step 5.
Type `CELLTEST` into the serial monitor.
Verify: `[Cellular] Data connected, APN=ansar`, then `[Cellular] MQTT connected over cellular`, then `[Cellular] Test publish sent`.
Cross-check: subscribe to `sf500/{lastSix}/data` on `broker.emqx.io` from this machine (`mosquitto_sub -h broker.emqx.io -p 1883 -t 'sf500/<lastSix>/data' -C 1`) — confirm the `{"celltest":true}` payload actually arrives, proving the round trip left the device.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/cellular.h src/cellular.cpp src/main.cpp
git commit -m "Add cellular data connection + manual MQTT-over-cellular test"
```

---

## Phase 3: HTTPS over cellular (Supabase)

Goal: prove a real Supabase call works over cellular, using software TLS since the modem's own SSL AT commands don't work on this firmware. Still manually triggered.

### Task 3.1: Add SSLClientMbedTLS and the secure-client abstraction

**Files:**
- Modify: `platformio.ini`
- Modify: `src/cellular.h` / `src/cellular.cpp`
- Modify: `src/globals.h` (add `extern Client* activeSecureClient;`)
- Modify: `src/main.cpp` (define + initialize `activeSecureClient`)

**Interfaces:**
- Consumes: `TinyGsmClient cellularClient` (Task 2.1).
- Produces: `SSLClient cellularSecureClient` (module-scope in `cellular.cpp`). Produces global `Client* activeSecureClient`, defaulting to `&secureClient` (the existing `WiFiClientSecure`) so behavior is unchanged until something explicitly switches it.

- [ ] **Step 1: Add the library dependency**

In `platformio.ini`'s `lib_deps`:

```ini
    https://github.com/vshymanskyy/SSLClientMbedTLS.git
```

- [ ] **Step 2: Extend `src/cellular.h`**

```cpp
#include <SSLClient.h>
// ... (keep existing declarations)
extern SSLClient cellularSecureClient;
```

- [ ] **Step 3: Extend `src/cellular.cpp`**

```cpp
SSLClient cellularSecureClient(&cellularClient);
```

In `connectCellularData()`, after the existing success path (before `return true;`), add:

```cpp
cellularSecureClient.setInsecure(); // TODO Task 5.1 replaces this with setCACert()
```

- [ ] **Step 4: Add the active-secure-client global**

In `src/globals.h`, near `extern WiFiClientSecure secureClient;`:

```cpp
extern Client* activeSecureClient;
```

In `src/main.cpp`, near the other global definitions (after `WiFiClientSecure secureClient;`):

```cpp
Client* activeSecureClient = &secureClient;
```

- [ ] **Step 5: Repoint every Supabase call site to the abstraction**

In `src/cloud.cpp`, replace every occurrence of `http.begin(secureClient, ...)` with `http.begin(*activeSecureClient, ...)`. There are 9 call sites, in these functions: `registerDevice()` (2 occurrences), `uploadSensorConfig()`, `uploadSensorReadings()`, `updateDeviceStatus()`, `fetchDeviceConfig()`, `fetchSchedules()`, `logDeviceActivity()` — and one more in `uploadSensorReadings()` or nearby (verify against the current file; the exact count was 9 as of this plan's writing — grep `secureClient` in `cloud.cpp` to confirm none were missed).

In `src/mqtt_handler.cpp`, replace the one occurrence (`http.begin(secureClient, url)` inside the relay-metrics logging function) the same way.

Do **not** touch `src/ota.cpp` — it uses its own local `WiFiClientSecure` instances (`apiClient`, `otaClient`), not the shared `secureClient`/`activeSecureClient`. OTA-over-cellular is explicitly out of scope for this plan (see spec §4.4) — OTA checks simply won't fire while the device is on cellular fallback; they resume once WiFi returns.

- [ ] **Step 6: Add a manual serial debug trigger**

Extend the `CELLTEST` command from Task 2.1 — after the MQTT test block, add:

```cpp
activeSecureClient = &cellularSecureClient;
fetchDeviceConfig();
activeSecureClient = &secureClient; // restore — this is a manual bench test, not the real switch (that's Phase 4)
```

- [ ] **Step 7: Build, flash, verify**

Run: `pio run` — clean build. Fix any missed call sites the compiler flags (it won't catch semantic mismatches, but a leftover `secureClient` reference will still compile — rely on the grep from Step 5, not just the build, to confirm completeness).
Flash and open serial monitor.
Type `CELLTEST`.
Verify: after the MQTT lines from Phase 2, see `fetchDeviceConfig()`'s existing log output (its normal `[CONFIG] ...` lines) succeed with real data — proving the Supabase HTTPS call completed over the cellular software-TLS path.

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/cellular.h src/cellular.cpp src/globals.h src/main.cpp src/cloud.cpp src/mqtt_handler.cpp
git commit -m "Add software-TLS Supabase client for cellular, repoint HTTPS call sites"
```

---

## Phase 4: Automatic WiFi <-> cellular state machine

Goal: replace the manual `CELLTEST` trigger with the real automatic fallback behavior from spec §4.3, driven by actual WiFi failure, with the `cellular_apn` config field wired end to end.

### Task 4.1: Database migration for `cellular_apn`

**Files:**
- Create: `docs/migrations/cellular-apn.sql` (record of the migration, matching the precedent set by `docs/migrations/plug-http-host.sql`)

**Interfaces:**
- Produces: `device_management.cellular_apn text` (nullable), applied to the Supabase project.

- [ ] **Step 1: Write and record the migration**

```sql
-- cellular_apn: APN string for the SIM inserted in this unit's onboard 4G
-- modem. Null/empty means cellular fallback stays inert even if the
-- hardware is detected (matches the tasmota_plug_host precedent: presence
-- of the value is what turns the feature on, no separate enable flag).
alter table device_management
  add column cellular_apn text;
```

- [ ] **Step 2: Apply it to the Supabase project**

Use the `supabase` MCP tool's `apply_migration` (project id: the one this codebase's `SUPABASE_URL` points at — `qkqeysggrqhxizkdmbhx`), name `cellular_apn`, with the SQL from Step 1.
Verify: `list_tables` or a direct `select cellular_apn from device_management limit 1;` shows the column exists.

- [ ] **Step 3: Commit the migration record**

```bash
git add docs/migrations/cellular-apn.sql
git commit -m "Add cellular_apn column migration record"
```

### Task 4.2: Fetch `cellular_apn` in `fetchDeviceConfig()`

**Files:**
- Modify: `src/cloud.cpp` (`fetchDeviceConfig()`)
- Modify: `src/globals.h` / `src/main.cpp` (`extern String cellularApn;` / definition)

**Interfaces:**
- Consumes: the `dev` JSON object already parsed inside `fetchDeviceConfig()` (same pattern as the existing `tasmota_plug_host` handling).
- Produces: global `String cellularApn`, updated whenever the config changes.

- [ ] **Step 1: Add the global**

In `src/globals.h`: `extern String cellularApn;`
In `src/main.cpp`: `String cellularApn = "";`

- [ ] **Step 2: Parse it in `fetchDeviceConfig()`**

In `src/cloud.cpp`, inside `fetchDeviceConfig()`, alongside the existing `if (!dev["tasmota_plug_host"].isNull())` block, add:

```cpp
if (!dev["cellular_apn"].isNull())
  cellularApn = dev["cellular_apn"].as<String>();
```

- [ ] **Step 3: Build and verify**

Run: `pio run` — clean build.
Set `cellular_apn = 'ansar'` on `sf500_107888` via SQL (`update device_management set cellular_apn = 'ansar' where device = 'sf500_107888';`).
Flash, open serial monitor, wait up to `CONFIG_CHECK_INTERVAL` (10s) after boot.
Verify: no crash, no new log line expected yet (this task only stores the value — Task 4.3 uses it). Confirm via a temporary added `LOGF` if needed during bench testing, then remove before commit — don't ship debug-only logging.

- [ ] **Step 4: Commit**

```bash
git add src/cloud.cpp src/globals.h src/main.cpp
git commit -m "Fetch cellular_apn from device config"
```

### Task 4.3: The fallback state machine

**Files:**
- Modify: `src/globals.h` (add `enum NetworkTransport` + `extern NetworkTransport activeTransport;`)
- Modify: `src/main.cpp` (definition + the fallback/recovery logic in `loop()`)
- Modify: `src/mqtt_handler.cpp` (`reconnectMQTT()` picks the transport-appropriate client)

**Interfaces:**
- Consumes: `cellularCapable` (1.1), `cellularApn` (4.2), `connectCellularData()`/`cellularClient`/`cellularSecureClient` (2.1/3.1), `activeSecureClient` (3.1), existing `wifiState`/`WiFiState` machinery (`wifi_portal.cpp`).
- Produces: `enum NetworkTransport { TRANSPORT_WIFI, TRANSPORT_CELLULAR };` and `extern NetworkTransport activeTransport;`, readable by any later task (e.g. Task 5.2's observability payload).

- [ ] **Step 1: Add the transport enum + global**

In `src/globals.h`, near the `WiFiState` enum:

```cpp
enum NetworkTransport { TRANSPORT_WIFI, TRANSPORT_CELLULAR };
extern NetworkTransport activeTransport;
```

In `src/main.cpp`: `NetworkTransport activeTransport = TRANSPORT_WIFI;`

- [ ] **Step 2: Trigger cellular fallback when WiFi's retry budget is exhausted**

In `src/main.cpp`'s `loop()`, find the existing block that handles `WiFi.status() != WL_CONNECTED` (the one with the `for (int attempt = 1; attempt <= 3; attempt++)` retry loop that currently falls through to `startWiFiPortal(); return;` on exhaustion). Immediately before that `startWiFiPortal()` call, insert:

```cpp
if (cellularCapable && cellularApn.length() > 0 && activeTransport == TRANSPORT_WIFI)
{
  LOGLN("[Cellular] WiFi exhausted, attempting cellular fallback...");
  if (connectCellularData(cellularApn.c_str()))
  {
    activeTransport = TRANSPORT_CELLULAR;
    activeSecureClient = &cellularSecureClient;
    mqttClient.setClient(cellularClient);
    LOGLN("[Cellular] Fallback active");
  }
  else
  {
    LOGLN("[Cellular] Fallback attempt failed, staying WiFi-only (portal)");
  }
}
```

Leave the existing `startWiFiPortal(); return;` call immediately after this block untouched — the portal still opens in parallel per spec §4.3 point 2, regardless of whether cellular fallback succeeded.

- [ ] **Step 3: Keep retrying WiFi in the background, switch back on success**

In the same `loop()`, find the existing `PORTAL_SAVED_RETRY_INTERVAL_MS`-driven auto-retry block (currently inside `wifi_portal.cpp`'s update function, called from `loop()` while `wifiState == STATE_PORTAL`). This logic already fires on the same timer regardless of `activeTransport`, so no new timer is needed — add the recovery step where that block currently sets `wifiState = STATE_ONLINE` on a successful reconnect. In `src/main.cpp`, right after the existing WiFi-reconnected confirmation log line (`LOGLNS("[WiFi] Reconnected: " + WiFi.localIP().toString());`), add:

```cpp
if (activeTransport == TRANSPORT_CELLULAR)
{
  modem.gprsDisconnect();
  activeTransport = TRANSPORT_WIFI;
  activeSecureClient = &secureClient;
  mqttClient.setClient(espClient);
  LOGLN("[Cellular] WiFi recovered, switched back from cellular");
}
```

- [ ] **Step 4: `reconnectMQTT()` respects the active transport**

In `src/mqtt_handler.cpp`'s `reconnectMQTT()`, the function currently assumes `espClient` implicitly (via `mqttClient`'s constructor binding). Since `mqttClient.setClient()` is now called explicitly whenever the transport switches (Steps 2 and 3), `reconnectMQTT()` itself needs no client-selection logic — it already just calls `mqttClient.connect(...)` against whatever client was last set. Confirm this by reading the function; if it references `espClient` directly anywhere (rather than only through `mqttClient`), that reference needs removing so it can't silently override the cellular client. (As of this plan's writing it does not — verify this is still true before skipping this step.)

- [ ] **Step 5: Remove the `CELLTEST` manual-restore hack from Task 3.1**

The `activeSecureClient = &secureClient;` restore line added in Task 3.1 Step 6's `CELLTEST` handler is no longer correct now that real transport switching exists — a manual test firing this would fight the state machine. Remove that one restore line from the `CELLTEST` command (keep the rest of `CELLTEST` as a diagnostic that now just reflects whatever `activeTransport` currently is).

- [ ] **Step 6: Build, flash, verify the full cycle on the bench**

Run: `pio run` — clean build.
Flash to `sf500_107888`.
Bench test (physically block/allow WiFi, or use `AT+CIPSHUT`-style... simplest: temporarily set a wrong WiFi password via the portal, or unplug the office AP if that's feasible for a controlled window):
1. Cold boot with WiFi reachable: confirm `activeTransport` stays `TRANSPORT_WIFI` (no `[Cellular] Fallback active` log).
2. Make WiFi unreachable (wrong saved credentials, or physically move the unit out of range): confirm `[Cellular] WiFi exhausted, attempting cellular fallback...` then `[Cellular] Fallback active`, then confirm real MQTT data (`sf500/{lastSix}/data`) is still arriving on the public broker during this window.
3. Restore WiFi reachability: confirm `[Cellular] WiFi recovered, switched back from cellular` within one `PORTAL_SAVED_RETRY_INTERVAL_MS` window (5 min).
4. With `cellular_apn` cleared (empty string) and WiFi unreachable: confirm the device behaves exactly as it did before this feature existed (portal only, no cellular attempt) — this is the safety-default check from spec §5.

- [ ] **Step 7: Commit**

```bash
git add src/globals.h src/main.cpp src/mqtt_handler.cpp
git commit -m "Wire automatic WiFi<->cellular fallback state machine"
```

---

## Phase 4B — REVISED: independent cellular transport (added 2026-09-02)

**Why this supersedes the tail of Phase 4.** Phase 4 (commits through `9063a87`) made
cellular a *fallback that rides alongside the captive portal*. Bench work exposed two
problems the original spec/plan glossed over:

1. `isRegistered` is a RAM-only flag re-set to `false` on **every boot**, and it is only
   set by `registerDevice()` on the `wifiState == STATE_ONLINE` path. `loop()` bails at
   `if (!isRegistered) return;` before any periodic work. So a deployed unit that reboots
   (power cut / watchdog / OTA) while its site WiFi is down brings up cellular and then
   does nothing — cellular is never actually independent.
2. The AP opens on *every* WiFi failure, so a cellular-only site runs a
   `sf500-xxxxx` / `admin123` softAP + web server 24/7 for no reason.

User decision (2026-09-02): make cellular a **true independent transport** and only open
the AP when a human needs it.

### Model

- **"Online" is transport-agnostic.** The device is online when it has *any* uplink:
  WiFi STA associated, or a cellular PDP session up.
- **`bringOnline()`** — one function, extracted from the two existing init blocks in
  `setup()` and `loop()`. Runs once per boot the first time the device is online on any
  transport: time sync, `registerDevice()`, `markAppValid()`, boot activity log,
  `checkForOTAUpdate()` (skip on cellular — `ota.cpp` is WiFi-only, spec §4.4),
  `initSensors()`, `loadSmartCalibration()`, `loadRainResetState()`,
  `uploadSensorConfig()`, `fetchDeviceConfig()`, set `startupTime`.
- **Time:** WiFi → `syncTimeWithNTP()` unchanged. Cellular → `syncTimeFromModem()`
  (new): TinyGSM network time (`AT+CTZU=1` then `modem.getNetworkTime()` / `AT+CCLK`,
  fall back to `AT+QNTP` against `pool.ntp.org`), then `settimeofday()`. **Bench-verify
  on the EC801E — this AT surface is the one real unknown.**

### Transport state machine (loop)

- Down-switch: WiFi lost → 3× quick reconnect (~30s) → still down & modem & APN →
  `connectCellularData()` → `activeTransport = CELLULAR`, `mqttClient.setClient(cellularClient)`.
  No portal required.
- Cold boot, no WiFi: `setup()` tries saved creds 15s; on fail it sets a flag and returns
  **without** opening the portal — `loop()`'s state machine tries cellular first.
- Up-switch: standalone `retryWifiInBackground()` on its own `PORTAL_SAVED_RETRY_INTERVAL_MS`
  timer (moved out of `handlePortalLoop()`), independent of `portalMode`. On `WL_CONNECTED`
  held ≥15s (hysteresis) → `modem.gprsDisconnect()`, `activeTransport = WIFI`, swap MQTT
  client back. If `!isRegistered` (booted straight onto cellular) also call `bringOnline()`
  so it picks up NTP time + OTA.
- Failed cellular connect rate-limited to 1/60s (already done).

### AP-open decision — `bool shouldOpenPortal()`

Open the AP when ANY of:
1. `pendingWifiPortal` — `wifi_cmd: portal` received (now deliverable over cellular).
2. 3× power-cycle gesture: NVS `"boot"/"count"`; increment on each boot, zero it after
   10s uptime; count reaching 3 sets `forcePortal`. No hardware button needed.
3. No saved WiFi creds **and** no cellular uplink established (fresh unit that also can't
   reach cellular — must be provisioned on-site).
4. WiFi down **and** cellular unavailable (no modem / no APN / connect failed) — the
   genuinely-offline case, same as pre-feature behaviour.

Do **not** open the AP when WiFi is down but cellular is carrying traffic and creds
exist — the device is online, nothing to configure. (Optional: if the AP is open and
later none of 1-4 hold and no station connected for ~10 min, close it.)

### Tasks

- [x] **R1** — `bringOnline()` extracted (commit ea8cfa9).
- [x] **R2** — `syncTimeFromModem()` — modem network time (AT+QLTS/CTZU, QNTP fallback);
  also `setenv("TZ","UTC-8")` so `localtime_r` yields +08:00 (commit ea8cfa9 + 5d8c524).
- [x] **R3** — standalone `retryWifiInBackground()`; down-switch no longer opens the portal
  (commit ea8cfa9).
- [x] **R4** — `portalRequestedByHuman()` (early) + `shouldOpenPortalOffline()` (late) +
  3× power-cycle NVS gesture (commit ea8cfa9).
- [x] **R5** — 4G-only bench pass DONE on sf500_107888: cold boot with no WiFi → registers +
  inits + modem time → steady MQTT + periodic tasks over cellular → `data` carries
  `cellular:{active,apn,signal}` → `wifi_cmd portal` and 3× power-cycle both open the AP →
  live dashboard relay on/off + timer control works over cellular.
- [x] **Lost-link recovery** (commits 9066236, 79baa7b) — `isGprsConnected()` health check
  every 20s; dead >40s → drop to WiFi/portal; `connectCellularData()` reboots the modem
  (`AT+CFUN=1,1`, ≤1/2min) on failure so a re-inserted SIM is re-scanned; `getSimStatus()`
  fast-fail for absent/PIN-locked. Verified real SIM pull + re-insert with no WiFi.
- [x] **Relay-card parity** (commit 5d8c524) — `publishRelayStatus()` moved out of the
  `if (WiFi.status()==WL_CONNECTED)` guard in `writeRelay()` so relay commands are
  confirmed + the card timer renders on cellular.

### Still to do (needs sf500_107888 back on a WiFi antenna)

- [ ] **R6a** — Healthy-WiFi regression check: fresh flash, onboard over WiFi, confirm the
  WiFi path is unchanged (bringOnline from setup(), lazy detect never fires, no fallback,
  no AP).
- [ ] **R6b** — WiFi → cellular → WiFi live cycle: kill the AP → `Fallback active` (AP must
  stay closed) → all periodic traffic over cellular → restore WiFi → switch-back after 15s.
- [ ] **R6c** — Task 5.3 soak: 30+ min on cellular with sensor/schedule activity, watch for
  the v1.2.5 stack-canary signature (`Guru Meditation` / `Stack canary`). `bringOnline()`
  now runs on `loopTask` on the cellular path too — extra stack pressure to watch.
- [ ] **R6d** — Bump `FIRMWARE_VERSION` (check `git tag -l "v1.2.*"` for the next number),
  commit, then `superpowers:finishing-a-development-branch` (merge `cellular-fallback` →
  `main`, tag).

### Dashboard follow-up (separate repo: `Dashboard/sf500`)

- [ ] **Header connectivity badge** — when the device is connected via cellular, swap the
  header's WiFi signal icon for a 4G/LTE icon. The `sf500/{lastSix}/data` payload already
  carries everything needed: `cellular:{active:true, apn, signal}` is present and the
  `wifi:{}` block is absent while on cellular. So: if `data.cellular?.active` → render the
  4G icon (optionally with `cellular.signal` as bars, CSQ 0–31); else the existing WiFi
  icon from `data.wifi`. Small, presentational; no firmware change.

### Cleanup once merged

- `CELLDETECT` / `CELLTEST` / `CELLAPN` / `CELLKILL` / `CELLOK` serial commands are
  bench helpers — decide whether to keep them (useful for field diagnostics) or strip.
- `sf500_107888` still has `cellular_apn = 'ansar'` in `device_management` and in NVS.

---

## Phase 5: Certificate pinning, observability, soak

Goal: replace the spike's `setInsecure()` shortcut with real certificate verification, add dashboard visibility, and soak-test before this goes near a second device.

### Task 5.1: Real CA certificate for the cellular TLS path

**Files:**
- Create: `src/cellular_cert.h`
- Modify: `src/cellular.cpp` (use `setCACert()` instead of `setInsecure()`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `CELLULAR_CA_CERT` (a `const char*` PEM constant), used only inside `cellular.cpp`.

- [ ] **Step 1: Obtain the correct root CA PEM**

Fetch the certificate chain Supabase's edge actually presents (it's Cloudflare-fronted): `openssl s_client -connect qkqeysggrqhxizkdmbhx.supabase.co:443 -showcerts </dev/null 2>/dev/null | openssl x509 -outform PEM` for the leaf, and trace to the issuing root (walk the chain with `-showcerts` and match the top-most cert's issuer against a standard root store, e.g. `ISRG Root X1` or whatever Cloudflare is issuing through at fetch time — do not assume ahead of time, confirm against the live chain).

- [ ] **Step 2: Create `src/cellular_cert.h`**

```cpp
#pragma once

// Root CA for qkqeysggrqhxizkdmbhx.supabase.co, fetched via:
//   openssl s_client -connect qkqeysggrqhxizkdmbhx.supabase.co:443 -showcerts
// Re-fetch and update this if Supabase/Cloudflare rotates their chain.
static const char* CELLULAR_CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
<paste the actual root cert PEM from Step 1 here>
-----END CERTIFICATE-----
)EOF";
```

- [ ] **Step 3: Use it in `cellular.cpp`**

In `src/cellular.cpp`, replace the `cellularSecureClient.setInsecure();` line from Task 3.1 with:

```cpp
#include "cellular_cert.h"
// ...
cellularSecureClient.setCACert(CELLULAR_CA_CERT);
```

- [ ] **Step 4: Build, flash, verify**

Run: `pio run` — clean build.
Flash to `sf500_107888`, force cellular fallback (Task 4.3 Step 6's method).
Verify: Supabase calls over cellular still succeed (same `[CONFIG] ...` success lines as Task 3.1's verification) — now with real certificate verification instead of `setInsecure()`. If the handshake fails, the fetched root cert doesn't match the live chain — re-run Step 1 and confirm you captured the actual root, not an intermediate.

- [ ] **Step 5: Commit**

```bash
git add src/cellular_cert.h src/cellular.cpp
git commit -m "Replace setInsecure() with real CA cert verification for cellular TLS"
```

### Task 5.2: Observability — `cellular` block in the data payload

**Files:**
- Modify: `src/sensors.cpp` (the data-payload JSON builder, same function that already adds the `plug:{...}` block)

**Interfaces:**
- Consumes: `activeTransport`, `modem.getSignalQuality()`, `cellularApn` (all from earlier tasks).
- Produces: an additional `cellular:{...}` key in the existing `sf500/{lastSix}/data` MQTT payload, only when relevant (mirrors how `plug:{...}` is only added `if (tasmotaPlugEnabled)`).

- [ ] **Step 1: Add the JSON block**

In `src/sensors.cpp`, in the same function/location that builds `doc["plug"]` (guarded by `if (tasmotaPlugEnabled)`), add immediately after it:

```cpp
if (cellularCapable)
{
  JsonObject cellObj = doc.createNestedObject("cellular");
  cellObj["active"] = (activeTransport == TRANSPORT_CELLULAR);
  cellObj["apn"]    = cellularApn;
  if (activeTransport == TRANSPORT_CELLULAR)
    cellObj["signal"] = modem.getSignalQuality();
}
```

Add `#include "cellular.h"` to `sensors.cpp` if not already present (needed for `modem`, `NetworkTransport`).

- [ ] **Step 2: Check the JSON buffer still fits**

The buffer was already bumped 1024→1536 in v1.2.4 (see `project_stack_canary_crash_v125` memory — this is the exact buffer implicated in that incident, so don't grow it carelessly). The `cellular` object added here is small (~40-60 bytes serialized). Build with `pio run` and check the reported RAM/flash usage hasn't meaningfully changed; if buffer overflow becomes a concern in testing (truncated/malformed JSON on the wire), that's a stop-and-reassess signal, not something to silently paper over by growing the buffer again without checking stack margin first.

- [ ] **Step 3: Build, flash, verify**

Run: `pio run` — clean build.
Flash to `sf500_107888`, force cellular fallback.
Subscribe to `sf500/{lastSix}/data` (`mosquitto_sub -h broker.emqx.io -p 1883 -t 'sf500/<lastSix>/data' -C 1 -v`).
Verify: payload includes `"cellular":{"active":true,"apn":"ansar","signal":<number>}`.

- [ ] **Step 4: Commit**

```bash
git add src/sensors.cpp
git commit -m "Add cellular status to the MQTT data payload"
```

### Task 5.3: Final soak test + version bump

**Files:**
- Modify: `src/config.h` (`FIRMWARE_VERSION`)

- [ ] **Step 1: Full soak on the bench unit**

Repeat Task 4.3 Step 6's full cycle (WiFi→cellular→WiFi, and the empty-APN safety check) at least twice back to back, plus: leave the unit running on cellular for 30+ minutes with normal sensor-read/auto-dosing/schedule-check activity happening, watching for the stack-canary crash signature (`Guru Meditation`, `Stack canary watchpoint`) from `project_stack_canary_crash_v125` — this feature adds real code to the same `loopTask` that already had a stack-margin incident once.
Verify: no crashes, no unexpected reboots, `plug:{...}`/`cellular:{...}` payload fields both correct throughout if the plug feature is also active on the test unit.

- [ ] **Step 2: Bump the firmware version**

In `src/config.h`, bump `FIRMWARE_VERSION` to the next patch version after whatever is live at implementation time (check `git tag -l "v1.2.*"` for the current latest before choosing the number).

- [ ] **Step 3: Commit**

```bash
git add src/config.h
git commit -m "Bump firmware version for cellular fallback release"
```

---

## Self-Review Notes

- **Spec coverage:** §3 (hardware facts) → Task 1.1. §4.1 (auto-detect) → Task 1.1. §4.2 (Client abstraction) → Tasks 2.1/3.1. §4.3 (state machine) → Task 4.3. §4.4 (transport-per-traffic-type table) → Tasks 2.1 (MQTT)/3.1 (HTTPS). §5 (`cellular_apn` config) → Tasks 4.1/4.2. §6 (cert pinning) → Task 5.1. §7 (observability) → Task 5.2. §8 risks are either resolved in-plan (cert pinning) or explicitly called out as bench-test checks (Task 4.3 Step 6's empty-APN case; SIM-absent/locked behavior is NOT separately covered — flagging this as a real gap: no task tests a present-but-SIM-less or PIN-locked modem. If that scenario matters before shipping, add a Task 4.3.5 bench check for it; deferring here since this session's test SIM was always ready.) §9 (testing discipline) → woven through every phase's Verify step plus Task 5.3's soak.
- **Placeholder scan:** clean except Task 5.1 Step 2's cert PEM, which is a genuine external artifact (the actual certificate bytes) that can't be known until fetched live — not a planning placeholder, an explicit fetch-and-paste step.
- **Type consistency:** `NetworkTransport`/`activeTransport` (4.3) used identically in 5.2. `activeSecureClient` (3.1) used identically in 4.3/5.1. `cellularApn` (4.2) used identically in 4.3/5.2. `detectCellularModem()`/`connectCellularData()` signatures (1.1/2.1) match their call sites in 4.3.
