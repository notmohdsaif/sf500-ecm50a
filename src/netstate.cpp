// =====================================================
// NETSTATE.CPP
// =====================================================

#include "netstate.h"
#include "logger.h"

RunState g_runState = RS_BOOT;

// Optimistic at boot: the first REST call is allowed through, and a genuine
// blackout debounces it false after UPLINK_FAIL_DEBOUNCE failures. Starting
// false would make every gated call skip before the first success and the
// device could never come online.
static bool     uplinkOk    = true;
static uint8_t  failStreak  = 0;
static bool     cfgLoaded   = false;
static uint32_t lastProbeAt = 0;   // millis() of the last offline REST probe / failure

static const char* rsName(RunState s)
{
  switch (s)
  {
    case RS_BOOT:               return "BOOT";
    case RS_ONLINE:             return "ONLINE";
    case RS_OFFLINE_AUTONOMOUS: return "OFFLINE_AUTONOMOUS";
    case RS_PROVISIONING:       return "PROVISIONING";
  }
  return "?";
}

void setRunState(RunState s)
{
  if (s == g_runState) return;
  LOGF("[RUN] %s -> %s\n", rsName(g_runState), rsName(s));
  g_runState = s;
}

bool haveUplink() { return uplinkOk; }

bool shouldTryUplink()
{
  if (uplinkOk) return true;
  // Debounced offline. Let one call through per probe interval so a REST
  // recovery does not depend on the MQTT broker coming back (the only other
  // path that re-arms uplinkOk).
  uint32_t now = millis();
  if (now - lastProbeAt >= UPLINK_PROBE_INTERVAL_MS)
  {
    lastProbeAt = now;
    return true;
  }
  return false;
}

void noteUplinkResult(bool ok)
{
  if (ok)
  {
    if (!uplinkOk) LOGLN("[net] uplink UP (REST ok)");
    uplinkOk   = true;
    failStreak = 0;
    return;
  }
  lastProbeAt = millis();   // restart the probe interval from this failure
  if (failStreak < 255) failStreak++;
  if (failStreak == UPLINK_FAIL_DEBOUNCE && uplinkOk)
    LOGF("[net] uplink DOWN (%u consecutive REST failures)\n", (unsigned)failStreak);
  if (failStreak >= UPLINK_FAIL_DEBOUNCE) uplinkOk = false;
}

void noteMqttState(bool connected)
{
  // A live broker connection is proof the uplink works. Called every loop (not
  // just on the connect edge) so REST can recover the moment the broker is up
  // again without waiting for a stale offline probe. A disconnect is advisory
  // only — a Supabase REST result is the authoritative "offline" signal (the
  // broker can drop us mid-loop while WAN is still fine).
  if (connected)
  {
    if (!uplinkOk) LOGLN("[net] uplink UP (MQTT connected)");
    uplinkOk   = true;
    failStreak = 0;
  }
}

bool configLoaded()    { return cfgLoaded; }
void setConfigLoaded() { cfgLoaded = true; }
