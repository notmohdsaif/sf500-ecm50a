#pragma once
#include "config.h"

// =====================================================
// LOGGER.H — Remote serial logging over MQTT
// When ENABLE_OTA_LOGS is defined: log lines are buffered
// in a ring buffer and flushed to sf500/{lastSix}/logs
// every LOG_PUBLISH_INTERVAL ms.
// When undefined: macros collapse to plain Serial calls.
// =====================================================

#ifdef ENABLE_OTA_LOGS
  void logWrite(const char* fmt, ...);
  void publishPendingLogs();
  #define LOGF(fmt, ...)  logWrite(fmt, ##__VA_ARGS__)
  #define LOGLN(msg)      logWrite("%s\n", (const char*)(msg))
  #define LOGLNS(msg)     logWrite("%s\n", (msg).c_str())
#else
  #define LOGF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define LOGLN(msg)      Serial.println(msg)
  #define LOGLNS(msg)     Serial.println(msg)
  inline void publishPendingLogs() {}
#endif
