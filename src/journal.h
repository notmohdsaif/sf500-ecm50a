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
