# Cellular (4G) Fallback Networking — Design Spec

Status: draft, pending review
Target device: `sf500_107888` initially (ECM50-A09 board revision with onboard Quectel EC801E-CN 4G module); design should generalize to any future unit built on the same board revision.

## 1. Problem

Some ECM50-A09 controller units (this specific board revision) have an onboard 4G/cellular modem that has never been used — the firmware only ever supported WiFi. Sites with unreliable WiFi (weak signal, the site's own AP going down) lose full connectivity even though a cellular path might be available. This spec adds cellular as an automatic fallback network transport.

## 2. Goal / Scope

- On boot and on reconnect, the controller tries WiFi first, exactly as it does today (no behavior change while WiFi is healthy).
- If WiFi is unavailable after the existing retry/portal logic, and the board has a working cellular modem, the controller automatically switches to cellular for all networking (MQTT + Supabase HTTPS) instead of sitting offline.
- The controller keeps periodically retrying WiFi in the background while on cellular (mirrors the existing 5-minute portal-retry pattern) and switches back to WiFi automatically once it's healthy again.
- Boards without the 4G module (the vast majority of the fleet) must be completely unaffected — this is additive, not a replacement of the WiFi path.
- Out of scope for this spec: a dashboard UI for cellular status/config beyond what's needed for basic visibility (see §7); data-usage/cost management; any change to the MQTT broker or Supabase project themselves.

## 3. Hardware facts (confirmed this session, on real hardware)

Source: manufacturer pin list (`ECM50-A_Pin_List_V1.0.xlsx`) + live testing on `sf500_107888`.

| Signal | Pin | Notes |
|---|---|---|
| Modem power enable | GPIO38 | Pull HIGH to power on. **Only populated on the 4G board variant** — this is the basis for hardware auto-detection (§4.1). |
| Modem UART2 TX (ESP32→modem) | GPIO39 | |
| Modem UART2 RX (modem→ESP32) | GPIO40 | 115200 baud, 8N1 |

Modem: Quectel EC801E-CN, firmware `EC801ECNCGR03A04M02_GENERAL`. Confirmed via live AT-command testing:
- Powers on, detects SIM, registers on network, obtains a real IP via `AT+CGDCONT`/`AT+CGACT` (APN `ansar` for the currently-inserted Ansar Mobile SIM — **the APN must be a configurable field, not hardcoded**, since it will differ per SIM/carrier across units).
- Plain TCP (`AT+QIOPEN`) and plain HTTP work.
- **The modem's own SSL/TLS AT commands (`AT+QSSLxxx`) are not supported on this firmware build** — confirmed both directly (`AT+QSSLCFG=?` errors) and via `AT+QHTTPGET` against an `https://` URL (fails, error 715) vs. an `http://` URL (succeeds cleanly).
- **Workaround confirmed working**: doing TLS in the ESP32's own software (mbedTLS) on top of the modem's raw TCP socket. Verified live against the real Supabase project — full handshake in ~1.8s, real HTTP response received (proper 401 JSON error for a request missing an API key, meaning the round trip and cert chain both worked).

## 4. Architecture

### 4.1 Hardware auto-detection (no new DB field needed for capability)

On boot, after the existing sensor-scan step, attempt to power on and handshake with the modem (`AT`, short timeout, 2-3 retries — mirrors how `initSensors()` already scans for optional hardware). If it responds, `cellularCapable = true` for this boot; if not, the modem code path is never touched again this session. This matches the existing pattern for optional hardware (sensors are auto-scanned, not admin-flagged) and means the exact same firmware image is safe to run on both board variants — no separate firmware build, no risk to the ~fleet of non-4G units.

### 4.2 Client abstraction

`cloud.cpp`, `mqtt_handler.cpp`, and `ota.cpp` currently hold direct `WiFiClient espClient` / `WiFiClientSecure secureClient` globals. These become:
- A `Client*` (or reference) that HTTPS callers use, pointing at either the existing `WiFiClientSecure` or a new `SSLClient` wrapping a `TinyGsmClient`, depending on active transport.
- Likewise for MQTT's plain `Client` (`WiFiClient` vs `TinyGsmClient` directly, no TLS needed either way since the broker connection itself is unencrypted).

Library additions: `TinyGSM` (`TINY_GSM_MODEM_BG96` profile — closest match to this Quectel module's shared AT command set; confirmed working live) and `SSLClientMbedTLS` (wraps any `Client` with ESP32's own mbedTLS, used to work around the modem's missing native TLS).

### 4.3 Transport state machine

Extends the existing `WiFiState` (`STATE_PORTAL` / `STATE_CONNECTING` / `STATE_ONLINE`) logic rather than replacing it:

1. Boot: try WiFi (existing logic, unchanged).
2. If WiFi's existing retry budget is exhausted (currently: 3×10s attempts, then portal) **and** `cellularCapable` **and** `cellular_apn` is configured (non-empty, per §5 — a unit with no confirmed APN never attempts cellular, same safety default as the hardware check): instead of only opening the captive portal, also bring up the cellular data connection (power on modem, `AT+CGDCONT`/`gprsConnect` with the configured APN) and switch the active `Client*` to the cellular-backed clients. The WiFi portal still opens in parallel (unchanged) so on-site WiFi reconfiguration keeps working.
3. While on cellular, keep the existing WiFi auto-retry timer running in the background (same `PORTAL_SAVED_RETRY_INTERVAL_MS` cadence). On a successful WiFi reconnect, tear down the cellular connection and switch the active `Client*` back to WiFi.
4. If cellular itself fails (no SIM, no signal, data connect fails), fall back to today's behavior exactly (sit in portal, keep retrying WiFi) — cellular is additive, never a hard dependency.

### 4.4 What runs over which transport

| Traffic | Over WiFi | Over cellular |
|---|---|---|
| MQTT (`broker.emqx.io:1883`, plain TCP) | unchanged | `TinyGsmClient` directly — no TLS needed either way |
| Supabase REST (registration, config, sensor/activity upload, OTA check) | `WiFiClientSecure` (unchanged) | `SSLClient` wrapping `TinyGsmClient` (software TLS) |

## 5. Config additions (device_management)

New nullable column, mirroring the existing `tasmota_plug_host` pattern (implicit-by-presence, no separate enable flag):
- `cellular_apn text` — the APN string for the inserted SIM. Null/empty means cellular fallback is inert even if hardware is detected (safety default — don't attempt cellular on a unit where nobody has confirmed what SIM/APN is present). Admin sets this once per unit at deployment.

No dashboard UI beyond exposing this one field (matches the "Plug IP" precedent — a single admin-panel input, nothing more, unless you want more).

## 6. Certificate verification (real implementation, not the spike's shortcut)

The spike used `setInsecure()` to isolate the handshake feasibility question — that skips server certificate verification and must not ship. The real implementation needs `setCACert()` with the correct root CA for Supabase's cert chain (Cloudflare-fronted; almost certainly a standard public CA already bundled in common root stores — needs pinning to a specific PEM the same way any embedded TLS client does). This is a well-understood, solvable detail, not an open risk — just needs doing carefully during implementation.

## 7. Visibility / observability

Extend the existing `wifi:{...}` block in the `sf500/{lastSix}/data` MQTT payload with a `cellular:{active, signal, apn}` object when cellular is the active transport (mirrors how the plug-HTTP work added a `plug:{...}` block) — lets the dashboard show which transport is live without a bigger UI change. Full dashboard treatment (e.g. a distinct icon) is a follow-up, not blocking this spec.

## 8. Risks / open items for implementation phase

- CA cert pinning for Supabase (see §6) — needs the actual cert chain fetched and embedded.
- Battery/current draw of the modem under cellular load hasn't been measured — worth a bench check before field deployment, not blocking for this unit specifically (mains-powered).
- `TINY_GSM_MODEM_BG96` is the closest available TinyGSM profile, not an exact match for the EC801E-CN — the AT command surface used (QIOPEN-family) tested clean, but any other BG96-profile behavior TinyGSM relies on hasn't been exhaustively exercised. Treat as validated for what this spec actually uses, not a blanket guarantee.
- Behavior when the modem is present but has no SIM inserted, or has a locked/PIN-protected SIM, hasn't been tested (this session's unit had a ready, unlocked SIM). Should degrade to "cellular unavailable, WiFi-only" per §4.4's fallback rule — needs verifying, not assuming.

## 9. Testing plan (for the implementation phase)

Mirrors the bench-test discipline already established for the plug-HTTP feature — must not repeat the "waived, shipped unverified" mistake from that feature's original rollout:
- Bench test on `sf500_107888` (tethered, serial monitor) covering: cold boot with WiFi available (cellular never engages), WiFi failure → cellular engages → real MQTT + Supabase traffic flows, WiFi recovery → falls back cleanly, cellular unavailable (no SIM/no signal) → degrades to today's WiFi-only behavior with no crash or hang.
- Explicit stack-margin check given the recent v1.2.5 stack-overflow incident (see project memory `project_stack_canary_crash_v125`) — this feature adds more code paths in the same `loopTask`; soak-test through repeated transport switches, not just a single one.
- Only after a clean bench soak does this go anywhere near a second device.
