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
static bool    uplinkOk   = true;
static uint8_t failStreak = 0;
static bool    cfgLoaded  = false;

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

void noteUplinkResult(bool ok)
{
  if (ok)
  {
    uplinkOk   = true;
    failStreak = 0;
    return;
  }
  if (failStreak < 255) failStreak++;
  if (failStreak >= UPLINK_FAIL_DEBOUNCE) uplinkOk = false;
}

void noteMqttState(bool connected)
{
  // A live broker connection is proof the uplink works. A disconnect is
  // advisory only — a Supabase REST result is the authoritative "offline"
  // signal (the broker can drop us mid-loop while WAN is still fine).
  if (connected)
  {
    uplinkOk   = true;
    failStreak = 0;
  }
}

bool configLoaded()    { return cfgLoaded; }
void setConfigLoaded() { cfgLoaded = true; }
