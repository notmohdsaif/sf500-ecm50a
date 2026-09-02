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
