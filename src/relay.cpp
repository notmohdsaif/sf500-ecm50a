// =====================================================
// RELAY.CPP
// Relay GPIO control, timed relay, schedule check, serial commands
// =====================================================

#include "relay.h"
#include "logger.h"
#include "mqtt_handler.h"   // publishRelayStatus()
#include "cloud.h"           // logDeviceActivity()
#include <HTTPClient.h>

// =====================================================
// RELAY WRITE — sets GPIO, logs to Supabase, publishes MQTT
// =====================================================

void writeRelay(uint8_t num, bool state)
{
  if (num < 1 || num > 2)
    return;

  uint8_t pin = (num == 1) ? RELAY1_PIN : RELAY2_PIN;
  digitalWrite(pin, state ? HIGH : LOW);
  relayStates[num - 1] = state;

  const char *label = (num == 1) ? " (Dosing)" : " (Mixing)";
  LOGF("R%d%s -> %s\n", num, label, state ? "ON" : "OFF");

  if (WiFi.status() == WL_CONNECTED)
  {
    // Log relay event to Supabase
    HTTPClient http;
    String url = String(SUPABASE_URL) + "/rest/v1/relay_metrics";

    if (http.begin(secureClient, url))
    {
      StaticJsonDocument<128> doc;
      char relayId[10];
      sprintf(relayId, "relay_%02d", num);
      doc["device"]   = deviceName;
      doc["relay_id"] = relayId;
      doc["status"]   = state ? 1 : 0;

      String payload;
      serializeJson(doc, payload);

      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", SUPABASE_KEY);
      http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
      http.setTimeout(4000);   // was 15s — relay metrics are audit-only, don't block the loop
      http.POST(payload);
      http.end();
    }

    publishRelayStatus();
  }
}

// =====================================================
// RELAY TIMERS — auto-off after duration expires
// =====================================================

void checkRelayTimers()
{
  unsigned long now = millis();

  for (int i = 0; i < 2; i++)
  {
    if (relayDurations[i] == 0 || relayTimers[i] == 0)
      continue;

    unsigned long elapsed = now - relayTimers[i];
    unsigned long total   = relayDurations[i] * 1000UL;

    // Debug every 10 seconds
    static unsigned long lastDebug[2] = {0, 0};
    if (now - lastDebug[i] >= 10000)
    {
      // Guard against underflow before printing remaining time
      unsigned long remaining = (elapsed < total) ? (total - elapsed) / 1000 : 0;
      LOGF("R%d timer: %lus remaining\n", i + 1, remaining);
      lastDebug[i] = now;
    }

    if (elapsed >= total)
    {
      relayDurations[i] = 0;
      relayTimers[i]    = 0;
      writeRelay(i + 1, false);
      LOGF("R%d timer expired\n", i + 1);
    }
  }

  if (r3Duration > 0 && r3Timer > 0)
  {
    unsigned long elapsed = now - r3Timer;
    unsigned long total   = r3Duration * 1000UL;

    static unsigned long lastDebugR3 = 0;
    if (now - lastDebugR3 >= 10000)
    {
      unsigned long remaining = (elapsed < total) ? (total - elapsed) / 1000 : 0;
      LOGF("R3 timer: %lus remaining\n", remaining);
      lastDebugR3 = now;
    }

    if (elapsed >= total)
    {
      r3Duration = 0;
      r3Timer    = 0;
      writePlugRelay(false);
      LOGLN("R3 timer expired");
    }
  }
}

// =====================================================
// SCHEDULE CHECK — triggers relay at configured time
// Bug fix: marker uses month+day+hour+min (no year) to avoid uint32 overflow
// =====================================================

