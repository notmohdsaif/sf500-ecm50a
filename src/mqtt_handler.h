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

// Tasmota plug — local HTTP transport (docs/plug-http-control.md)
void logR3Transition(bool newState);   // POST relay_03 status to relay_metrics on an actual transition
void pollPlugHttpState();               // read the plug's Power state over HTTP (called from loop() in http mode)
