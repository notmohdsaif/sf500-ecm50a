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
