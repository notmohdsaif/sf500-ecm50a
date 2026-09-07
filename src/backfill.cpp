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
#include <WiFiClientSecure.h>

// Backfill gets its OWN TLS client, kept off the shared global `secureClient`.
// It POSTs in bursts (a whole batch per tick, every loop while the journal has
// content). Sharing `secureClient` with the periodic REST calls let one burst's
// socket churn wedge every later handshake on that client — the device then
// latched RS_OFFLINE_AUTONOMOUS with a healthy MQTT link and the journal never
// drained. A dedicated client, plus ONE reused TLS connection per batch, keeps
// that churn clear of the periodic calls.
static WiFiClientSecure bfClient;
static bool             bfClientReady = false;

static void ensureBfClient()
{
  if (bfClientReady) return;
  bfClient.setInsecure();
  bfClient.setHandshakeTimeout(5);
  bfClientReady = true;
}

static bool doseCritical()
{
  return autoState == AUTO_DOSING || autoState == AUTO_PRE_MIX || autoState == AUTO_POST_MIX;
}

// Outcome of draining one batch.
struct DrainResult
{
  int  advanced     = 0;   // rows to move the journal offset past (2xx + dropped 4xx)
  int  posted       = 0;   // rows that got a 2xx — proof the uplink is alive
  int  dropped      = 0;   // rows permanently rejected (4xx) and skipped
  bool transientStop = false;  // stopped on a 5xx / network error — retry next tick
};

// Classify one POST result and fold it into `r`. Returns false when the caller
// must stop the batch (a transient failure that should be retried, not skipped).
// A 4xx is permanent — the row will never be accepted (RLS, constraint,
// malformed) — so it is dropped and the batch continues, otherwise one bad row
// would wedge the whole journal forever. 408/429 are the exceptions: request
// timeout and rate-limit are transient, so back off and retry, never drop.
static bool classifyPost(int code, const JournalRec& rec, DrainResult& r)
{
  if (code >= 200 && code < 300) { r.advanced++; r.posted++;  return true; }

  if (code >= 400 && code < 500 && code != 408 && code != 429)
  {
    LOGF("[backfill] dropping %s row (%d): %s\n", rec.tbl.c_str(), code,
         rec.row.substring(0, 160).c_str());
    r.advanced++; r.dropped++;
    return true;
  }

  LOGF("[backfill] POST %s transient fail (%d) — will retry\n", rec.tbl.c_str(), code);
  r.transientStop = true;
  return false;
}

// Drain recs[0..n) over cellular — one request each (the modem layer owns its
// session).
static DrainResult drainCellular(const JournalRec* recs, int n)
{
  DrainResult r;
  for (int i = 0; i < n; i++)
  {
    String url = String(SUPABASE_URL) + "/rest/v1/" + recs[i].tbl;
    String resp;
    int code = cellularSupabaseRequest("POST", url, recs[i].row, "application/json",
                                       "return=minimal", resp);
    if (!classifyPost(code, recs[i], r)) break;
  }
  return r;
}

// Drain recs[0..n) over WiFi, reusing ONE TLS connection to Supabase for the
// whole batch, on backfill's dedicated client.
static DrainResult drainWifi(const JournalRec* recs, int n)
{
  ensureBfClient();

  HTTPClient http;
  http.setReuse(true);          // hold the socket up across the batch
  http.setTimeout(8000);

  DrainResult r;
  for (int i = 0; i < n; i++)
  {
    String url = String(SUPABASE_URL) + "/rest/v1/" + recs[i].tbl;
    if (!http.begin(bfClient, url))
    {
      LOGLN("[backfill] http.begin failed — will retry");
      r.transientStop = true;
      break;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    http.addHeader("Prefer", "return=minimal");
    int code = http.POST(recs[i].row);
    if (!classifyPost(code, recs[i], r)) break;
  }
  http.end();
  // Release the socket + mbedTLS context now. Backfill is the only user of
  // bfClient and it stops running once the journal is empty — without this the
  // reuse keep-alive would pin ~40 KB of heap indefinitely between ticks.
  bfClient.stop();
  return r;
}

void backfillTick()
{
  static unsigned long last = 0;

  if (!shouldTryUplink() || !sdMounted() || doseCritical()) return;
  if (millis() - last < BACKFILL_MIN_INTERVAL_MS) return;
  if (journalPendingBytes() == 0) return;
  last = millis();

  size_t off = journalReadOffset();
  JournalRec recs[BACKFILL_BATCH];
  size_t     ends[BACKFILL_BATCH];
  size_t     scannedTo = off;
  int n = journalNextBatch(off, recs, ends, BACKFILL_BATCH, &scannedTo);
  if (n == 0)
  {
    // Nothing decoded. If the scan got past one or more complete-but-corrupt
    // lines (should not happen — a torn trailing line is left alone), skip that
    // dead prefix so we don't re-read the same window every tick.
    if (scannedTo > off)
    {
      journalWriteOffset(scannedTo);
      LOGF("[backfill] skipped %u B of unparseable journal prefix\n",
           (unsigned)(scannedTo - off));
    }
    return;
  }

  DrainResult r = (activeTransport == TRANSPORT_CELLULAR)
                    ? drainCellular(recs, n)
                    : drainWifi(recs, n);

  if (r.advanced > 0)
  {
    journalWriteOffset(ends[r.advanced - 1]);
    // Any HTTP response (2xx or a 4xx drop) proves the uplink is alive.
    noteUplinkResult(true);
    LOGF("[backfill] %d/%d replayed, %d dropped, offset -> %u (pending %u B)\n",
         r.posted, n, r.dropped,
         (unsigned)ends[r.advanced - 1], (unsigned)journalPendingBytes());

    if (journalReadOffset() >= JOURNAL_COMPACT_THRESHOLD)
      journalCompact();
    journalEnforceRetention();
  }

  if (r.transientStop)
    noteUplinkResult(false);
}