void checkSchedules()
{
  if (scheduleCount == 0)
    return;

  time_t now = time(nullptr);
  if (now < 1000000000)
    return;

  struct tm ti;
  localtime_r(&now, &ti);

  // Unique per-minute key within a year: max = 12*1000000 + 31*10000 + 23*100 + 59 = 12,312,359
  uint32_t marker = (uint32_t)((ti.tm_mon + 1) * 1000000L
                               + ti.tm_mday   * 10000L
                               + ti.tm_hour   * 100L
                               + ti.tm_min);

  for (int i = 0; i < scheduleCount; i++)
  {
    Schedule &sch = schedules[i];

    if (!sch.enabled)                                   continue;
    if (!sch.days[ti.tm_wday])                          continue;
    if (sch.hour != (uint8_t)ti.tm_hour ||
        sch.minute != (uint8_t)ti.tm_min)               continue;
    if (lastTriggeredTime[i] == (unsigned long)marker)  continue;
    if (sch.relayNum < 1 || sch.relayNum > 3)           continue;

    if (sch.relayNum == 3)
    {
      if (!tasmotaPlugEnabled || r3Duration > 0) continue;
      LOGLNS("\n[SCHEDULE] " + sch.name);
      LOGF("  R3 (Plug) ON for %ds\n", sch.duration);
      lastTriggeredTime[i] = (unsigned long)marker;
      r3Duration = sch.duration;
      r3Timer    = millis();
      writePlugRelay(true);
      logDeviceActivity("schedule", (String("Schedule \"") + sch.name + "\": R3 ON for " + sch.duration + "s").c_str());
      continue;
    }

    int idx = sch.relayNum - 1;
    if (relayDurations[idx] > 0)
      continue;  // relay already running, skip

    LOGLNS("\n[SCHEDULE] " + sch.name);
    LOGF("  R%d ON for %ds\n", sch.relayNum, sch.duration);

    lastTriggeredTime[i]  = (unsigned long)marker;
    relayDurations[idx]   = sch.duration;
    relayTimers[idx]      = millis();
    writeRelay(sch.relayNum, true);
    logDeviceActivity("schedule", (String("Schedule \"") + sch.name + "\": R" + sch.relayNum + " ON for " + sch.duration + "s").c_str());
  }
}

// =====================================================
// SERIAL COMMANDS — for manual debug/testing
// =====================================================

void handleSerialCommands()
{
  if (!Serial.available())
    return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0)
    return;

  if (cmd == "R1ON")
    writeRelay(1, true);
  else if (cmd == "R1OFF")
    writeRelay(1, false);
  else if (cmd == "R2ON")
    writeRelay(2, true);
  else if (cmd == "R2OFF")
    writeRelay(2, false);
  else if (cmd == "ALLON")
  {
    writeRelay(1, true);
    delay(50);
    writeRelay(2, true);
  }
  else if (cmd == "ALLOFF")
  {
    writeRelay(1, false);
    delay(50);
    writeRelay(2, false);
  }
  else if (cmd == "PLUGON")
    writePlugRelay(true);
  else if (cmd == "PLUGOFF")
    writePlugRelay(false);
  else if (cmd == "WIFIINFO")
  {
    LOGLN("\n--- WiFi Info ---");
    if (WiFi.status() == WL_CONNECTED)
    {
      LOGLN("Status: Connected");
      LOGLNS("SSID:   " + WiFi.SSID());
      LOGLNS("IP:     " + WiFi.localIP().toString());
      LOGF("RSSI:   %d dBm\n", WiFi.RSSI());
    }
    else
    {
      LOGLN("Status: Disconnected");
    }
    wifiPrefs.begin("wifi", true);
    LOGLNS("Saved SSID: " + wifiPrefs.getString("ssid", "(none)"));
    wifiPrefs.end();
    LOGLN("-----------------\n");
  }
  else if (cmd == "RAINRESET")
  {
    if (!rainSensorFound)
    {
      LOGLN("[RAINRESET] Rain sensor not found — skipping");
    }
    else
    {
      LOGF("[RAINRESET] Writing 0x5A to register 0 on ID %d…\n", rainSensorId);
      modbus.begin(rainSensorId, Serial1);
      delay(10);
      uint8_t result = modbus.writeSingleRegister(RAIN_REG_TIPS, 0x5A);
      if (result == modbus.ku8MBSuccess)
        LOGLN("[RAINRESET] Write OK — check next [RAIN] line, should drop to 0");
      else
        LOGF("[RAINRESET] Write failed (Modbus code 0x%02X)\n", result);
    }
  }
  else if (cmd == "HELP")
  {
    LOGLN("\n--- Commands ---");
    LOGLN("R1ON/R1OFF   - Relay 1 (Dosing)");
    LOGLN("R2ON/R2OFF   - Relay 2 (Mixing)");
    LOGLN("ALLON/ALLOFF - All relays");
    LOGLN("PLUGON/PLUGOFF - Relay 3 (Tasmota Plug)");
    LOGLN("WIFIINFO     - WiFi status");
    LOGLN("RAINRESET    - Try resetting rain counter (test)");
    LOGLN("HELP         - This list");
    LOGLN("----------------\n");
  }
  else
  {
    LOGLNS("Unknown: " + cmd + " (type HELP)");
  }
}
