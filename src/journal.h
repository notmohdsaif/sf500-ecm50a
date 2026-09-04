#pragma once
#include <Arduino.h>

// =====================================================
// JOURNAL.H — append-only NDJSON telemetry journal on SD
//
// One record per line:
//   {"t":<epoch>,"approx":<0|1>,"tbl":"<table>","row":<row-json>}
//
// Written while offline (or on any failed live POST) and drained in order by
// backfill.cpp when the uplink returns. `tbl` is one of the three telemetry
// tables; anything else is rejected on decode.
//
// This task (3.1) defines only the wire format. Tasks 3.2 / 3.4 / 3.5 add the
// SD append, offset-batch read, compaction and retention functions; those
// touch sdcard.h and are wrapped in #ifndef UNIT_TEST so this file's pure
// logic still builds in the native host-test env.
// =====================================================

struct JournalRec {
  time_t t;
  bool   approx;
  String tbl;
  String row;   // the exact JSON object the live POST would have sent
};

// Encode one line (no trailing newline).
String journalEncode(time_t t, bool approx, const char* tbl, const String& rowJson);

// Parse one line. Returns false on a JSON error, missing field, or an
// unrecognised `tbl` (also catches a torn/partial trailing line on replay).
bool journalDecode(const String& line, JournalRec& out);

// --- Pure helpers, exposed for host tests ---------------------------------

// Content of `all` after byte `offset`, snapped forward to the next line start
// (drops the consumed prefix). Empty if the offset is at/after the last line.
String journalDropPrefix(const String& all, size_t offset);

// Rewrite `all` keeping every activity_log + relay_metrics line and only the
// newest sensor_metrics lines that fit under `targetBytes`. Order preserved.
String journalEvictSensorMetrics(const String& all, size_t targetBytes);

// True if the raw journal line `p[0..len)` is a sensor_metrics record.
bool   journalLineIsSensorMetrics(const char* p, size_t len);

// --- Device-side (SD-backed); not built in the native host-test env -------
#ifndef UNIT_TEST

#define JOURNAL_PATH               "/buffer/pending.ndjson"
#define JOURNAL_OFFSET_PATH        "/buffer/offset"
#define JOURNAL_COMPACT_THRESHOLD  (512UL * 1024UL)          // compact once the offset passes this
#define JOURNAL_MAX_BYTES          (256UL * 1024UL * 1024UL) // hard retention cap

// Append one record, stamped with time(nullptr) + clockIsApprox(). false if no SD.
bool   journalAppend(const char* tbl, const String& rowJson);

size_t journalReadOffset();
void   journalWriteOffset(size_t off);
size_t journalPendingBytes();                                 // fileSize - offset

// Decode up to maxRecs complete lines from `fromOffset`. ends[i] = byte offset
// just past line i (what the caller commits after a successful POST). A torn
// trailing line is ignored. Returns the count decoded.
int    journalNextBatch(size_t fromOffset, JournalRec* recs, size_t* ends, int maxRecs);

void   journalCompact();            // drop [0, offset); reset offset to 0
void   journalEnforceRetention();   // evict oldest sensor_metrics if over the cap

#endif  // UNIT_TEST
