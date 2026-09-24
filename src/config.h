#pragma once

// =====================================================
// CONFIG.H — All compile-time constants
// =====================================================

// IMPORTANT:
// Secrets (AP password / Supabase key) are defined in `config_secrets.h`,
// which is intentionally NOT committed to git.
#include "config_secrets.h"

// WiFi Portal Settings
#define AP_CH 6
#define DNS_PORT 53
#define PORTAL_DOMAIN "sfconnect.com"
#define PORTAL_ROOT "http://sfconnect.com/"
#define CONNECT_TIMEOUT_MS 30000UL
#define AUTO_CONNECT_TIMEOUT_MS 15000UL
#define PORTAL_SAVED_RETRY_INTERVAL_MS (5UL * 60UL * 1000UL) // retry saved creds every 5 min while portal is open

// Non-blocking WiFi down-switch recovery (loop(), tickWifiReconnect()). The
// control plane keeps its 1s cadence during a WiFi outage because none of this
// blocks — association completes across later loop iterations.
#define WIFI_RECONNECT_KICK_MS   20000UL   // re-issue WiFi.begin() this often while the link is down
#define WIFI_RECONNECT_CYCLE_MS  120000UL  // full radio power-cycle this often while down (recovers a wedged supplicant after the AP vanished and returned)
#define WIFI_DOWN_FALLBACK_MS    30000UL   // link down this long -> (re)try cellular fallback (no-op on non-4G boards; matches the old blocking loop's ~30s to first fallback)
#define WIFI_PORTAL_LAST_RESORT_MS 180000UL  // genuinely-offline (has creds, no cellular) portal is a last resort, gated behind this much downtime so it doesn't preempt tickWifiReconnect() (see shouldOpenPortalOffline() — D1 fix, was ~1s -> measured 300s reconnect)
#define WIFI_RECONNECT_STABLE_MS 15000UL     // require this long of continuous WL_CONNECTED before declaring the outage over — a flapping AP that briefly re-associates would otherwise reset the whole ladder (wifiDownSince/wifiLastCycle) every blip, starving the last-resort portal gate above of the sustained downtime it needs to ever fire. Matches the existing 15s cellular up-switch debounce (wifiUpSince) below.

// Supabase
#define SUPABASE_URL "https://qkqeysggrqhxizkdmbhx.supabase.co"

// MQTT
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883

// Hardware Pins (ECM50-A)
#define RX_PIN 18     // RS485 UART1 RX
#define TX_PIN 17     // RS485 UART1 TX
#define RELAY1_PIN 15 // DO1 - Dosing Pumps A+B
#define RELAY2_PIN 16 // DO2 - Mixing Pump

// 4G modem (Quectel EC801E-CN, present only on the ECM50-A09 4G board variant).
// Detection is lazy — the modem is only powered/probed the first time WiFi
// fails, so non-4G boards never pay the probe cost. See src/cellular.cpp.
#define MODEM_PWR_PIN 38 // HIGH = modem powered
#define MODEM_TX_PIN  39 // ESP32 TX -> modem RXD
#define MODEM_RX_PIN  40 // ESP32 RX <- modem TXD
#define MODEM_BAUD    115200
// While on cellular, if MQTT (plain TCP to the broker) stays unreachable this
// long, treat the data session as dead even when modem.isGprsConnected() still
// reports it up (network-side "zombie" teardown) and force a transport rebuild.
#define CELL_UPLINK_DEAD_MS (3UL * 60UL * 1000UL)

