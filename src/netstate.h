#pragma once
#include <Arduino.h>

// =====================================================
// NETSTATE.H — the single run-state + connectivity signal
//
// Splits the "control plane" (dosing, sensors, schedules — must always run)
// from the "connectivity plane" (MQTT / Supabase — best effort). Every network
// caller checks haveUplink() and early-returns instantly when it is false,
// instead of blocking on a doomed TLS handshake.
//
// Post-rebase onto cellular-fallback: RunState composes with activeTransport
// (WiFi vs cellular); haveUplink() must also reflect the cellular data state.
// =====================================================

#define UPLINK_FAIL_DEBOUNCE 2      // consecutive REST failures before haveUplink() flips false
#define UPLINK_PROBE_INTERVAL_MS 60000UL  // while offline, let one gated REST call through this often

enum RunState {
  RS_BOOT,                // pre-config
  RS_ONLINE,             // config from cloud, uplink healthy
  RS_OFFLINE_AUTONOMOUS, // running on local (NVS/SD) config, no uplink
  RS_PROVISIONING        // captive portal open (may still be autonomous underneath)
};

extern RunState g_runState;

void setRunState(RunState s);        // logs the transition once

bool haveUplink();                   // debounced "backend reachable right now" (no side effect)
bool shouldTryUplink();             // gate a REST attempt: haveUplink() OR a probe is due.
                                    //   Returns true once per UPLINK_PROBE_INTERVAL_MS while
                                    //   offline so REST can recover even if the MQTT broker
                                    //   (the usual re-arm) stays down. Consumes the probe.
void noteUplinkResult(bool ok);      // call after every Supabase REST attempt
void noteMqttState(bool connected);  // call on MQTT connect / disconnect

bool configLoaded();                 // true once cloud OR local config populated the globals
void setConfigLoaded();
