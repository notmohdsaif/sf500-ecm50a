#pragma once
#include "globals.h"

// =====================================================
// RELAY.H
// Relay GPIO control, timed relay, schedule check, serial commands
// =====================================================

void writeRelay(uint8_t num, bool state);
void checkRelayTimers();
void checkSchedules();
void handleSerialCommands();
