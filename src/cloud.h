#pragma once
#include "globals.h"

// =====================================================
// CLOUD.H
// Supabase REST API + NTP time sync
// =====================================================

void syncTimeWithNTP();
void registerDevice();
void uploadSensorConfig();
void uploadSensorReadings();
void updateDeviceStatus(const char *status);
void fetchDeviceConfig();
void fetchSchedules();
void logDeviceActivity(const char *category, const char *action);
String isoNow();   // ISO-8601 +08:00 capture time, or "" before the clock is valid
String isoFromEpochUtc(time_t t);   // UTC ISO-8601 ("...Z") from a raw epoch, or "" if implausible
