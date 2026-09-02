#pragma once
#include <Arduino.h>

// =====================================================
// PERSIST.H — local persistence of configuration + a coarse clock
//
// Lets sf500_107888 cold-start auto-dosing with no connectivity: the last
// config synced from Supabase is mirrored to NVS (dosing scalars) and SD
// (schedules + full snapshot), and reloaded at boot when the cloud is
// unreachable.
//
// This task (2.1) defines only the wire format. Tasks 2.2 / 2.3 / 2.5 add the
// NVS + SD read/write functions and the coarse-clock helpers; those touch the
// firmware globals and are wrapped in #ifndef UNIT_TEST so this file's pure
// JSON logic still builds in the native host-test env.
// =====================================================

struct LocalConfig {
  bool     autoDosing;
  float    ecTarget;
  uint32_t dosingTime;
  bool     autoMixing;
  bool     smartDosing;
  uint32_t minWlDosing;
  char     plugMode[12];
  float    refillCutoffMm;
  bool     plugEnabled;
  char     plugTopic[48];
  char     plugHost[40];
  uint8_t  ecSensorId, wlSensorId, ambSensorId, rainSensorId;
  bool     ecFound, wlFound, ambFound, rainFound;
  int      lastRainResetDay;
};

// Serialize to a single-line JSON object.
String configToJson(const LocalConfig& c);

// Parse back. Returns false on a JSON error or a missing required key
// (ecTarget, dosingTime). Unset optional keys take documented defaults.
bool configFromJson(const String& json, LocalConfig& c);
