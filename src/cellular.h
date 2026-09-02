#pragma once
#include <Arduino.h>
#ifndef TINY_GSM_MODEM_BG96
#define TINY_GSM_MODEM_BG96
#endif
#include <TinyGsmClient.h>

// UART2 to the onboard Quectel EC801E-CN modem. Nothing else on this board
// uses UART2 (RS485/Modbus is on Serial1), so this is exclusively the modem's.
extern HardwareSerial modemSerial;

// TinyGSM driver + a plain (non-TLS) TCP client over the modem. cellularClient
// is what MQTT rides on when cellular is the active transport (the broker link
// is unencrypted either way).
extern TinyGsm       modem;
extern TinyGsmClient cellularClient;

// Powers the modem (MODEM_PWR_PIN HIGH), opens UART2, and probes with AT.
// Safe to call on boards without the modem populated — the probes just time
// out and it returns false (after ~11s). Detection is lazy: this is only
// called the first time WiFi fails, never during a normal boot, so non-4G
// units never incur the cost.
bool detectCellularModem();

// Brings up the cellular data session (network registration + GPRS/PDP attach)
// on the given APN. Call only after detectCellularModem() has returned true.
bool connectCellularData(const char *apn);

// Sets the ESP32 system clock from the modem's network time (NITZ/CTZU, with
// an AT+QNTP fallback). Used instead of syncTimeWithNTP() when the active
// transport is cellular — SNTP/configTime() can't route through TinyGSM.
// Returns true if the clock was set to a plausible value.
bool syncTimeFromModem();

// One Supabase REST call over cellular: software TLS (ESP32 mbedTLS) on top of
// the modem's raw TCP, driven by ArduinoHttpClient (ESP32's HTTPClient can't
// take a non-WiFiClient). Adds the apikey + Bearer auth headers itself.
//  - method: "GET" | "POST" | "PATCH"
//  - url: full https://<supabase-host>/... URL
//  - reqBody: JSON body for POST/PATCH, "" for GET
//  - contentType: e.g. "application/json", or nullptr for GET
//  - prefer: value for the Prefer header, or nullptr
//  - outBody: receives the response body
// Returns the HTTP status code, or a negative ArduinoHttpClient error code.
int cellularSupabaseRequest(const char *method, const String &url,
                            const String &reqBody, const char *contentType,
                            const char *prefer, String &outBody);
