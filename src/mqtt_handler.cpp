// =====================================================
// MQTT_HANDLER.CPP
// MQTT connection, command callback, relay status publish
// =====================================================

#include "mqtt_handler.h"
#include "logger.h"
#include "relay.h"    // writeRelay()
#include "cloud.h"    // logDeviceActivity()
#include <HTTPClient.h>

// =====================================================
// RECONNECT
// =====================================================

void reconnectMQTT()
{
  for (int i = 0; i < 5 && !mqttClient.connected(); i++)
  {
    String clientId = "SF500_" + lastSix;
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS))
    {
      mqttClient.subscribe(topicRelayUpdate.c_str());
      mqttClient.subscribe(topicWifiCmd.c_str());
      mqttClient.subscribe(topicDeviceCmd.c_str());
      if (!plugUseHttp && tasmotaPlugEnabled && tasmotaPlugTopic.length() > 0)
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
    // Keep the relay auto-off timer running even while the broker is slow to
    // accept us — otherwise a timed relay can overrun its duration during a
    // reconnect storm.
    for (int w = 0; w < 10; w++)
    {
      checkRelayTimers();
      delay(200);
    }
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
    bool newR3State = (msg == "ON" || msg == "1");

    // Log to relay_metrics only on an actual transition — this status topic also fires
    // on every reconnect poll (see reconnectMQTT), which would otherwise duplicate-log
    // the same unchanged state on every reconnect. writePlugRelay() only logs R3 changes
    // it commands itself; this is the only place an externally-driven change (Tasmota's
    // own rule/timer, or someone toggling the plug outside this dashboard) gets captured.
    if (newR3State != r3State)
      logR3Transition(newR3State);

    r3State = newR3State;
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

  // --- Device command handler (sensor rescan, auto-dosing alarm reset) ---
  if (topicStr == topicDeviceCmd)
  {
    String msg;
    for (unsigned int i = 0; i < length; i++)
      msg += (char)payload[i];

    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, msg) == DeserializationError::Ok)
    {
      String cmd = doc["cmd"].as<String>();
      if (cmd == "rescan")
      {
        LOGLN("[Rescan] Command received");
        pendingRescan = true;
      }
      else if (cmd == "reset_auto_dosing")
      {
        LOGLN("[Auto] Reset command received");
        pendingAutoDosingReset = true;
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
      if (val == 1 && plugMode == "refill" && wlSensorFound && refillCutoffMm > 0.0f && sensors.wl >= refillCutoffMm)
      {
        logDeviceActivity("plug", "Refill blocked: water level already at 95%");
      }
      else if (val == 1 && plugMode == "fertigate" && ecSensorFound && fabs(sensors.ec - ecTarget) > FERTIGATE_EC_TOLERANCE)
      {
        logDeviceActivity("plug", "Fertigate blocked: EC out of range");
      }
      else if (val == 1 && duration > 0)
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

void publishRelayStatus(const char* r3Reason)
{
  if (!mqttClient.connected())
    return;

  StaticJsonDocument<384> doc;
  doc["r1"] = relayStates[0] ? 1 : 0;
  doc["r2"] = relayStates[1] ? 1 : 0;
  doc["r3"] = r3State ? 1 : 0;
  if (r3Reason != nullptr)
    doc["r3_reason"] = r3Reason;

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
// TASMOTA PLUG — LOCAL HTTP TRANSPORT (docs/plug-http-control.md)
// Used when plugHttpHost is set: the controller reaches the plug over its LAN
// /cm API instead of MQTT. All of this is inert unless plugUseHttp is true.
// =====================================================

// Percent-encode everything that is not an unreserved char — covers the spaces
// and ';' in a "Backlog ..." command string.
static String urlEncodeCmnd(const String &s)
{
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      out += c;
    else
    {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Resolve plugHttpHost to an IPAddress, cached. A literal dotted-quad is used as
// is. An mDNS ".local" name goes through MDNS.queryHost(); anything else through
// DNS. Re-resolves only when the cache is empty, or every
// PLUG_HOST_RERESOLVE_INTERVAL. Returns false only when nothing usable is known.
static bool resolvePlugHost()
{
  if (plugHttpHost.isEmpty())
    return false;

  IPAddress literal;
  if (literal.fromString(plugHttpHost))
  {
    plugHttpHostIp = literal;
    return true;
  }

  bool haveCache = (plugHttpHostIp != IPAddress());
  unsigned long nowMs = millis();
  if (haveCache && nowMs - lastPlugHostResolve < PLUG_HOST_RERESOLVE_INTERVAL)
    return true;
  lastPlugHostResolve = nowMs;

  // One-time mDNS responder init — only ever runs on an HTTP-transport device.
  static bool mdnsStarted = false;
  if (!mdnsStarted)
    mdnsStarted = MDNS.begin(("sf500-" + lastSix).c_str());

  IPAddress resolved;
  if (plugHttpHost.endsWith(".local"))
  {
    String name = plugHttpHost.substring(0, plugHttpHost.length() - 6);
    resolved = MDNS.queryHost(name, 1500);
  }
  else
  {
    IPAddress tmp;
    if (WiFi.hostByName(plugHttpHost.c_str(), tmp) == 1)
      resolved = tmp;
  }

  if (resolved != IPAddress())
  {
    plugHttpHostIp = resolved;
    return true;
  }
  return haveCache; // fall back to a stale-but-usable cached IP if we had one
}

// Single GET to the plug's /cm endpoint. body holds the response on HTTP 200.
// Uses a throwaway WiFiClient — never the MQTT socket (espClient) or the TLS
// client (secureClient).
static bool plugHttpGet(const String &cmnd, String &body)
{
  body = "";
  if (plugHttpHost.isEmpty() || !resolvePlugHost())
    return false;

  WiFiClient client;
  HTTPClient http;
  String path = "/cm?cmnd=" + urlEncodeCmnd(cmnd);
  if (!http.begin(client, plugHttpHostIp.toString(), 80, path))
    return false;
  http.setConnectTimeout(PLUG_HTTP_TIMEOUT_MS);
  http.setTimeout(PLUG_HTTP_TIMEOUT_MS);
  int code = http.GET();
  if (code == HTTP_CODE_OK)
    body = http.getString();
  http.end();
  return code == HTTP_CODE_OK;
}

// POST relay_03 status to relay_metrics — call on an actual transition only.
// Shared by the stat/POWER handler (MQTT) and the HTTP command / poll paths.
void logR3Transition(bool newState)
{
  if (WiFi.status() != WL_CONNECTED)
    return;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/relay_metrics";
  if (!http.begin(secureClient, url))
    return;

  StaticJsonDocument<128> logDoc;
  logDoc["device"]   = deviceName;
  logDoc["relay_id"] = "relay_03";
  logDoc["status"]   = newState ? 1 : 0;
  String postPayload;
  serializeJson(logDoc, postPayload);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(4000);
  http.POST(postPayload);
  http.end();
}

// Drive the plug ON/OFF over HTTP. Mirrors the MQTT Backlog/PulseTime logic in
// writePlugRelay(). Commits r3State + logs the transition only on a confirmed
// HTTP 200; a failure leaves r3State untouched for pollPlugHttpState() to
// reconcile. One immediate retry.
static void plugHttpCommand(bool state)
{
  String cmnd;
  if (state && r3Duration > 0)
  {
    uint32_t dur  = (r3Duration > 64800) ? 64800 : (uint32_t)r3Duration;
    uint32_t pval = (dur < 12) ? dur * 10 : dur + 100;
    cmnd = "Backlog PulseTime1 " + String((unsigned long)pval) + "; Power1 1";
  }
  else if (state)
  {
    cmnd = "Power1 1";
  }
  else
  {
    cmnd = "Backlog PulseTime1 0; Power1 0";
  }

  String body;
  bool ok = plugHttpGet(cmnd, body);
  if (!ok)
  {
    delay(PLUG_HTTP_RETRY_DELAY_MS);
    ok = plugHttpGet(cmnd, body);
  }

  if (ok)
  {
    bool changed = (state != r3State);
    r3State            = state;
    plugHttpReachable  = true;
    plugHttpFailStreak = 0;
    if (changed)
      logR3Transition(state);
    LOGF("R3 (Plug/HTTP) -> %s\n", state ? "ON" : "OFF");
  }
  else
  {
    LOGLN("[R3] HTTP command to plug failed");
  }
  publishRelayStatus();
}

// Poll the plug's current Power state over HTTP. Called from loop() every
// PLUG_HTTP_POLL_INTERVAL while plugUseHttp is true. Debounces "offline" over
// PLUG_OFFLINE_FAIL_STREAK consecutive failures; reconciles r3State against the
// plug's real state (external toggles, PulseTime expiry, plug reboot).
void pollPlugHttpState()
{
  String body;
  if (!plugHttpGet("Power", body))
  {
    if (plugHttpFailStreak < 255)
      plugHttpFailStreak++;
    if (plugHttpFailStreak >= PLUG_OFFLINE_FAIL_STREAK)
      plugHttpReachable = false;
    return;
  }

  plugHttpFailStreak = 0;
  plugHttpReachable  = true;

  String up = body;
  up.toUpperCase();
  bool newState;
  if (up.indexOf("\"ON\"") >= 0)
    newState = true;
  else if (up.indexOf("\"OFF\"") >= 0)
    newState = false;
  else
    return; // unparseable — leave state as-is

  if (newState != r3State)
  {
    logR3Transition(newState);
    r3State = newState;
    publishRelayStatus();
  }
}

// =====================================================
// WRITE PLUG RELAY — publish MQTT command to Tasmota plug
// =====================================================

void writePlugRelay(bool state)
{
  if (plugUseHttp)
  {
    plugHttpCommand(state); // owns r3State / logging / publishRelayStatus on success
    return;
  }

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
