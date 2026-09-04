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
#include <vector>
#include <cstring>

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

// ===========================================================================
// Pure helpers (host-tested) — raw char ops so they build on both Arduino
// String and std::string.
// ===========================================================================

String journalDropPrefix(const String& all, size_t offset)
{
  const char* p = all.c_str();
  size_t n = all.length();
  if (offset >= n) return String();

  size_t start;
  if (offset == 0 || p[offset - 1] == '\n')
  {
    start = offset;                       // already at a line boundary
  }
  else
  {
    size_t i = offset;                    // mid-line — snap past the partial line
    while (i < n && p[i] != '\n') i++;
    if (i >= n) return String();
    start = i + 1;
  }

  String out;
  out.reserve(n - start);
  for (size_t i = start; i < n; i++) out += p[i];
  return out;
}

// A journalled line is a sensor_metrics row if its `tbl` field is
// "sensor_metrics". Substring match on the raw line — the sensor row body never
// contains that string, so this agrees with journalDecode's classification.
bool journalLineIsSensorMetrics(const char* p, size_t len)
{
  for (size_t k = 0; k + 14 <= len; k++)
    if (memcmp(p + k, "sensor_metrics", 14) == 0) return true;
  return false;
}

String journalEvictSensorMetrics(const String& all, size_t targetBytes)
{
  const char* p = all.c_str();
  size_t n = all.length();

  struct Line { size_t s, e; bool sensor; };
  std::vector<Line> lines;
  for (size_t i = 0; i < n; )
  {
    size_t j = i;
    while (j < n && p[j] != '\n') j++;
    if (j >= n) break;                    // ignore a torn trailing line
    j++;                                  // include the newline
    lines.push_back({ i, j, journalLineIsSensorMetrics(p + i, j - i) });
    i = j;
  }

  size_t auditBytes = 0;
  for (const auto& l : lines)
    if (!l.sensor) auditBytes += (l.e - l.s);

  size_t sensorBudget = (targetBytes > auditBytes) ? targetBytes - auditBytes : 0;

  std::vector<bool> keep(lines.size(), false);
  size_t used = 0;
  for (size_t idx = lines.size(); idx-- > 0; )
  {
    if (!lines[idx].sensor) continue;
    size_t sz = lines[idx].e - lines[idx].s;
    if (used + sz > sensorBudget) break;  // stop at the first that doesn't fit
    used += sz;
    keep[idx] = true;
  }

  String out;
  out.reserve(auditBytes + used);
  for (size_t idx = 0; idx < lines.size(); idx++)
  {
    if (lines[idx].sensor && !keep[idx]) continue;
    for (size_t k = lines[idx].s; k < lines[idx].e; k++) out += p[k];
  }
  return out;
}

// ===========================================================================
// Device-side (SD-backed) — not compiled for the native host-test env.
// ===========================================================================
#ifndef UNIT_TEST

#include "sdcard.h"
#include "persist.h"   // clockIsApprox()
#include "logger.h"
#include <time.h>
#include <esp_task_wdt.h>

bool journalAppend(const char* tbl, const String& rowJson)
{
  if (!sdMounted()) return false;
  String line = journalEncode(time(nullptr), clockIsApprox(), tbl, rowJson);
  return sdAppendLine(JOURNAL_PATH, line);
}

size_t journalReadOffset()
{
  String s;
  if (!sdReadFile(JOURNAL_OFFSET_PATH, s)) return 0;
  long v = s.toInt();
  return v > 0 ? (size_t)v : 0;
}

void journalWriteOffset(size_t off)
{
  char b[16];
  snprintf(b, sizeof(b), "%lu", (unsigned long)off);
  sdAtomicWrite(JOURNAL_OFFSET_PATH, (const uint8_t*)b, strlen(b));
}

size_t journalPendingBytes()
{
  size_t sz  = sdFileSize(JOURNAL_PATH);
  size_t off = journalReadOffset();
  return sz > off ? sz - off : 0;
}

int journalNextBatch(size_t fromOffset, JournalRec* recs, size_t* ends, int maxRecs)
{
  // A batch of maxRecs lines is at most a few KB — read a bounded window, not
  // the whole (potentially multi-MB) journal.
  const size_t WINDOW = 8192;
  String win;
  if (!sdReadRange(JOURNAL_PATH, fromOffset, WINDOW, win)) return 0;

  const char* p = win.c_str();
  size_t n = win.length();
  int cnt = 0;
  size_t pos = 0;
  while (cnt < maxRecs && pos < n)
  {
    size_t nl = pos;
    while (nl < n && p[nl] != '\n') nl++;
    if (nl >= n) break;                   // no complete line left in the window

    String line;
    line.reserve(nl - pos);
    for (size_t k = pos; k < nl; k++) line += p[k];

    if (journalDecode(line, recs[cnt]))
    {
      ends[cnt] = fromOffset + nl + 1;    // absolute byte offset past this line
      cnt++;
    }
    pos = nl + 1;
  }
  return cnt;
}

