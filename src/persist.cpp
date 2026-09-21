// =====================================================
// PERSIST.CPP — local config + coarse-clock persistence
//
// Pure wire-format logic (configToJson / configFromJson) is compiled both for
// the ESP32 and the native host-test env. The NVS + SD + globals-touching
// helpers added in Tasks 2.2 / 2.3 / 2.5 live under #ifndef UNIT_TEST.
// =====================================================

#include "persist.h"
#include <ArduinoJson.h>

String configToJson(const LocalConfig& c)
{
  StaticJsonDocument<1024> d;
  d["autoDosing"]     = c.autoDosing;
  d["ecTarget"]       = c.ecTarget;
  d["dosingTime"]     = c.dosingTime;
  d["autoMixing"]     = c.autoMixing;
  d["smartDosing"]    = c.smartDosing;
  d["minWlDosing"]    = c.minWlDosing;
  d["plugMode"]       = c.plugMode;
  d["refillCutoffMm"] = c.refillCutoffMm;
  d["plugEnabled"]    = c.plugEnabled;
  d["plugTopic"]      = c.plugTopic;
  d["plugHost"]       = c.plugHost;
  d["ecSensorId"]     = c.ecSensorId;
  d["wlSensorId"]     = c.wlSensorId;
  d["ambSensorId"]    = c.ambSensorId;
  d["rainSensorId"]   = c.rainSensorId;
  d["ecFound"]        = c.ecFound;
  d["wlFound"]        = c.wlFound;
  d["ambFound"]       = c.ambFound;
  d["rainFound"]      = c.rainFound;
  d["lastRainResetDay"] = c.lastRainResetDay;

  String s;
  serializeJson(d, s);
  return s;
}

bool configFromJson(const String& json, LocalConfig& c)
{
  StaticJsonDocument<1024> d;
  if (deserializeJson(d, json) != DeserializationError::Ok) return false;
  if (!d.containsKey("ecTarget") || !d.containsKey("dosingTime")) return false;

  c.autoDosing  = d["autoDosing"]  | false;
  c.ecTarget    = d["ecTarget"]    | 1.5f;
  c.dosingTime  = d["dosingTime"]  | 30;
  c.autoMixing  = d["autoMixing"]  | false;
  c.smartDosing = d["smartDosing"] | false;
  c.minWlDosing = d["minWlDosing"] | 0;
  strlcpy(c.plugMode, d["plugMode"] | "custom", sizeof(c.plugMode));
  c.refillCutoffMm = d["refillCutoffMm"] | 0.0f;
  c.plugEnabled    = d["plugEnabled"]    | false;
  strlcpy(c.plugTopic, d["plugTopic"] | "", sizeof(c.plugTopic));
  strlcpy(c.plugHost,  d["plugHost"]  | "", sizeof(c.plugHost));
  c.ecSensorId   = d["ecSensorId"]   | 0;
  c.wlSensorId   = d["wlSensorId"]   | 0;
  c.ambSensorId  = d["ambSensorId"]  | 0;
  c.rainSensorId = d["rainSensorId"] | 0;
  c.ecFound   = d["ecFound"]   | false;
  c.wlFound   = d["wlFound"]   | false;
  c.ambFound  = d["ambFound"]  | false;
  c.rainFound = d["rainFound"] | false;
  c.lastRainResetDay = d["lastRainResetDay"] | -1;
  return true;
}

// ===========================================================================
// Device-side persistence — not compiled for the native host-test env.
// ===========================================================================
#ifndef UNIT_TEST

#include "globals.h"
#include "sdcard.h"
#include "netstate.h"
#include "logger.h"
#include "cloud.h"        // logDeviceActivity()
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

static Preferences cfgnvs;

// --- config <-> globals ----------------------------------------------------

void snapshotGlobalsToConfig(LocalConfig& c)
{
  c.autoDosing  = autoDosing;
  c.ecTarget    = ecTarget;
  c.dosingTime  = dosingTime;
  c.autoMixing  = autoMixing;
  c.smartDosing = smartDosing;
  c.minWlDosing = minWlDosing;
  strlcpy(c.plugMode, plugMode.c_str(), sizeof(c.plugMode));
  c.refillCutoffMm = refillCutoffMm;
  c.plugEnabled    = tasmotaPlugEnabled;
  strlcpy(c.plugTopic, tasmotaPlugTopic.c_str(), sizeof(c.plugTopic));
  strlcpy(c.plugHost,  plugHttpHost.c_str(),     sizeof(c.plugHost));
  c.ecSensorId   = ecSensorId;
  c.wlSensorId   = wlSensorId;
  c.ambSensorId  = ambSensorId;
  c.rainSensorId = rainSensorId;
  c.ecFound   = ecSensorFound;
  c.wlFound   = wlSensorFound;
  c.ambFound  = ambSensorFound;
  c.rainFound = rainSensorFound;
  c.lastRainResetDay = lastRainResetDay;
}

