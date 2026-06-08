// =====================================================
// MQTT_HANDLER.CPP
// MQTT connection, command callback, relay status publish
// =====================================================

#include "mqtt_handler.h"
#include "logger.h"
#include "relay.h"    // writeRelay()
#include <HTTPClient.h>

// =====================================================
// RECONNECT
// =====================================================

void reconnectMQTT()
{
  for (int i = 0; i < 5 && !mqttClient.connected(); i++)
  {
    String clientId = "SF500_" + lastSix;
    if (mqttClient.connect(clientId.c_str()))
    {
      mqttClient.subscribe(topicRelayUpdate.c_str());
      mqttClient.subscribe(topicWifiCmd.c_str());
      if (tasmotaPlugEnabled && tasmotaPlugTopic.length() > 0)
      {
        mqttClient.subscribe(("stat/" + tasmotaPlugTopic + "/POWER").c_str());
        // Query current plug state so r3State reflects reality after reconnect
        mqttClient.publish(("cmnd/" + tasmotaPlugTopic + "/Power").c_str(), "");
      }
      publishRelayStatus();

      // Publish wifi info immediately so dashboard updates without waiting for sensor cycle
      if (WiFi.status() == WL_CONNECTED)
      {
        StaticJsonDocument<128> doc;
        JsonObject wifiObj = doc.createNestedObject("wifi");
        wifiObj["ssid"] = WiFi.SSID();
        wifiObj["rssi"] = WiFi.RSSI();
        wifiObj["ip"]   = WiFi.localIP().toString();
        char buf[128];
        serializeJson(doc, buf);
        mqttClient.publish(mqttTopicData.c_str(), buf);
      }

      LOGLN("MQTT connected");
      return;
    }
    delay(2000);
  }
}

// =====================================================
// CALLBACK — handles relay update messages
// =====================================================

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  String topicStr = String(topic);

  // --- Tasmota plug status update ---
  if (tasmotaPlugEnabled && tasmotaPlugTopic.length() > 0 &&
      topicStr == "stat/" + tasmotaPlugTopic + "/POWER")
  {
    String msg;
    for (unsigned int i = 0; i < length; i++)
      msg += (char)payload[i];
    msg.toUpperCase();
    r3State = (msg == "ON" || msg == "1");
    publishRelayStatus();
    return;
  }

  // --- WiFi command handler ---
  if (topicStr == topicWifiCmd)
  {
    String msg;
    for (unsigned int i = 0; i < length; i++)
      msg += (char)payload[i];

    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, msg) == DeserializationError::Ok)
    {
      String cmd = doc["cmd"].as<String>();
      if (cmd == "forget")
      {
        LOGLN("[WiFi] Forget command received");
        pendingWifiForget = true;
      }
      else if (cmd == "portal")
      {
        LOGLN("[WiFi] Portal command received");
        pendingWifiPortal = true;
      }
    }
    return;
  }

  if (topicStr != topicRelayUpdate)
    return;

  // Startup protection: ignore commands for first 5 seconds
  if (!startupComplete && (millis() - startupTime < 5000))
  {
    LOGLN("Ignoring MQTT during startup");
    return;
  }
  startupComplete = true;

  String msg;
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, msg) != DeserializationError::Ok)
    return;

  unsigned int duration = doc.containsKey("t") ? doc["t"].as<unsigned int>() : 0;

  const char *keys[] = {"r1", "r2"};
  for (int i = 0; i < 2; i++)
  {
    if (!doc.containsKey(keys[i]))
      continue;

    int val = doc[keys[i]];
    if (val != 0 && val != 1)
      continue;

    if (val == 1 && duration > 0)
    {
      relayDurations[i] = duration;
      relayTimers[i]    = millis();
      writeRelay(i + 1, true);
      LOGF("R%d timed for %ds\n", i + 1, duration);
    }
    else
    {
      relayDurations[i] = 0;
      relayTimers[i]    = 0;
      writeRelay(i + 1, val == 1);
    }
  }

  if (doc.containsKey("r3") && tasmotaPlugEnabled && tasmotaPlugTopic.length() > 0)
  {
    int val = doc["r3"];
    if (val == 0 || val == 1)
    {
      if (val == 1 && duration > 0)
      {
        r3Duration = duration;
        r3Timer    = millis();
        writePlugRelay(true);
        LOGF("R3 (Plug) timed for %ds\n", duration);
      }
      else
      {
        r3Duration = 0;
        r3Timer    = 0;
        writePlugRelay(val == 1);
      }
    }
  }
}