void journalCompact()
{
  // Stream the un-consumed tail into a fresh file — bounded RAM.
  if (sdStreamDropPrefix(JOURNAL_PATH, journalReadOffset()))
    journalWriteOffset(0);
}

// Forward-scan the whole journal WINDOW bytes at a time, summing bytes of
// sensor_metrics lines vs everything else. Bounded RAM. false on a read error
// or a single line longer than the window (corruption).
static bool journalScanSizes(size_t& auditBytes, size_t& sensorBytes)
{
  const size_t WIN = 4096;
  auditBytes = 0;
  sensorBytes = 0;
  size_t pos = 0;
  String win;
  for (;;)
  {
    if (!sdReadRange(JOURNAL_PATH, pos, WIN, win)) return false;
    size_t n = win.length();
    if (n == 0) return true;                       // EOF
    const char* p = win.c_str();
    size_t lineStart = 0;
    bool sawNl = false;
    for (size_t i = 0; i < n; i++)
    {
      if (p[i] != '\n') continue;
      size_t len = i + 1 - lineStart;
      if (journalLineIsSensorMetrics(p + lineStart, len)) sensorBytes += len;
      else                                               auditBytes  += len;
      lineStart = i + 1;
      sawNl = true;
    }
    if (!sawNl) return n < WIN;                    // EOF torn tail = ok; full window, no NL = corrupt
    pos += lineStart;
    if (n < WIN) return true;                      // consumed to EOF
    esp_task_wdt_reset();
  }
}

// Second pass: stream the journal into a fresh file, skipping the oldest
// `dropBytes` worth of sensor_metrics lines and copying every other line
// verbatim. Bounded RAM (one 4KB window + the SD write buffer), one open/close.
static bool journalRewriteDropOldestSensor(size_t dropBytes)
{
  const size_t WIN = 4096;
  if (!sdRewriteBegin(JOURNAL_PATH)) return false;

  size_t pos = 0, dropped = 0;
  String win;
  bool ok = true;
  for (;;)
  {
    if (!sdReadRange(JOURNAL_PATH, pos, WIN, win)) { ok = false; break; }
    size_t n = win.length();
    if (n == 0) break;
    const char* p = win.c_str();
    size_t lineStart = 0;
    bool sawNl = false;
    for (size_t i = 0; i < n; i++)
    {
      if (p[i] != '\n') continue;
      size_t s = lineStart, len = i + 1 - lineStart;
      bool sensor = journalLineIsSensorMetrics(p + s, len);
      if (sensor && dropped < dropBytes)
        dropped += len;                             // discard this old sensor row
      else if (!sdRewriteAppend((const uint8_t*)(p + s), len)) { ok = false; break; }
      lineStart = i + 1;
      sawNl = true;
    }
    if (!ok) break;
    if (!sawNl) { ok = (n < WIN); break; }          // torn tail at EOF is fine
    pos += lineStart;
    if (n < WIN) break;
    esp_task_wdt_reset();
  }

  if (!ok) { sdRewriteAbort(); return false; }
  return sdRewriteCommit();
}

void journalEnforceRetention()
{
  if (!sdMounted()) return;
  if (sdFileSize(JOURNAL_PATH) <= JOURNAL_MAX_BYTES) return;

  // Clear the already-drained prefix first (streaming, bounded) so the cap
  // applies to live data only and the rewrite below can start from offset 0.
  journalCompact();

  size_t total = sdFileSize(JOURNAL_PATH);
  if (total <= JOURNAL_MAX_BYTES) return;

  // Over the hard cap — an outage of ~256 days at the worst-case fill rate.
  size_t auditBytes = 0, sensorBytes = 0;
  if (!journalScanSizes(auditBytes, sensorBytes))
  {
    LOGLN("[journal] retention: scan failed (corrupt journal) — file left intact");
    return;
  }

  const size_t target       = JOURNAL_MAX_BYTES - JOURNAL_MAX_BYTES / 10;   // 90%
  size_t       sensorBudget  = target > auditBytes ? target - auditBytes : 0;
  size_t       dropBytes     = sensorBytes > sensorBudget ? sensorBytes - sensorBudget : 0;

  if (dropBytes == 0)
  {
    LOGF("[journal] over cap (%lu B) but it is almost entirely audit rows — cannot trim\n",
         (unsigned long)total);
    return;
  }

  if (journalRewriteDropOldestSensor(dropBytes))
  {
    journalWriteOffset(0);
    LOGF("[journal] retention: %lu -> ~%lu B (dropped ~%lu B oldest sensor rows, kept %lu B audit)\n",
         (unsigned long)total, (unsigned long)(total - dropBytes),
         (unsigned long)dropBytes, (unsigned long)auditBytes);
  }
  else
  {
    LOGLN("[journal] retention: rewrite failed — file left intact");
  }
}

#endif  // UNIT_TEST
