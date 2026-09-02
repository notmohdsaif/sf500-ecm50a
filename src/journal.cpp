// =====================================================
// JOURNAL.CPP — NDJSON telemetry journal
//
// Pure wire-format logic (journalEncode / journalDecode) builds for both the
// ESP32 and the native host-test env. The SD-backed append / offset-read /
// compaction / retention functions added in Tasks 3.2 / 3.4 / 3.5 live under
// #ifndef UNIT_TEST.
// =====================================================

#include "journal.h"
#include <ArduinoJson.h>

static bool knownTable(const String& t)
{
  return t == "sensor_metrics" || t == "activity_log" || t == "relay_metrics";
}

String journalEncode(time_t t, bool approx, const char* tbl, const String& rowJson)
{
  // Build numeric parts with snprintf so this behaves identically on Arduino
  // String and the host's std::string (which has no operator+=(long)).
  char tbuf[24];
  snprintf(tbuf, sizeof(tbuf), "%ld", (long)t);

  String s;
  s.reserve(rowJson.length() + 48);
  s += "{\"t\":";
  s += tbuf;
  s += ",\"approx\":";
  s += approx ? "1" : "0";
  s += ",\"tbl\":\"";
  s += tbl;
  s += "\",\"row\":";
  s += rowJson;
  s += "}";
  return s;
}

bool journalDecode(const String& line, JournalRec& out)
{
  StaticJsonDocument<1024> d;
  if (deserializeJson(d, line) != DeserializationError::Ok) return false;
  if (!d.containsKey("t") || !d.containsKey("tbl") || !d.containsKey("row")) return false;

  out.t      = (time_t)(long)d["t"];
  out.approx = (d["approx"] | 0) != 0;
  out.tbl    = String((const char*)(d["tbl"] | ""));
  if (!knownTable(out.tbl)) return false;

  String r;
  serializeJson(d["row"], r);
  out.row = r;
  return true;
}
