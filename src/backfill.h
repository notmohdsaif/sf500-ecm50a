#pragma once
#include <Arduino.h>

// =====================================================
// BACKFILL.H — drain the SD telemetry journal to Supabase
//
// One bounded task: when an uplink is available and the device is not in a
// dose-critical phase, POST a small batch of journalled rows, advance the
// committed byte-offset, and compact / enforce retention. Rate-limited so a
// reconnect backlog never starves MQTT keepalive or the loop-task watchdog.
// =====================================================

#define BACKFILL_BATCH            8
#define BACKFILL_MIN_INTERVAL_MS  3000UL

// Call from loop() every iteration — it self-rate-limits and no-ops when
// there is nothing to do.
void backfillTick();