// Sensor Configuration — ID ranges match admin panel SENSOR_TYPES
#define EC_SENSOR_DEFAULT 3 // Try this ID first before range scan
#define EC_SCAN_START 3     // EC sensor scan range fallback
#define EC_SCAN_END 5
#define WL_SCAN_START 13 // Water level scan range (IDs 13–15)
#define WL_SCAN_END 15
#define AMB_SCAN_START 20 // Ambient sensor scan range (IDs 20–22)
#define AMB_SCAN_END 22
#define AMB_REG_HUMID 500 // Holding register: humidity
#define AMB_REG_TEMP  501 // Holding register: temperature
#define AMB_REG_LUX   507 // Holding register: lux
#define RAIN_SCAN_START 30 // Rain sensor (single fixed ID)
#define RAIN_SCAN_END 30
#define RAIN_REG_TIPS 0    // Holding register: rainfall in 0.1mm increments
#define MET_SENSOR_ID 35    // Weather station — fixed ID, confirmed via bench test (not a scan range)
#define MET_DETECT_RETRIES 3 // boot-time detection attempts — single-shot was vulnerable to bus
                              // contention from the EC/WL/Ambient/Rain scans running immediately
                              // before it on the same shared RS485 bus
#define MET_REG_BASE 500    // Block read start: wind speed/force/dir(x2)/humidity/temp/noise/pm2.5/pm10
#define MET_REG_COUNT 12    // Registers 500-511 inclusive — widened from 9 (500-508) to pull the lux
                             // register (511) into this same transaction instead of a second one
#define MET_REG_LUX_OFFSET 11 // index of register 511 within the MET_REG_COUNT block read above
                               // (low 16 bits of the 32-bit precise Lux value, raw = lux, no scaling —
                               // coarser reg 512's x100-Lux single-register reading was too low-
                               // resolution at indoor light levels). NOTE: only the low word is read —
                               // the high word's register address isn't documented anywhere in this
                               // codebase, so values above 65535 lux (full sun, common outdoors) still
                               // wrap. Needs the weather station's Modbus register map to fix properly.

// Timing Constants (milliseconds)
#define SENSOR_READ_INTERVAL 1000UL
#define SENSOR_UPLOAD_INTERVAL (5UL * 60UL * 1000UL) // 5 minutes
#define STATUS_UPDATE_INTERVAL 30000UL
#define CONFIG_CHECK_INTERVAL 10000UL
#define SCHEDULE_CHECK_INTERVAL 1000UL
#define SCHEDULE_FETCH_INTERVAL 60000UL

// EC Automation — thresholds
#define EC_SAMPLES 30
#define EC_HYSTERESIS 0.10f
#define EC_MIN_PLAUSIBLE 0.05f // below this, a live nutrient tank can't legitimately read — treat as sensor/Modbus fault, not real EC
#define EC_IMPLAUSIBLE_CONFIRM_MS (120UL * 1000UL) // sustained below-floor readings this long are trusted as real (e.g. a genuine plain-water refill), not a fault
#define EC_CEILING_MARGIN 0.30f
#define DOSE_RESPONSE_THRESHOLD 0.02f
#define MAX_INEFFECTIVE_DOSES 3
#define MAX_TOTAL_INEFFECTIVE_DOSES 5 // hard ceiling regardless of streak-cumulative resets —
                                      // caps how long noise-driven "progress" can defer the alarm
#define EC_CEILING_HOLD_TIMEOUT (30UL * 60UL * 1000UL) // 30 min sustained ceiling hold → alarm
#define EC_CEILING_LOG_INTERVAL  (5UL * 60UL * 1000UL) // 5 min minimum between ceiling-hold log entries

#define REFILL_CUTOFF_PCT 0.95f       // Refill mode: stop R3 once WL reaches this fraction of tank max
#define FERTIGATE_EC_TOLERANCE 0.10f  // Fertigate mode: block R3 unless |EC - ec_target| is within this
#define REFILL_RESET_DELAY 15000UL    // Settle time after R3 turns off before re-checking WL for the
                                       // AUTO_ALARM refill-recovery reset (checkAutoDosing)
#define WL_JUMP_THRESHOLD_MM 10        // WL rise (mm) since baseline that counts as "water was added" —
                                       // plug-independent refill signal for sites with no Tasmota plug (or a
                                       // manual top-up at a plug site). Validated against production data:
                                       // real refills move WL 29-440mm; a single dose moves it ~0.2mm.

