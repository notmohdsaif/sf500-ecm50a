# Offline Autonomy + Data Retention — Design Spec

Status: approved (design), implementation deferred pending cellular-fallback landing
Target device: `sf500_107888` only (ECM50-A09 bench unit). Not a fleet feature.
Base branch: `offline-autonomy`, cut from `main` @ v1.2.5. Intended to be rebased
onto `cellular-fallback` before any code lands (see §9).

---

## 1. Problem

The controller cannot operate without connectivity. Three concrete failures:

1. **Control logic halts on connectivity loss.** When WiFi is lost for more than
   the reconnect budget (`3×10s`), `loop()` calls `startWiFiPortal()` and every
   subsequent iteration returns early at
   `if (portalMode) { handlePortalLoop(); return; }`. From that point
   `readSensors()`, `checkAutoDosing()`, `checkRefillCutoff()` and
   `checkSchedules()` never run. Auto-dosing, water-in detection and refill
   cutoff all stop until connectivity returns.

2. **No cold-start without connectivity.** `setup()` only registers and loads
   config `if (wifiState == STATE_ONLINE)`. With no network at boot the device
   goes to the portal, `isRegistered` stays `false`, and `loop()` does
   `if (!isRegistered) { delay(1000); return; }` indefinitely. A power cut during
   an outage takes the farm down until a human or the network intervenes.

3. **No data retention.** `uploadSensorReadings()` (5 min), `logDeviceActivity()`,
   `writeRelay()`→`relay_metrics`, `logR3Transition()`→`relay_metrics` and
   `updateDeviceStatus()` are all fire-and-forget. Everything emitted during an
   outage is lost. Several of these paths (`logDeviceActivity()` in particular)
   do not check link state before an `http.begin(secureClient, …)` call, so while
   the device is associated-but-offline every emit blocks the loop for several
   seconds on a doomed TLS handshake.

## 2. Goal / Scope

Locked with the requester:

- Auto-dosing, water-in (WL) detection and refill cutoff **keep running through
  any connectivity loss** — WiFi down, 4G down, or both — with no dependence on
  reaching the broker or Supabase.
- **Cold-start offline**: a power cut during a blackout must bring the device
  back up and resume auto-dosing + water-in detection autonomously, from
  locally-persisted config and a coarse clock, with zero connectivity.
- **Log and sensor data are buffered to the SD card** while offline and replayed
  to Supabase, in order and with real capture timestamps, on recovery.
- Plug actuation during a true blackout (no LAN at all) is **out of scope** — the
  existing MQTT and LAN-HTTP plug transports are unchanged; when neither the
  broker nor a LAN plug is reachable the plug is simply unavailable.
- **`sf500_107888` only.** No fleet rollout, no dashboard UI work.

Out of scope: hardware RTC; Ethernet/W5500 path; buffering `device_management`
heartbeat/status; any dashboard change; fleet firmware.

## 3. Hardware facts

Source: `ECM50-A_Pin_List_V1.0.xlsx` + the ESP32-S3R8 datasheet.

| Signal | Pin | Notes |
|---|---|---|
| SD_CS | GPIO1 | microSD chip select |
| SPI2_SCK | GPIO10 | microSD clock |
| SPI2_MISO | GPIO9 | microSD data out |
| SPI2_MOSI | GPIO46 | microSD data in |
| SD_CD | GPIO3 | card-detect (active state to be confirmed on the bench) |

The microSD slot is on **SPI2**, a dedicated bus. Ethernet (W5500) is on SPI1
(GPIO47/12/13/11/14/48) and the 4G modem is on UART2 (GPIO38/39/40), so SD I/O
contends with neither.

Card: a 16 GB microSD is fitted in `sf500_107888`. Must be formatted **FAT32**.

The board has **no battery-backed RTC**. The ESP32 internal RTC keeps time only
while powered; it does not survive a full power cut.

## 4. Design principle — split the control plane from the connectivity plane

Today the two are entangled. The fix is to make the control tasks
(`readSensors`, `checkAutoDosing`, `checkRefillCutoff`, `checkSchedules`,
`checkRelayTimers`, SD flush) run **every loop iteration, unconditionally**, as
long as a configuration (from cloud or local storage) has been loaded — and to
turn connectivity into a background service that never blocks or gates them.

### 4.1 Run-state model

Add an explicit enum for observability (logged to serial, and to the
`sf500/{lastSix}/data` MQTT payload when online):

```
enum RunState {
  RS_BOOT,               // pre-config
  RS_ONLINE,             // config from cloud, uplink healthy
  RS_OFFLINE_AUTONOMOUS, // running on local config, no uplink
  RS_PROVISIONING        // captive portal open (may still be autonomous underneath)
};
```

The control tasks run in `RS_ONLINE` **and** `RS_OFFLINE_AUTONOMOUS`, and also
while `RS_PROVISIONING` if a config is loaded. `RS_BOOT` is the only state in
which they do not run, and boot resolves to one of the other three within one
config-load attempt.

