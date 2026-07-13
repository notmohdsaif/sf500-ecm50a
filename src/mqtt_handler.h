#pragma once
#include "globals.h"

// =====================================================
// MQTT_HANDLER.H
// MQTT connection, command callback, relay status publish
// =====================================================

void reconnectMQTT();
void mqttCallback(char *topic, byte *payload, unsigned int length);
void publishRelayStatus(const char* r3Reason = nullptr);
void writePlugRelay(bool state);