// =====================================================
// PUBLISH RELAY STATUS
// =====================================================

void publishRelayStatus()
{
  if (!mqttClient.connected())
    return;

  StaticJsonDocument<384> doc;
  doc["r1"] = relayStates[0] ? 1 : 0;
  doc["r2"] = relayStates[1] ? 1 : 0;
  doc["r3"] = r3State ? 1 : 0;

  unsigned long now = millis();
  unsigned long maxRemaining = 0;
  const char* etKeys[] = { "et1", "et2" };

  for (int i = 0; i < 2; i++)
  {
    if (relayDurations[i] > 0 && relayTimers[i] > 0)
    {
      unsigned long elapsed   = now - relayTimers[i];
      unsigned long total     = relayDurations[i] * 1000UL;
      if (elapsed < total)
      {
        unsigned long remaining = total - elapsed;
        if (remaining > maxRemaining)
          maxRemaining = remaining;

        time_t endTime = time(nullptr) + (time_t)(remaining / 1000);
        struct tm ti;
        localtime_r(&endTime, &ti);
        char ts[30];
        sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
        doc[etKeys[i]] = ts;
      }
    }
  }

  if (r3Duration > 0 && r3Timer > 0)
  {
    unsigned long elapsed = now - r3Timer;
    unsigned long total   = r3Duration * 1000UL;
    if (elapsed < total)
    {
      unsigned long remaining = total - elapsed;
      if (remaining > maxRemaining)
        maxRemaining = remaining;

      time_t endTime = time(nullptr) + (time_t)(remaining / 1000);
      struct tm ti;
      localtime_r(&endTime, &ti);
      char ts[30];
      sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
              ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
              ti.tm_hour, ti.tm_min, ti.tm_sec);
      doc["et3"] = ts;
    }
  }

  // Keep legacy "et" (max remaining) for backward compat
  if (maxRemaining > 0)
  {
    time_t endTime = time(nullptr) + (time_t)(maxRemaining / 1000);
    struct tm ti;
    localtime_r(&endTime, &ti);
    char ts[30];
    sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
            ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
            ti.tm_hour, ti.tm_min, ti.tm_sec);
    doc["et"] = ts;
  }

  char buf[384];
  serializeJson(doc, buf);
  mqttClient.publish(topicRelayStatus.c_str(), buf);
}

// =====================================================
// WRITE PLUG RELAY — publish MQTT command to Tasmota plug
// =====================================================

void writePlugRelay(bool state)
{
  if (!tasmotaPlugEnabled || tasmotaPlugTopic.length() == 0)
    return;

  String backlogTopic = "cmnd/" + tasmotaPlugTopic + "/Backlog";

  if (state && r3Duration > 0)
  {
    // Embed PulseTime so the plug auto-shuts off even if the ESP32 reboots mid-session.
    // PulseTime formula: 1-111 = value × 0.1s; 112-64900 = (value-100)s.
    uint32_t dur  = (r3Duration > 64800) ? 64800 : (uint32_t)r3Duration;
    uint32_t pval = (dur < 12) ? dur * 10 : dur + 100;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "PulseTime1 %lu; Power1 1", (unsigned long)pval);
    mqttClient.publish(backlogTopic.c_str(), cmd);
    LOGF("R3 (Plug) -> ON (PulseTime=%lus via Backlog)\n", (unsigned long)dur);
  }
  else if (state)
  {
    // ON without a duration — plain command, no PulseTime
    mqttClient.publish(("cmnd/" + tasmotaPlugTopic + "/Power").c_str(), "1");
    LOGLN("R3 (Plug) -> ON");
  }
  else
  {
    // OFF — clear PulseTime first so it doesn't fire after a future reboot
    mqttClient.publish(backlogTopic.c_str(), "PulseTime1 0; Power1 0");
    LOGLN("R3 (Plug) -> OFF (PulseTime cleared)");
  }

  r3State = state;

  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    String url = String(SUPABASE_URL) + "/rest/v1/relay_metrics";
    if (http.begin(secureClient, url))
    {
      StaticJsonDocument<128> logDoc;
      logDoc["device"]   = deviceName;
      logDoc["relay_id"] = "relay_03";
      logDoc["status"]   = state ? 1 : 0;
      String payload;
      serializeJson(logDoc, payload);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", SUPABASE_KEY);
      http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
      http.setTimeout(4000);   // was 15s — relay metrics are audit-only, don't block the loop
      http.POST(payload);
      http.end();
    }
  }

  publishRelayStatus();
}
