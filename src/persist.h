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
  uint8_t  ecSensorId, wlSensorId, ambSensorId, rainSensorId, metSensorId;
  bool     ecFound, wlFound, ambFound, rainFound, metFound;
  int      lastRainResetDay;
};

// Serialize to a single-line JSON object.
String configToJson(const LocalConfig& c);

// Parse back. Returns false on a JSON error or a missing required key
// (ecTarget, dosingTime). Unset optional keys take documented defaults.
bool configFromJson(const String& json, LocalConfig& c);

// ---------------------------------------------------------------------------
// Device-side persistence (not built in the native host-test env).
// ---------------------------------------------------------------------------
#ifndef UNIT_TEST

// Config: NVS scalars ("cfg" namespace) + a full SD mirror at
// /config/device.json. Written only when fetchDeviceConfig() reports a change.
void snapshotGlobalsToConfig(LocalConfig& c);   // fill c from the live globals
void applyConfigToGlobals(const LocalConfig& c);
bool persistConfig();                            // globals -> NVS + SD
bool loadConfigLocal();                          // NVS (or SD fallback) -> globals; calls setConfigLoaded()

// Schedules: SD only (/config/schedules.json), rewritten on every fetch.
void persistSchedules();
bool loadSchedulesLocal();                       // -> schedules[] / scheduleCount

// Coarse wall clock across an offline reboot (the board has no battery RTC).
void persistClock();          // time(nullptr) -> /state/clock + NVS, every ~5 min
bool seedClockFromStore();    // on an offline boot, settimeofday() from the store
void noteNtpSynced();         // clears the approx flag; logs the correction delta
bool clockIsApprox();         // true while running on a seeded (non-NTP) clock

#endif  // UNIT_TEST