void applyConfigToGlobals(const LocalConfig& c)
{
  autoDosing  = c.autoDosing;
  ecTarget    = c.ecTarget;
  ecMinusHys  = ecTarget - EC_HYSTERESIS;
  dosingTime  = c.dosingTime;
  autoMixing  = c.autoMixing;
  smartDosing = c.smartDosing;
  minWlDosing = c.minWlDosing;
  plugMode    = c.plugMode;
  refillCutoffMm     = c.refillCutoffMm;
  tasmotaPlugEnabled = c.plugEnabled;
  tasmotaPlugTopic   = c.plugTopic;
  plugHttpHost       = c.plugHost;
  ecSensorId   = c.ecSensorId;
  wlSensorId   = c.wlSensorId;
  ambSensorId  = c.ambSensorId;
  rainSensorId = c.rainSensorId;
  ecSensorFound   = c.ecFound;
  wlSensorFound   = c.wlFound;
  ambSensorFound  = c.ambFound;
  rainSensorFound = c.rainFound;
  lastRainResetDay = c.lastRainResetDay;
  plugUseHttp = tasmotaPlugEnabled && plugHttpHost.length() > 0;
}

bool persistConfig()
{
  LocalConfig c{};
  snapshotGlobalsToConfig(c);

  cfgnvs.begin("cfg", false);
  cfgnvs.putBool ("autoDosing",  c.autoDosing);
  cfgnvs.putFloat("ecTarget",    c.ecTarget);
  cfgnvs.putUInt ("dosingTime",  c.dosingTime);
  cfgnvs.putBool ("autoMixing",  c.autoMixing);
  cfgnvs.putBool ("smartDosing", c.smartDosing);
  cfgnvs.putUInt ("minWlDosing", c.minWlDosing);
  cfgnvs.putString("plugMode",   c.plugMode);
  cfgnvs.putFloat("refillCut",   c.refillCutoffMm);
  cfgnvs.putBool ("plugEnabled", c.plugEnabled);
  cfgnvs.putString("plugTopic",  c.plugTopic);
  cfgnvs.putString("plugHost",   c.plugHost);
  cfgnvs.putUChar("ecId",   c.ecSensorId);
  cfgnvs.putUChar("wlId",   c.wlSensorId);
  cfgnvs.putUChar("ambId",  c.ambSensorId);
  cfgnvs.putUChar("rainId", c.rainSensorId);
  cfgnvs.putUChar("found",
    (c.ecFound ? 1 : 0) | (c.wlFound ? 2 : 0) | (c.ambFound ? 4 : 0) | (c.rainFound ? 8 : 0));
  cfgnvs.putInt("rainDay", c.lastRainResetDay);
  cfgnvs.end();

  String js = configToJson(c);
  sdAtomicWrite("/config/device.json", (const uint8_t*)js.c_str(), js.length());
  return true;   // NVS is the guaranteed store; SD is best-effort
}

bool loadConfigLocal()
{
  LocalConfig c{};

  cfgnvs.begin("cfg", true);
  bool haveNvs = cfgnvs.isKey("ecTarget");
  if (haveNvs)
  {
    c.autoDosing  = cfgnvs.getBool ("autoDosing",  false);
    c.ecTarget    = cfgnvs.getFloat("ecTarget",    1.5f);
    c.dosingTime  = cfgnvs.getUInt ("dosingTime",  30);
    c.autoMixing  = cfgnvs.getBool ("autoMixing",  false);
    c.smartDosing = cfgnvs.getBool ("smartDosing", false);
    c.minWlDosing = cfgnvs.getUInt ("minWlDosing", 0);
    cfgnvs.getString("plugMode", c.plugMode, sizeof(c.plugMode));
    if (c.plugMode[0] == '\0') strlcpy(c.plugMode, "custom", sizeof(c.plugMode));
    c.refillCutoffMm = cfgnvs.getFloat("refillCut", 0.0f);
    c.plugEnabled    = cfgnvs.getBool ("plugEnabled", false);
    cfgnvs.getString("plugTopic", c.plugTopic, sizeof(c.plugTopic));
    cfgnvs.getString("plugHost",  c.plugHost,  sizeof(c.plugHost));
    c.ecSensorId   = cfgnvs.getUChar("ecId",   0);
    c.wlSensorId   = cfgnvs.getUChar("wlId",   0);
    c.ambSensorId  = cfgnvs.getUChar("ambId",  0);
    c.rainSensorId = cfgnvs.getUChar("rainId", 0);
    uint8_t f = cfgnvs.getUChar("found", 0);
    c.ecFound = f & 1; c.wlFound = f & 2; c.ambFound = f & 4; c.rainFound = f & 8;
    c.lastRainResetDay = cfgnvs.getInt("rainDay", -1);
  }
  cfgnvs.end();

  if (!haveNvs)
  {
    String js;
    if (!sdReadFile("/config/device.json", js) || !configFromJson(js, c))
      return false;
  }

  applyConfigToGlobals(c);
  setConfigLoaded();
  LOGF("[persist] local config loaded (ecTarget %.2f, autoDosing %d, %d schedules pending)\n",
       c.ecTarget, c.autoDosing, scheduleCount);
  return true;
}