### 4.2 Portal no longer gates control

- **Connectivity loss never calls `startWiFiPortal()`.** The captive portal is a
  provisioning tool, opened only on: (a) first boot with no saved WiFi
  credentials, (b) an explicit `wifi_cmd: portal` MQTT command, (c) a
  `KEY_BOOT` (GPIO0) long-press.
- When the portal *is* open, `handlePortalLoop()` becomes a co-task: `loop()`
  services it and then falls through to the control tasks instead of
  `return`-ing. (The cellular-fallback branch already moves in this direction —
  see §9.)

### 4.3 Non-blocking reconnect

- WiFi reconnect: replace the inline `3 × (10 s blocking)` loop with a single
  attempt per `loop()` pass plus a back-off timer. Return immediately.
- MQTT reconnect: replace the `5 × (connect + 2 s)` loop in `reconnectMQTT()`
  with one attempt per invocation, back-off timer, immediate return.
- The control tasks keep running between attempts.

### 4.4 `haveUplink()` — the single connectivity signal

One debounced global, `g_uplinkOk`, updated by:

- MQTT connect / disconnect transitions.
- The result of every Supabase HTTP call (`true` on a 2xx, `false` on a
  connect/timeout failure).
- Link-layer state (`WiFi.status()`, and — post-rebase — cellular data state).

`haveUplink()` returns `g_uplinkOk`. **Every** network function early-returns
immediately when it is `false`, except the periodic reconnect probe itself. This
removes the associated-but-offline TLS stalls described in §1.3.

## 5. Local configuration persistence

Two tiers.

### 5.1 NVS (Preferences) — dosing-critical scalars

Namespace `"cfg"`. Written only when a value actually changes (`fetchDeviceConfig()`
already computes a `changed` flag; extend it to cover every field below):

| Key | Source column |
|---|---|
| `autoDosing` | `device_management.auto_dosing` |
| `ecTarget` | `ec_target` |
| `dosingTime` | `dosing_time` |
| `autoMixing` | `mixing_pump` |
| `smartDosing` | `smart_dosing` |
| `minWlDosing` | `min_wl_dosing` |
| `plugMode` | `tasmota_plug_mode` |
| `refillCutoffMm` | derived from `sensor_config.max_thres` |
| `plugEnabled` | `tasmota_plug_enabled` |
| `ecSensorId` / `wlSensorId` / `ambSensorId` / `rainSensorId` + found flags | sensor scan |
| `lastRainResetDay` | local |

Smart-calibration state already persists via `loadSmartCalibration()` /
`saveSmartCalibration()` — unchanged.

### 5.2 SD card — schedules + full mirror

`/config/device.json` (a full snapshot of the above plus plug host/topic) and
`/config/schedules.json` (the parsed `schedules[]` array). Atomic writes
(temp file + rename). Written on a confirmed change only.

### 5.3 Boot / load order

1. Mount SD (best-effort).
2. Try Supabase `registerDevice()` + `fetchDeviceConfig()` + `fetchSchedules()`
   exactly as today.
3. On failure (no uplink): load NVS scalars, then overlay `/config/schedules.json`
   if present. Set `RunState = RS_OFFLINE_AUTONOMOUS`.
4. Start the control loop. `isRegistered` no longer gates it.
5. Once an uplink appears later, the normal `fetchDeviceConfig()` /
   `fetchSchedules()` cadence resumes and overwrites local state as usual.

A card-less unit still cold-starts dosing from NVS; it just has no schedules
while offline.

## 6. Coarse clock across reboot

- Persist `time(nullptr)` to `/state/clock` (and NVS as a fallback) every 5 min
  and on graceful hooks.
- On an offline boot with no NTP, seed the system clock (`settimeofday()`) from
  the stored epoch. Time is then never earlier than truth and wrong by at most
  the outage duration.
- Records buffered under a seeded (non-NTP) clock carry `approx: true`.
- When NTP later succeeds, log the correction delta to `activity_log`.

**Documented limitation:** time-of-day schedules run approximately during a
cold-boot blackout (or not at all if the stored clock was never initialised).
EC auto-dosing and WL detection are time-independent and unaffected.

## 7. SD data buffering + backfill

### 7.1 Journal format

Append-only NDJSON at `/buffer/pending.ndjson`, one record per line:

```json
{"t":1725283200,"approx":false,"tbl":"sensor_metrics","row":{ … }}
```

`tbl` is one of `sensor_metrics`, `activity_log`, `relay_metrics`. `row` is the
exact JSON object the existing code already builds for that table's POST.

### 7.2 Always-buffer-then-drain

`uploadSensorReadings()`, `logDeviceActivity()`, the `relay_metrics` POST in
`writeRelay()`, and `logR3Transition()` stop POSTing directly. They append a
journal line. A single **drain task** owns all POSTing. Benefits: one code path,
and a *failed* online POST is retried rather than lost.

