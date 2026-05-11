# WiFi Reconnect — Implementation Notes

## Overview

The firmware has two layers of WiFi recovery:

1. **Runtime reconnect** — fires immediately when `WiFi.status() != WL_CONNECTED` in the main loop
2. **Portal auto-retry** — fires every 5 minutes while the captive portal is open (added v1.0.5)

---

## Layer 1: Runtime Reconnect (`main.cpp`)

Triggered on every loop iteration when WiFi drops.

- Reads saved SSID/pass from NVS (`Preferences`, namespace `"wifi"`)
- Attempts **3 × 10 s** blocking reconnects
- During each wait: `checkRelayTimers()` and `handleSerialCommands()` still run
- On success: logs IP, falls through to MQTT keepalive which reconnects MQTT
- On 3 failures: calls `startWiFiPortal()` — opens AP `sf500-{lastSix}`

**Timing constants (`config.h`):**
| Constant | Value | Purpose |
|---|---|---|
| `AUTO_CONNECT_TIMEOUT_MS` | 15 s | Setup-phase single attempt timeout |
| `CONNECT_TIMEOUT_MS` | 30 s | Portal user-initiated connect timeout |

---

## Layer 2: Portal Auto-Retry (`wifi_portal.cpp`) — added v1.0.5

**Problem it solves:** Before v1.0.5, if all 3 runtime reconnect attempts failed (e.g. router rebooting at midnight), the device opened the captive portal and stayed there indefinitely — requiring someone on-site to re-enter WiFi credentials. Unacceptable for unmanned overnight deployments.

**How it works:**

- When `startWiFiPortal()` is called, `portalStartedAt = millis()` is stamped
- `handlePortalLoop()` checks every iteration: if `wifiState == STATE_PORTAL` and `millis() - portalStartedAt >= PORTAL_SAVED_RETRY_INTERVAL_MS` (5 min)...
  - Reads saved SSID/pass from NVS
  - Switches to `WIFI_AP_STA` mode (AP stays up so a user can still reconfigure)
  - Calls `WiFi.begin(savedSSID, savedPass)`
  - Transitions to `STATE_CONNECTING` with `bgRetryActive = true`
- Background retry uses `AUTO_CONNECT_TIMEOUT_MS` (15 s) instead of 30 s
- **On success:** `STATE_ONLINE` → AP closes after 5 s → device resumes normally
- **On timeout:** logs `Background retry timed out`, resets `portalStartedAt = millis()`, tries again in another 5 minutes
- Repeats indefinitely until the network returns

**Timing constant (`config.h`):**
| Constant | Value | Purpose |
|---|---|---|
| `PORTAL_SAVED_RETRY_INTERVAL_MS` | 5 min | Interval between background retry attempts |

**Key variables:**
| Variable | Defined in | Purpose |
|---|---|---|
| `portalStartedAt` | `main.cpp` / `globals.h` | Timestamp when portal opened or last retry failed |
| `bgRetryActive` (static) | `wifi_portal.cpp` | Distinguishes background vs user-initiated connect attempt |

---

## WiFi State Machine

```
enum WiFiState { STATE_PORTAL, STATE_CONNECTING, STATE_ONLINE }
```

| Transition | Trigger |
|---|---|
| `→ STATE_PORTAL` | No saved creds at boot / 3 runtime reconnect failures / user MQTT cmd `{"cmd":"portal"}` / connect timeout |
| `→ STATE_CONNECTING` | User submits `/save` form OR background auto-retry fires |
| `→ STATE_ONLINE` | `WiFi.status() == WL_CONNECTED` while in `STATE_CONNECTING` |

---

## Known Limitations / Future Work

- **Blocking reconnect during runtime:** The 3×10 s reconnect loop in `main.cpp` still blocks `readSensors()`, `mqttClient.loop()`, and uploads for up to 30 s. Could be made non-blocking with a state machine if this becomes a problem.
- **No reconnect cooldown:** If WiFi flaps (connects and drops repeatedly), the runtime reconnect fires immediately on every drop. A `lastReconnectAttemptMs` guard would prevent tight cycles.
- **Setup vs runtime retry asymmetry:** Setup tries once for 15 s; runtime tries 3 × 10 s. Intentional (setup is slower path) but undocumented.

---

## Files Changed in v1.0.5

| File | Change |
|---|---|
| `src/config.h` | Added `PORTAL_SAVED_RETRY_INTERVAL_MS` |
| `src/globals.h` | Added `extern unsigned long portalStartedAt` |
| `src/main.cpp` | Defined `portalStartedAt = 0` |
| `src/wifi_portal.cpp` | `startWiFiPortal()` stamps `portalStartedAt`; `handlePortalLoop()` adds auto-retry block + `bgRetryActive` flag |
