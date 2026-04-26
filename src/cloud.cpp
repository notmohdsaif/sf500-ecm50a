// =====================================================
// CLOUD.CPP
// Supabase REST API + NTP time sync
// =====================================================

#include "cloud.h"
#include <HTTPClient.h>

// =====================================================
// NTP TIME SYNC
// =====================================================

void syncTimeWithNTP()
{
  const char *servers[] = {
    "pool.ntp.org", "time.google.com",
    "time.cloudflare.com", "time.windows.com"
  };

  Serial.print("Syncing NTP");

  for (int s = 0; s < 4; s++)
  {
    configTime(8 * 3600, 0, servers[s]);

    for (int i = 0; i < 20; i++)
    {
      time_t now = time(nullptr);
      if (now > 1000000000)
      {
        struct tm ti;
        localtime_r(&now, &ti);
        Serial.println(" OK");
        Serial.printf("Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
        return;
      }
      delay(500);
      Serial.print(".");
    }
  }

  Serial.println(" FAILED");
}

// =====================================================
// DEVICE REGISTRATION
// =====================================================

void registerDevice()
{
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_management?device=eq." + deviceName;

  if (!http.begin(secureClient, url))
    return;

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(15000);

  int code = http.GET();
  Serial.println("[REG] HTTP code: " + String(code));

  if (code == 200)
  {
    String response = http.getString();
    Serial.println("[REG] Response: " + response.substring(0, 80));

    if (response == "[]" || response.length() < 5)
    {
      // New device — register it
      http.end();

      String regUrl = String(SUPABASE_URL) + "/rest/v1/device_management";
      if (!http.begin(secureClient, regUrl))
        return;

      StaticJsonDocument<256> doc;
      doc["device"]   = deviceName;
      doc["mac"]      = deviceMAC;
      doc["status"]   = "online";
      doc["location"] = "Auto-registered";
      doc["sensor"]   = serialized("{}");

      String payload;
      serializeJson(doc, payload);

      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", SUPABASE_KEY);
      http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
      http.addHeader("Prefer", "return=representation,resolution=merge-duplicates");

      code = http.POST(payload);
      isRegistered = (code == 200 || code == 201 || code == 409);
    }
    else
    {
      isRegistered = true;
    }
  }

  http.end();
  if (isRegistered)
    Serial.println("Device registered");
}

// =====================================================
// SENSOR CONFIG UPLOAD
// Bug fix: use createNestedObject for ArduinoJson v6
// =====================================================

void uploadSensorConfig()
{
  if (!ecSensorFound && !wlSensorFound)
    return;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_management?device=eq." + deviceName;

  if (!http.begin(secureClient, url))
    return;

  StaticJsonDocument<512> doc;
  JsonObject sensorObj = doc.createNestedObject("sensor");

  int idx = 1;
  if (ecSensorFound)
  {
    String key = "sensor" + String(idx++);
    JsonObject s = sensorObj.createNestedObject(key);
    s["ID"]     = String(ecSensorId);
    s["type"]   = "EC";
    s["status"] = "online";
  }
  if (wlSensorFound)
  {
    String key = "sensor" + String(idx++);
    JsonObject s = sensorObj.createNestedObject(key);
    s["ID"]     = String(wlSensorId);
    s["type"]   = "WL";
    s["status"] = "online";
  }

  String payload;
  serializeJson(doc, payload);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=representation");
  http.setTimeout(15000);

  int code = http.PATCH(payload);
  http.end();

  Serial.println(code == 200 || code == 204 ? "Sensor config uploaded" : "Config upload failed");
}

// =====================================================
// SENSOR READINGS UPLOAD
// =====================================================

void uploadSensorReadings()
{
  if (!sensors.hasData)
    return;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/sensor_metrics";

  DynamicJsonDocument doc(512);
  JsonArray arr = doc.to<JsonArray>();

  if (ecSensorFound)
  {
    char ecId[8], wtId[8];
    sprintf(ecId, "ec_%02d", ecSensorId);
    sprintf(wtId, "wt_%02d", ecSensorId);

    JsonObject ec = arr.createNestedObject();
    ec["device"]    = deviceName;
    ec["sensor_id"] = ecId;
    ec["value"]     = serialized(String(sensors.ec, 3));

    JsonObject wt = arr.createNestedObject();
    wt["device"]    = deviceName;
    wt["sensor_id"] = wtId;
    wt["value"]     = serialized(String(sensors.temp, 1));
  }

  if (wlSensorFound)
  {
    char wlId[8];
    sprintf(wlId, "wl_%02d", wlSensorId);

    JsonObject wl = arr.createNestedObject();
    wl["device"]    = deviceName;
    wl["sensor_id"] = wlId;
    wl["value"]     = sensors.wl;
  }

  String payload;
  serializeJson(arr, payload);

  if (http.begin(secureClient, url))
  {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    http.setTimeout(15000);

    int code = http.POST(payload);
    if (code == 200 || code == 201)
      Serial.println("Uploaded " + String(arr.size()) + " readings");
    http.end();
  }
}

// =====================================================
// DEVICE STATUS UPDATE
// =====================================================

void updateDeviceStatus(const char *status)
{
  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_management?device=eq." + deviceName;

  if (!http.begin(secureClient, url))
    return;

  StaticJsonDocument<64> doc;
  doc["status"] = status;

  String payload;
  serializeJson(doc, payload);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(15000);
  http.PATCH(payload);
  http.end();
}

// =====================================================
// DEVICE CONFIG FETCH
// =====================================================

void fetchDeviceConfig()
{
  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/device_management?device=eq." + deviceName +
               "&select=auto_dosing,ec_target,mixing_pump,dosing_time";

  if (!http.begin(secureClient, url))
    return;

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) { http.end(); return; }

  String response = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) return;
  if (doc.size() == 0) return;

  JsonObject dev = doc[0];
  bool changed = false;

  if (!dev["auto_dosing"].isNull())
  {
    bool newVal = dev["auto_dosing"];
    if (newVal != autoDosing)
    {
      autoDosing = newVal;
      autoDosingStartTime = autoDosing ? millis() : 0;
      autoState = AUTO_IDLE; // state machine resets on any toggle
      Serial.println(autoDosing ? "\n[CONFIG] Auto-Dosing ON" : "\n[CONFIG] Auto-Dosing OFF");
      changed = true;
    }
  }

  if (!dev["mixing_pump"].isNull())
  {
    bool newVal = dev["mixing_pump"];
    if (newVal != autoMixing)
    {
      autoMixing = newVal;
      Serial.println(autoMixing ? "[CONFIG] Mixing Pump: ENABLED" : "[CONFIG] Mixing Pump: DISABLED");
      changed = true;
    }
  }

  if (!dev["ec_target"].isNull())
  {
    float newVal = dev["ec_target"];
    if (abs(newVal - ecTarget) > 0.01f)
    {
      ecTarget   = newVal;
      ecMinusHys = ecTarget - EC_HYSTERESIS;

      ecReadingCount = 0;
      ecReadingIndex = 0;
      autoState = AUTO_IDLE; // reset state machine when target changes

      Serial.println("\n[CONFIG] EC Target: " + String(ecTarget, 2) +
                     " (dose below " + String(ecMinusHys, 2) + ")");
      changed = true;
    }
  }

  if (!dev["dosing_time"].isNull())
  {
    unsigned int newVal = dev["dosing_time"].as<unsigned int>();
    if (newVal > 0 && newVal != dosingTime)
    {
      dosingTime = newVal;
      Serial.println("[CONFIG] Dosing Time: " + String(dosingTime) + "s");
      changed = true;
    }
  }

  if (changed && autoDosing)
    Serial.println("[INFO] Dose when EC < " + String(ecMinusHys, 2) + "\n");
}