If no SD card is present, these fall back to the current behaviour
(direct POST when `haveUplink()`, drop otherwise).

Heartbeat / `device_management` status and server-derived `history_24h` are not
journalled — they are latest-state, and the `last_heartbeat_at` gap is itself the
outage signal.

### 7.3 Drain task

- Runs from `loop()` only when `haveUplink()` and not inside a dose-critical
  window (`AUTO_DOSING`, `AUTO_PRE_MIX`, `AUTO_POST_MIX`).
- Reads a small batch (5–10 lines), POSTs (array POST where the table supports
  it — `sensor_metrics` already does), then advances a byte offset in
  `/buffer/offset`.
- Bounded: small batches, short HTTP timeouts, at most one batch per few seconds,
  so a reconnect backfill never starves MQTT keepalive or the 60 s task
  watchdog.
- The file is compacted (consumed prefix truncated) or rotated once the offset
  or file size passes a threshold.

### 7.4 Retention

Total journal size capped (default 256 MB). On overflow, oldest
`sensor_metrics` records are dropped first; `activity_log` and `relay_metrics`
(the audit trail) are preserved.

### 7.5 Timestamp column — the one non-firmware change

Backfilled rows must not collapse to the reconnect instant. Add a nullable
`recorded_at timestamptz` to `sensor_metrics`, `activity_log` and
`relay_metrics`, and include it in every firmware insert (live and backfilled).
The dashboard can ignore the column for now; it is required for the retained
data to be meaningful. Migration recorded under `docs/migrations/`.

## 8. Risks

- **Stack.** `SdFat` + ArduinoJson + mbedTLS all run on `loopTask` (20480 bytes
  after the v1.2.5 fix). The high-water mark must be re-measured with SD active;
  a further bump or a dedicated SD-I/O task may be needed. This is the same class
  of failure as the v1.2.5 stack-canary crash — treat it as a first-class test,
  not an afterthought.
- **Power loss mid-write.** Config writes use temp-file + rename. The NDJSON
  journal tolerates a torn trailing line — the replay parser skips an
  unparseable last record.
- **Card removed at runtime.** Every SD op is guarded and checks the CD pin;
  the code falls back to no-card behaviour without crashing.
- **Watchdog interaction.** Backfill batches must complete well within the 60 s
  task watchdog even on a slow uplink.
- **Card format.** 16 GB card must be FAT32 — note in bench provisioning.

## 9. Relationship to the cellular-fallback branch

`docs/superpowers/specs/2026-09-02-cellular-fallback-design.md` §4.3 step 4 says
that if cellular also fails the device should "sit in portal, keep retrying
WiFi." **This design overrides that terminal state**: the fallback terminal
state becomes `RS_OFFLINE_AUTONOMOUS` — control loop running, SD buffering
active, both transports retried in the background — never a portal halt.

At the time this spec was written the `cellular-fallback` branch had in-flight,
uncommitted work touching `loop()`'s portal path, `relay.cpp` serial commands and
`globals.h` — the same lines this feature changes. Therefore:

- Docs (this spec + the implementation plan) land first on `offline-autonomy`
  off `main`, with no code.
- Code implementation begins only after the cellular-fallback work is committed
  and stable, at which point `offline-autonomy` is rebased onto it and the plan's
  phases execute against that base.
- Phase 1 of the plan must re-read the cellular-fallback design and code and
  extend its transport/`activeTransport` model rather than duplicate it —
  `haveUplink()` should fold in the cellular data state, and `RunState` should
  compose with `activeTransport`, not replace it.

## 10. Testing plan

Bench-only, on `sf500_107888` tethered over USB serial. No second device.

- **Phase 0** — SD mounts on SPI2; size and free space logged; survives a card
  reseat; CD pin polarity confirmed.
- **Phase 1** — kill WiFi and 4G mid-dose: dosing and WL detection continue on
  serial; the captive portal does not open; reconnect attempts do not stall the
  loop (verify loop cadence stays ~1 s).
- **Phase 2** — cold-boot with no connectivity: device resumes auto-dosing and
  WL detection from stored config within one boot; schedules load from SD; clock
  seeds from `/state/clock`.
- **Phase 3** — run offline for 1 h: journal line counts match the dose /
  sensor-batch / activity events seen on serial.
- **Phase 4** — restore connectivity: journalled rows arrive in Supabase in
  order, with `recorded_at` set to real capture time; the live path is
  uninterrupted during backfill.
- **Phase 5** — hardening: low-heap emit, card pulled mid-write, power cut during
  a config write (atomic rename holds), stack high-water with SD + TLS + JSON
  concurrent, 24–48 h soak with repeated induced outages. Explicit stack-margin
  check per the v1.2.5 incident.

Only a clean Phase 5 soak closes the work.
