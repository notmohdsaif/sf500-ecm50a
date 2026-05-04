// =====================================================
// LOGGER.CPP — Ring-buffer MQTT logger
// Only compiled when ENABLE_OTA_LOGS is defined.
// =====================================================

#include "logger.h"
#ifdef ENABLE_OTA_LOGS

#include "globals.h"
#include <stdarg.h>
#include <time.h>

#define LOG_SLOTS     48
#define LOG_SLOT_SIZE 160

static char s_buf[LOG_SLOTS][LOG_SLOT_SIZE];
static int  s_head  = 0;   // next slot to write
static int  s_count = 0;   // occupied slots (0 – LOG_SLOTS)

// =====================================================
// logWrite — format, echo to Serial, store in ring buffer
// =====================================================

void logWrite(const char* fmt, ...)
{
  char raw[LOG_SLOT_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(raw, sizeof(raw), fmt, args);
  va_end(args);

  // Physical serial output is unchanged
  Serial.print(raw);

  // Strip trailing CR/LF before storing
  int len = (int)strlen(raw);
  while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r'))
    raw[--len] = '\0';

  // Skip blank lines (e.g. bare newlines used as spacers)
  if (len == 0) return;

  // Prepend [HH:MM:SS] timestamp
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);

  char entry[LOG_SLOT_SIZE];
  snprintf(entry, sizeof(entry), "[%02d:%02d:%02d] %s",
           ti.tm_hour, ti.tm_min, ti.tm_sec, raw);

  strncpy(s_buf[s_head], entry, LOG_SLOT_SIZE - 1);
  s_buf[s_head][LOG_SLOT_SIZE - 1] = '\0';

  s_head = (s_head + 1) % LOG_SLOTS;
  if (s_count < LOG_SLOTS) s_count++;
}

// =====================================================
// publishPendingLogs — drain buffer to MQTT, then clear
// =====================================================

void publishPendingLogs()
{
  if (!mqttClient.connected() || s_count == 0) return;

  // Build newline-separated payload, oldest entry first
  static char payload[3000];
  int pos = 0;

  int start = (s_head - s_count + LOG_SLOTS) % LOG_SLOTS;

  for (int i = 0; i < s_count; i++)
  {
    int idx = (start + i) % LOG_SLOTS;
    int space = (int)sizeof(payload) - pos - 2; // leave room for \n and \0
    if (space <= 0) break;

    int written = snprintf(payload + pos, (size_t)(space + 1), "%s\n", s_buf[idx]);
    if (written < 0 || written > space) break;
    pos += written;
  }

  if (pos > 0)
    mqttClient.publish(topicLogs.c_str(), (const uint8_t*)payload, (unsigned int)pos, false);

  s_count = 0;
  s_head  = 0;
}

#endif // ENABLE_OTA_LOGS
