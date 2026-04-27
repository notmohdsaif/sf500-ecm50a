// =====================================================
// MQTT_HANDLER.CPP
// MQTT connection, command callback, relay status publish
// =====================================================

#include "mqtt_handler.h"
#include "relay.h"    // writeRelay()

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

      Serial.println("MQTT connected");
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
        Serial.println("[WiFi] Forget command received");
        pendingWifiForget = true;
      }
      else if (cmd == "portal")
      {
        Serial.println("[WiFi] Portal command received");
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
    Serial.println("Ignoring MQTT during startup");
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
      Serial.println("R" + String(i + 1) + " timed for " + String(duration) + "s");
    }
    else
    {
      relayDurations[i] = 0;
      relayTimers[i]    = 0;
      writeRelay(i + 1, val == 1);
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

  StaticJsonDocument<128> doc;
  doc["r1"] = relayStates[0] ? 1 : 0;
  doc["r2"] = relayStates[1] ? 1 : 0;

  // Include end-time for any active timed relay
  unsigned long now = millis();
  unsigned long maxRemaining = 0;

  for (int i = 0; i < 2; i++)
  {
    if (relayDurations[i] > 0 && relayTimers[i] > 0)
    {
      unsigned long elapsed = now - relayTimers[i];
      unsigned long total   = relayDurations[i] * 1000UL;
      if (elapsed < total)
      {
        unsigned long remaining = total - elapsed;
        if (remaining > maxRemaining)
          maxRemaining = remaining;
      }
    }
  }

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

  char buf[128];
  serializeJson(doc, buf);
  mqttClient.publish(topicRelayStatus.c_str(), buf);
}
