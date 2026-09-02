// =====================================================
// BACKFILL.CPP — replay the SD journal to Supabase on uplink recovery
// =====================================================

#include "backfill.h"
#include "journal.h"
#include "netstate.h"
#include "sdcard.h"
#include "globals.h"
#include "cellular.h"   // cellularSupabaseRequest()
#include "logger.h"
#include <HTTPClient.h>

static bool doseCritical()
{
  return autoState == AUTO_DOSING || autoState == AUTO_PRE_MIX || autoState == AUTO_POST_MIX;
}

// One row to /rest/v1/<tbl>, over whichever transport is live. Returns true on 2xx.
static bool postRow(const String& tbl, const String& rowJson)
{
  String url = String(SUPABASE_URL) + "/rest/v1/" + tbl;

  if (activeTransport == TRANSPORT_CELLULAR)
  {
    String resp;
    int code = cellularSupabaseRequest("POST", url, rowJson, "application/json",
                                       "return=minimal", resp);
    return code >= 200 && code < 300;
  }

  HTTPClient http;
  if (!http.begin(secureClient, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(4000);
  int code = http.POST(rowJson);
  http.end();
  return code >= 200 && code < 300;
}

void backfillTick()
{
  static unsigned long last = 0;

  if (!haveUplink() || !sdMounted() || doseCritical()) return;
  if (millis() - last < BACKFILL_MIN_INTERVAL_MS) return;
  if (journalPendingBytes() == 0) return;
  last = millis();

  size_t off = journalReadOffset();
  JournalRec recs[BACKFILL_BATCH];
  size_t     ends[BACKFILL_BATCH];
  int n = journalNextBatch(off, recs, ends, BACKFILL_BATCH);
  if (n == 0)
  {
    // Only an unparseable prefix left (should not happen) — skip past it so we
    // don't spin. journalNextBatch already ignores a torn trailing line.
    return;
  }

  int committed = 0;
  for (int i = 0; i < n; i++)
  {
    if (!postRow(recs[i].tbl, recs[i].row))
    {
      noteUplinkResult(false);
      break;
    }
    committed = i + 1;
  }

  if (committed > 0)
  {
    journalWriteOffset(ends[committed - 1]);
    noteUplinkResult(true);
    LOGF("[backfill] %d row(s) replayed, offset -> %u (pending %u B)\n",
         committed, (unsigned)ends[committed - 1], (unsigned)journalPendingBytes());

    if (journalReadOffset() >= JOURNAL_COMPACT_THRESHOLD)
      journalCompact();
    journalEnforceRetention();
  }
}