// =====================================================
// SCHEDULES FETCH
// =====================================================

void fetchSchedules()
{
  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/relay_schedule?device=eq." + deviceName +
               "&status=eq.true&select=*";

  if (!http.begin(secureClient, url))
    return;

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) { http.end(); return; }

  String response = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response) != DeserializationError::Ok)
    return;

  scheduleCount = 0;

  for (JsonObject s : doc.as<JsonArray>())
  {
    if (scheduleCount >= MAX_SCHEDULES)
      break;

    Schedule &sch   = schedules[scheduleCount];
    sch.id          = s["id"];
    sch.name        = s["schedule_name"].as<String>();
    sch.hour        = s["hour"];
    sch.minute      = s["minute"];
    sch.duration    = s["duration"];
    sch.enabled     = s["status"];

    // Parse relay_id: "relay01" -> 1
    String rid  = s["relay_id"].as<String>();
    sch.relayNum = (uint8_t)rid.substring(5).toInt();

    for (int i = 0; i < 7; i++)
      sch.days[i] = false;

    const char *dayNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (JsonVariant d : s["day"].as<JsonArray>())
    {
      String ds = d.as<String>();
      for (int i = 0; i < 7; i++)
      {
        if (ds == dayNames[i]) sch.days[i] = true;
      }
    }

    scheduleCount++;
  }

  Serial.println("\n[SCHEDULE] " + String(scheduleCount) + " active");
  for (int i = 0; i < scheduleCount; i++)
  {
    Serial.printf("  [%lu] %s: R%d @ %02d:%02d for %ds\n",
                  (unsigned long)schedules[i].id,
                  schedules[i].name.c_str(),
                  schedules[i].relayNum,
                  schedules[i].hour,
                  schedules[i].minute,
                  schedules[i].duration);
  }
}

// =====================================================
// DEVICE ACTIVITY LOGGING
// Fire-and-forget POST to activity_log table.
// =====================================================

void logDeviceActivity(const char *category, const char *action)
{
  if (!isRegistered || deviceName.isEmpty()) return;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/activity_log";
  if (!http.begin(secureClient, url)) return;

  StaticJsonDocument<256> doc;
  doc["device"]   = deviceName;
  doc["category"] = category;
  doc["action"]   = action;
  doc["source"]   = "device";

  String payload;
  serializeJson(doc, payload);

  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer",        "return=minimal");
  http.setTimeout(10000);
  http.POST(payload);
  http.end();
}