// --- schedules -----------------------------------------------------------

void persistSchedules()
{
  DynamicJsonDocument d(8192);
  if (d.capacity() == 0) { LOGLN("[persist] schedule alloc failed (low heap)"); return; }
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < scheduleCount; i++)
  {
    Schedule& s = schedules[i];
    JsonObject o = a.createNestedObject();
    o["id"]       = s.id;
    o["name"]     = s.name;
    o["relayNum"] = s.relayNum;
    o["hour"]     = s.hour;
    o["minute"]   = s.minute;
    o["duration"] = s.duration;
    o["enabled"]  = s.enabled;
    JsonArray days = o.createNestedArray("days");
    for (int k = 0; k < 7; k++) days.add(s.days[k]);
  }
  String js;
  serializeJson(a, js);

  // fetchSchedules() calls this every 60s. Skip the card write when nothing
  // changed — one read + compare is far cheaper than an atomic rewrite.
  String cur;
  if (sdReadFile("/config/schedules.json", cur) && cur == js)
    return;

  sdAtomicWrite("/config/schedules.json", (const uint8_t*)js.c_str(), js.length());
}

bool loadSchedulesLocal()
{
  String js;
  if (!sdReadFile("/config/schedules.json", js)) return false;

  DynamicJsonDocument d(8192);
  if (d.capacity() == 0) return false;
  if (deserializeJson(d, js) != DeserializationError::Ok) return false;

  scheduleCount = 0;
  for (JsonObject o : d.as<JsonArray>())
  {
    if (scheduleCount >= MAX_SCHEDULES) break;
    Schedule& s = schedules[scheduleCount++];
    s.id       = o["id"];
    s.name     = o["name"].as<String>();
    s.relayNum = o["relayNum"];
    s.hour     = o["hour"];
    s.minute   = o["minute"];
    s.duration = o["duration"];
    s.enabled  = o["enabled"];
    int k = 0;
    for (JsonVariant v : o["days"].as<JsonArray>())
      if (k < 7) s.days[k++] = v.as<bool>();
  }
  LOGF("[persist] %d schedule(s) loaded from SD\n", scheduleCount);
  return true;
}

// --- coarse clock ------------------------------------------------------

static bool   approxClock        = false;
static time_t storedEpochAtBoot  = 0;

void persistClock()
{
  time_t t = time(nullptr);
  if (t < 1000000000) return;

  // NVS every call (wear-levelled, designed for frequent small writes);
  // the SD copy — an atomic temp-write + rename — only hourly, to spare the
  // card. NVS is the primary source seedClockFromStore() reads first anyway.
  cfgnvs.begin("cfg", false);
  cfgnvs.putULong("epoch", (uint32_t)t);
  cfgnvs.end();

  static unsigned long lastSdFlush = 0;
  unsigned long nowMs = millis();
  if (lastSdFlush == 0 || nowMs - lastSdFlush >= 3600000UL)
  {
    lastSdFlush = nowMs;
    char b[16];
    snprintf(b, sizeof(b), "%ld", (long)t);
    sdAtomicWrite("/state/clock", (const uint8_t*)b, strlen(b));
  }
}

bool seedClockFromStore()
{
  if (time(nullptr) >= 1000000000) return true;   // real time already set

  uint32_t e = 0;
  cfgnvs.begin("cfg", true);
  e = cfgnvs.getULong("epoch", 0);
  cfgnvs.end();
  if (e < 1000000000)
  {
    String s;
    if (sdReadFile("/state/clock", s)) e = (uint32_t)s.toInt();
  }
  if (e < 1000000000) return false;

  struct timeval tv;
  tv.tv_sec  = (time_t)e;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  // settimeofday() alone doesn't set TZ — same fix as cellular.cpp's NITZ path
  // (see its comment). Without this, localtime_r() renders unshifted UTC
  // digits on a cold offline boot (no NTP/modem sync yet this session) while
  // isoNow()/isoFromEpoch() still stamp them "+08:00" — an 8h-off recorded_at,
  // reproducing the D2 bug through this path instead of the one it fixed.
  setenv("TZ", "UTC-8", 1);
  tzset();

  storedEpochAtBoot = (time_t)e;
  approxClock = true;
  LOGF("[clock] seeded from store: %u (approx)\n", (unsigned)e);
  return true;
}

void noteNtpSynced()
{
  if (approxClock && storedEpochAtBoot)
  {
    long delta = (long)time(nullptr) - (long)storedEpochAtBoot;
    char m[80];
    snprintf(m, sizeof(m), "Clock corrected after offline boot: %+lds", delta);
    logDeviceActivity("system", m);
  }
  approxClock = false;
}

bool clockIsApprox() { return approxClock; }

#endif  // UNIT_TEST