// Tasmota Plug (R3) — local HTTP transport
// Used when device_management.tasmota_plug_host is set: the controller drives the
// plug over its LAN /cm API instead of MQTT (for sites where the plug can't reach
// the broker). See docs/plug-http-control.md.
#define PLUG_HTTP_TIMEOUT_MS         2000UL   // TCP connect + read cap per call — stays well under MQTT keepalive
#define PLUG_HTTP_POLL_INTERVAL      10000UL  // how often the controller re-reads the plug's Power state
#define PLUG_HTTP_RETRY_DELAY_MS     300UL    // one immediate retry after a failed command GET
#define PLUG_HOST_RERESOLVE_INTERVAL (5UL * 60UL * 1000UL) // re-resolve the mDNS/DNS name at most this often
#define PLUG_OFFLINE_FAIL_STREAK     2        // consecutive failed polls before plug.online is reported false

// Smart Dosing
#define SMART_CAL_DURATION 60        // calibration dose length (seconds) — floor only; scales with dosingTime
#define SMART_MIN_DOSE 5             // minimum computed dose (seconds)
#define SMART_MAX_DOSE 300           // maximum computed dose cap (seconds)
#define SMART_ERROR_THRESHOLD 0.30f  // re-calibrate if prediction error > 30%
#define DOSE_ABORT_MARGIN 0.05f      // abort active dose if EC exceeds target by this margin (mS/cm)
#define SMART_WL_FACTOR_MAX 1.5f     // cap upward WL correction factor (prevents over-extension at high WL)
#define SMART_CAL_MAX_RETRIES 5      // max consecutive calibration failures before alarm
#define SMART_RATE_MAX 0.02f         // P5 upper bound on ec_rate (mS/cm/s) — used as conservative floor
                                     // in headroom cap so a stale underestimated rate cannot bypass the cap

// EC Automation — timing (milliseconds)
#define INITIAL_WAIT 60000UL
#define PRE_MIX_DURATION 15000UL
#define POST_MIX_DURATION 30000UL
#define POST_DOSE_DELAY_NO_MIX 120000UL
#define POST_DOSE_DELAY_MIX 60000UL
#define STABILISE_SKIP_NO_MIX 15
#define STABILISE_SKIP_MIX 10
// Hard bound on time spent in AUTO_STABILISING. Both the skip counter and the
// post-skip sample count only advance on a successful, plausible EC read
// (see readSensors()'s ecReadOk gate) — a flaky probe (failed Modbus reads or
// a sustained implausible value) can stall both indefinitely with no other
// exit condition. Past this many ms, give up on the response check for this
// cycle and resume SAMPLING rather than hanging until a manual reset.
#define STABILISE_TIMEOUT_MS (600UL * 1000UL)
// Bound on time AUTO_SAMPLING can sit without a single full window of usable EC
// data (ecReadingCount stuck below EC_SAMPLES). No relay/dose is pending here,
// so it's not unsafe to keep waiting — this exists purely so a probe that never
// produces usable reads gets surfaced instead of idling silently forever.
#define SAMPLING_STALL_TIMEOUT_MS (1200UL * 1000UL)

// Schedules
#define MAX_SCHEDULES 100

// Firmware version — must match GitHub release tag (without 'v' prefix)
#define FIRMWARE_VERSION "1.2.9"

// GitHub OTA repository
#define GITHUB_USER "notmohdsaif"
#define GITHUB_REPO "sf500-ecm50a"

// OTA check interval
#define OTA_CHECK_INTERVAL (6UL * 60UL * 60UL * 1000UL)

// Remote serial logging over MQTT — comment out to disable entirely
#define ENABLE_OTA_LOGS
#define LOG_PUBLISH_INTERVAL 5000UL
