#pragma once

// =====================================================
// GLOBALS.H — Shared types, structs, and extern declarations
// All variables are DEFINED in main.cpp
// =====================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include "config.h"

// =====================================================
// TYPES & STRUCTS
// =====================================================

enum WiFiState
{
  STATE_PORTAL,
  STATE_CONNECTING,
  STATE_ONLINE
};

struct NetItem
{
  String  ssid;
  int32_t rssi;
  bool    secure;
};

struct SensorData
{
  float ec       = 0.0f;
  float temp     = 0.0f;
  float wl       = 0.0f;
  float ambHumid = 0.0f;
  float ambTemp  = 0.0f;
  float ambLux   = 0.0f;
  float rainfall = 0.0f; // mm (rain bucket: raw register * 0.1)
  bool  hasData  = false;
};

struct Schedule
{
  uint32_t id;
  String   name;
  uint8_t  relayNum;
  uint8_t  hour;
  uint8_t  minute;
  uint16_t duration;
  bool     enabled;
  bool     days[7];
};

// =====================================================
// GLOBAL VARIABLE DECLARATIONS (defined in main.cpp)
// =====================================================

// Core objects
extern WiFiClient       espClient;
extern WiFiClientSecure secureClient;
extern PubSubClient     mqttClient;
extern ModbusMaster     modbus;
extern Preferences      wifiPrefs;
extern WebServer        portalServer;
extern DNSServer        dnsServer;

// WiFi state
extern WiFiState            wifiState;
extern std::vector<NetItem> scanList;
extern bool                 portalMode;
extern unsigned long        portalConnectStartMs;
extern unsigned long        portalStartedAt;

// Device identity
extern String deviceMAC;
extern String deviceName;
extern String lastSix;
extern bool   isRegistered;

// MQTT topics
extern String mqttTopicData;
extern String topicRelayUpdate;
extern String topicRelayStatus;
extern String topicWifiCmd;

// Pending WiFi commands (set in callback, executed in loop to avoid re-entrancy)
extern bool pendingWifiForget;
extern bool pendingWifiPortal;

// Sensor state
extern uint8_t    ecSensorId;
extern uint8_t    wlSensorId;
extern uint8_t    ambSensorId;
extern uint8_t    rainSensorId;
extern int        lastRainResetDay; // local-day-of-month of last rain counter reset; -1 if never
extern bool       ecSensorFound;
extern bool       wlSensorFound;
extern bool       ambSensorFound;
extern bool       rainSensorFound;
extern SensorData sensors;

// Relay state
extern bool          relayStates[2];
extern unsigned long relayTimers[2];
extern unsigned int  relayDurations[2];

// EC automation — config
extern bool          autoDosing;
extern bool          autoMixing;
extern float         ecTarget;
extern float         ecMinusHys;
extern unsigned int  dosingTime;

// Smart Dosing
extern bool          smartDosing;
extern bool          smartCalibrated;
extern bool          smartCalPhase;
extern float         ecRiseRate;
extern float         wlDropRate;
extern float         wlBeforeCal;
extern float         wlAtCal;           // WL at calibration time — persisted to NVS for WL correction
extern unsigned int  computedDoseTime;
extern unsigned int  actualCalDuration; // actual calibration dose length used (≥ SMART_CAL_DURATION)
extern int           calRetryCount;     // consecutive calibration failures (separate from production doses)

// EC automation — rolling average
extern float         ecReadings[EC_SAMPLES];
extern int           ecReadingIndex;
extern int           ecReadingCount;
extern float         ecAverage;

// EC automation — state machine
enum AutoDosingState {
  AUTO_IDLE,
  AUTO_STARTUP_WAIT,
  AUTO_SAMPLING,
  AUTO_PRE_MIX,
  AUTO_DOSING,
  AUTO_POST_MIX,
  AUTO_COOLDOWN,
  AUTO_STABILISING,
  AUTO_ALARM
};
extern AutoDosingState autoState;
extern unsigned long   autoStateEnteredAt;
extern float           preDoseEC;
extern int             consecutiveIneffectiveDoses;
extern int             dosesToday;
extern int             lastDoseDay;
extern time_t          lastDoseTimestamp;
extern unsigned long   doseEndTime;
extern int             stabiliseSkipCount;
extern unsigned int    activeDoseTime;     // actual duration used this cycle (smart or fixed)
extern unsigned int    minWlDosing;        // min water level mm before dosing fires (0=disabled)
extern bool            lastDoseAborted;    // true if last dose ended via P0 early abort
extern unsigned int    actualDoseElapsed;  // seconds the relay actually ran (< activeDoseTime on abort)

// Timing state
extern unsigned long lastSensorRead;
extern unsigned long lastSensorUpload;
extern unsigned long lastStatusUpdate;
extern unsigned long lastConfigCheck;
extern unsigned long lastScheduleCheck;
extern unsigned long lastScheduleFetch;
extern unsigned long lastOTACheck;

// Schedule storage
extern Schedule      schedules[MAX_SCHEDULES];
extern int           scheduleCount;
extern unsigned long lastTriggeredTime[MAX_SCHEDULES];

// Startup protection
extern unsigned long startupTime;
extern bool          startupComplete;

// Remote serial logging
#ifdef ENABLE_OTA_LOGS
extern String         topicLogs;
extern unsigned long  lastLogPublish;
#endif
