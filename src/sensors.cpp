// =====================================================
// SENSORS.CPP
// Sensor init/scan, RS485/Modbus read, EC averaging, auto-dosing
// =====================================================

#include "sensors.h"
#include "logger.h"
#include "relay.h"    // writeRelay() used in checkAutoDosing
#include "cloud.h"    // logDeviceActivity()

// =====================================================
// SENSOR INITIALISATION
// =====================================================

void initSensors()
{
  LOGLN("\n--- Initializing Sensors ---");

  // EC Sensor detection: try default ID first, then fall back to range scan
  // Register address 0 attempted first; address 1 used as fallback (varies by sensor model)
  ecSensorFound = false;

  auto tryECId = [&](uint8_t id) -> bool {
    modbus.begin(id, Serial1);
    delay(50);
    bool ok = (modbus.readHoldingRegisters(0, 4) == modbus.ku8MBSuccess);
    if (!ok) { delay(30); ok = (modbus.readHoldingRegisters(1, 4) == modbus.ku8MBSuccess); }
    return ok;
  };

  if (tryECId(EC_SENSOR_DEFAULT))
  {
    ecSensorId    = EC_SENSOR_DEFAULT;
    ecSensorFound = true;
    LOGF("EC Sensor: ID %d (default)\n", EC_SENSOR_DEFAULT);
  }
  else
  {
    LOGF("EC Sensor: ID %d not responding — scanning IDs %d–%d\n",
         EC_SENSOR_DEFAULT, EC_SCAN_START, EC_SCAN_END);
    for (uint8_t id = EC_SCAN_START; id <= EC_SCAN_END; id++)
    {
      if (id == EC_SENSOR_DEFAULT) { delay(30); continue; } // already tried
      if (tryECId(id))
      {
        ecSensorId    = id;
        ecSensorFound = true;
        LOGF("EC Sensor: ID %d (scan)\n", id);
        break;
      }
      delay(30);
    }
  }
  if (!ecSensorFound)
    LOGF("EC Sensor: not found (tried ID %d + scan %d–%d)\n",
         EC_SENSOR_DEFAULT, EC_SCAN_START, EC_SCAN_END);

  // Scan for Water Level Sensor (IDs 13–15)
  wlSensorFound = false;
  for (uint8_t id = WL_SCAN_START; id <= WL_SCAN_END; id++)
  {
    modbus.begin(id, Serial1);
    delay(50);

    if (modbus.readHoldingRegisters(4, 1) == modbus.ku8MBSuccess)
    {
      uint16_t raw = modbus.getResponseBuffer(0);
      if (raw < 10000)
      {
        wlSensorId    = id;
        wlSensorFound = true;
        LOGF("Water Level: ID %d\n", id);
        break;
      }
    }
    delay(30);
  }
  if (!wlSensorFound)
    LOGF("Water Level: not found (scanned IDs %d–%d)\n", WL_SCAN_START, WL_SCAN_END);

  // Scan for Ambient sensor (IDs 20–22) — may be direct or via LoRa transparent bridge
  ambSensorFound = false;
  for (uint8_t id = AMB_SCAN_START; id <= AMB_SCAN_END; id++)
  {
    modbus.begin(id, Serial1);
    delay(50);
    if (modbus.readHoldingRegisters(AMB_REG_HUMID, 1) == modbus.ku8MBSuccess)
    {
      ambSensorId    = id;
      ambSensorFound = true;
      LOGF("Ambient: ID %d\n", id);
      break;
    }
    delay(30);
  }
  if (!ambSensorFound)
    LOGF("Ambient: not found (scanned IDs %d–%d)\n", AMB_SCAN_START, AMB_SCAN_END);

  // Scan for Rain sensor (ID 30) — via LoRa transparent bridge
  rainSensorFound = false;
  for (uint8_t id = RAIN_SCAN_START; id <= RAIN_SCAN_END; id++)
  {
    modbus.begin(id, Serial1);
    delay(50);
    if (modbus.readHoldingRegisters(RAIN_REG_TIPS, 1) == modbus.ku8MBSuccess)
    {
      rainSensorId    = id;
      rainSensorFound = true;
      LOGF("Rain: ID %d\n", id);
      break;
    }
    delay(30);
  }
  if (!rainSensorFound)
    LOGF("Rain: not found (scanned IDs %d–%d)\n", RAIN_SCAN_START, RAIN_SCAN_END);

  int found = (int)ecSensorFound + (int)wlSensorFound + (int)ambSensorFound + (int)rainSensorFound;
  LOGF("Found: %d/4 sensors\n", found);

  String msg = "Sensor init: ";
  if (ecSensorFound)   msg += "EC(ID="   + String(ecSensorId)   + ") ";
  if (wlSensorFound)   msg += "WL(ID="   + String(wlSensorId)   + ") ";
  if (ambSensorFound)  msg += "AMB(ID="  + String(ambSensorId)  + ") ";
  if (rainSensorFound) msg += "RAIN(ID=" + String(rainSensorId) + ")";
  msg.trim();
  if (!ecSensorFound && !wlSensorFound && !ambSensorFound && !rainSensorFound)
    msg += "none found";
  logDeviceActivity("system", msg.c_str());
}

// =====================================================
// SMART DOSING — NVS persist / restore
// =====================================================

void loadSmartCalibration()
{
  wifiPrefs.begin("smartdose", true);
  uint8_t cal = wifiPrefs.getUChar("calibrated", 0);
  if (cal)
  {
    ecRiseRate = wifiPrefs.getFloat("ec_rate", 0.0f);
    wlDropRate = wifiPrefs.getFloat("wl_rate", 0.0f);
    wlAtCal    = wifiPrefs.getFloat("wl_at_cal", 0.0f);

    // P5: reject physically implausible rates (valid range: 0.0003 – 0.02 mS/cm/s)
    if (ecRiseRate < 0.0003f || ecRiseRate > 0.02f)
    {
      LOGF("[Smart] ec_rate %.5f out of bounds — discarding calibration\n", ecRiseRate);
      ecRiseRate = 0.0f;
    }
    smartCalibrated = (ecRiseRate > 0.0f);
  }
  wifiPrefs.end();

  if (smartCalibrated)
  {
    LOGF("[Smart] Loaded calibration: ec_rate=%.5f wl_at_cal=%.0f\n", ecRiseRate, wlAtCal);
    // R1 guard: wlAtCal missing means first boot after firmware update — WL correction skipped
    if (wlAtCal <= 0.0f)
      LOGLN("[Smart] wl_at_cal not set — WL correction disabled until re-calibration");
  }
  else
    LOGLN("[Smart] No calibration stored — will calibrate on first smart dose");
}

void saveSmartCalibration()
{
  wifiPrefs.begin("smartdose", false);
  wifiPrefs.putUChar("calibrated", 1);
  wifiPrefs.putFloat("ec_rate", ecRiseRate);
  wifiPrefs.putFloat("wl_rate", wlDropRate);
  wifiPrefs.putFloat("wl_at_cal", wlBeforeCal);  // P10: persist WL at calibration time
  wifiPrefs.end();
}

// =====================================================
// RAIN — daily counter reset
// =====================================================

void loadRainResetState()
{
  wifiPrefs.begin("rain", true);
  lastRainResetDay = wifiPrefs.getInt("lastDay", -1);
  wifiPrefs.end();
  LOGF("[Rain] Last reset day loaded: %d\n", lastRainResetDay);
}

void checkRainDailyReset()
{
  if (!rainSensorFound) return;

  struct tm ti;
  if (!getLocalTime(&ti, 0)) return; // time not valid yet

  int today = ti.tm_mday;
  if (today == lastRainResetDay) return; // already reset today

  // Day changed (or never reset) — write magic value to reset the counter
  modbus.begin(rainSensorId, Serial1);
  delay(10);
  uint8_t result = modbus.writeSingleRegister(RAIN_REG_TIPS, 0x5A);
  if (result == modbus.ku8MBSuccess)
  {
    lastRainResetDay = today;
    wifiPrefs.begin("rain", false);
    wifiPrefs.putInt("lastDay", today);
    wifiPrefs.end();
    LOGF("[Rain] Daily reset OK (day %d)\n", today);
    logDeviceActivity("system", "Rain counter reset (daily)");
  }
  else
  {
    LOGF("[Rain] Daily reset failed (Modbus 0x%02X) — will retry next read\n", result);
  }
}

// =====================================================
// SENSOR READ
// =====================================================

void readSensors()
{
  bool success = false;

  // --- EC Sensor ---
  if (ecSensorFound)
  {
    modbus.begin(ecSensorId, Serial1);
    delay(10);

    if (modbus.readHoldingRegisters(0, 4) == modbus.ku8MBSuccess)
    {
      uint16_t r0 = modbus.getResponseBuffer(0);
      uint16_t r1 = modbus.getResponseBuffer(1);
      uint16_t r2 = modbus.getResponseBuffer(2);

      uint32_t ecRaw = ((uint32_t)r0 << 16) | r1;
      sensors.ec   = ecRaw / 100000.0f;
      sensors.temp = r2 / 10.0f;
      success = true;
    }
    delay(50);
  }

  // --- Water Level Sensor ---
  if (wlSensorFound)
  {
    modbus.begin(wlSensorId, Serial1);
    delay(10);

    if (modbus.readHoldingRegisters(4, 1) == modbus.ku8MBSuccess)
    {
      sensors.wl = modbus.getResponseBuffer(0);
      success    = true;
    }
    delay(50);
  }

  // --- Ambient Sensor (direct or via LoRa transparent bridge) ---
  if (ambSensorFound)
  {
    modbus.begin(ambSensorId, Serial1);
    delay(10);

    uint16_t rawHumid = 0, rawTemp = 0, rawLux = 0;
    bool ambOk = false;

    if (modbus.readHoldingRegisters(AMB_REG_HUMID, 2) == modbus.ku8MBSuccess)
    {
      rawHumid = modbus.getResponseBuffer(0);
      rawTemp  = modbus.getResponseBuffer(1);
      ambOk    = true;
    }
    delay(20);
    if (modbus.readHoldingRegisters(AMB_REG_LUX, 1) == modbus.ku8MBSuccess)
      rawLux = modbus.getResponseBuffer(0);

    if (ambOk)
    {
      sensors.ambHumid = rawHumid / 10.0f;
      sensors.ambTemp  = rawTemp  / 10.0f;
      sensors.ambLux   = (float)rawLux;
      success = true;
    }
    delay(50);
  }

  // --- Rain Sensor (via LoRa transparent bridge) ---
  if (rainSensorFound)
  {
    modbus.begin(rainSensorId, Serial1);
    delay(10);

    if (modbus.readHoldingRegisters(RAIN_REG_TIPS, 1) == modbus.ku8MBSuccess)
    {
      uint16_t rawTips = modbus.getResponseBuffer(0);
      sensors.rainfall = rawTips / 10.0f; // 0.1mm increments → mm
      success = true;
    }
    delay(50);

    checkRainDailyReset();
  }

  if (success)
    sensors.hasData = true;

  // --- Publish via MQTT (always publish if connected; sensor fields only when available) ---
  if (mqttClient.connected())
  {
    StaticJsonDocument<896> doc;

    if (success)
    {
      if (ecSensorFound)
      {
        doc["ec"]   = sensors.ec;
        doc["temp"] = sensors.temp;
      }
      if (wlSensorFound)
        doc["wl"] = sensors.wl;
      if (ambSensorFound)
      {
        JsonObject ambObj  = doc.createNestedObject("amb");
        ambObj["temp"]     = serialized(String(sensors.ambTemp,  1));
        ambObj["humid"]    = serialized(String(sensors.ambHumid, 1));
        ambObj["lux"]      = serialized(String(sensors.ambLux,   0));
      }
      if (rainSensorFound)
        doc["rain"] = serialized(String(sensors.rainfall, 1));
    }

    // Include auto-dosing state when active or in alarm
    if (autoDosing || autoState == AUTO_ALARM)
    {
      const char* stateNames[] = {
        "idle","startup_wait","sampling","pre_mix",
        "dosing","post_mix","cooldown","stabilising","alarm"
      };
      JsonObject autoObj = doc.createNestedObject("auto");
      autoObj["state"]       = stateNames[autoState];
      autoObj["target"]      = serialized(String(ecTarget, 2));
      autoObj["samples"]     = ecReadingCount;
      autoObj["avg"]         = serialized(String(ecAverage, 3));
      autoObj["doses_today"] = dosesToday;

      if (lastDoseTimestamp > 0)
      {
        struct tm ti;
        localtime_r(&lastDoseTimestamp, &ti);
        char ts[30];
        sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
        autoObj["last_dose"] = ts;
      }

      if (smartDosing)
      {
        JsonObject smartObj       = autoObj.createNestedObject("smart");
        smartObj["calibrated"]    = smartCalibrated;
        smartObj["calibrating"]   = smartCalPhase;
        if (smartCalibrated)
          smartObj["ec_rate"]     = serialized(String(ecRiseRate, 5));
        if (computedDoseTime > 0)
          smartObj["computed"]    = computedDoseTime;
      }
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      JsonObject wifiObj = doc.createNestedObject("wifi");
      wifiObj["ssid"] = WiFi.SSID();
      wifiObj["rssi"] = WiFi.RSSI();
      wifiObj["ip"]   = WiFi.localIP().toString();
    }

    doc["fw"] = FIRMWARE_VERSION;

    JsonObject sensorsObj = doc.createNestedObject("sensors");
    sensorsObj["ec"]   = ecSensorFound;
    sensorsObj["wl"]   = wlSensorFound;
    sensorsObj["amb"]  = ambSensorFound;
    sensorsObj["rain"] = rainSensorFound;

    char buf[896];
    serializeJson(doc, buf);
    mqttClient.publish(mqttTopicData.c_str(), buf);
  }

  // Only add to rolling average when R1 is NOT dosing and not in stabilising skip window
  if (success && ecSensorFound && !relayStates[0])
  {
    if (autoState == AUTO_STABILISING)
      tickStabiliseSkip(); // count skipped readings, don't add to average
    else
      updateECAverage(sensors.ec);
  }

  // Debug output when auto-dosing is active
  if (autoDosing && ecSensorFound)
  {
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug >= 10000)
    {
      const char* stateNames[] = {
        "idle","startup_wait","sampling","pre_mix",
        "dosing","post_mix","cooldown","stabilising","alarm"
      };
      LOGF("[Auto] state:%s EC:%.2f Avg:%.2f samples:%d/%d\n",
           stateNames[autoState], sensors.ec, ecAverage, ecReadingCount, EC_SAMPLES);
      lastDebug = millis();
    }
  }
}

// =====================================================
// EC ROLLING AVERAGE
// =====================================================

void updateECAverage(float reading)
{
  ecReadings[ecReadingIndex] = reading;
  ecReadingIndex = (ecReadingIndex + 1) % EC_SAMPLES;

  if (ecReadingCount < EC_SAMPLES)
    ecReadingCount++;

  float sum = 0;
  for (int i = 0; i < ecReadingCount; i++)
    sum += ecReadings[i];
  ecAverage = sum / ecReadingCount;
}

// =====================================================
// AUTO-DOSING STATE MACHINE
// =====================================================

// Ceiling-hold tracking — module scope so they can be reset from any SAMPLING sub-path
static unsigned long ceilingHoldStart    = 0;    // millis() when ceiling hold began (0 = not holding)
static unsigned long lastCeilingLogMs    = 0;    // millis() of last ceiling-hold log entry
static float         lastLoggedCeilingEc = -1.0f; // ecAverage at last ceiling-hold log entry

static void enterState(AutoDosingState next)
{
  autoState          = next;
  autoStateEnteredAt = millis();
}

static void triggerAlarm(const String& reason)
{
  LOGLNS("\n[Auto] ALARM: " + reason);
  // Do NOT set autoDosing = false here — the config fetch would re-enable it automatically.
  // Instead, block all activity via AUTO_ALARM state; user must toggle off→on to reset.
  writeRelay(1, false);
  writeRelay(2, false);
  enterState(AUTO_ALARM);
  String msg = "Auto-dosing alarm: " + reason;
  logDeviceActivity("alarm", msg.c_str());
}

void checkAutoDosing()
{
  unsigned long now = millis();

  // Sync state to IDLE when auto-dosing is disabled (including clearing an active alarm)
  if (!autoDosing)
  {
    if (autoState != AUTO_IDLE)
      enterState(AUTO_IDLE);
    return;
  }

  if (!ecSensorFound)
    return;

  switch (autoState)
  {
    // -------------------------------------------------
    case AUTO_IDLE:
      ecReadingCount = 0;
      ecReadingIndex = 0;
      consecutiveIneffectiveDoses = 0;
      stabiliseSkipCount = 0;
      calRetryCount = 0;
      // Reset ceiling-hold timer — ec_target or autoDosing may have changed,
      // so the new cycle gets a fresh 30-min window from scratch.
      ceilingHoldStart    = 0;
      lastCeilingLogMs    = 0;
      lastLoggedCeilingEc = -1.0f;
      enterState(AUTO_STARTUP_WAIT);
      LOGF("[Auto] Starting — waiting %lus\n", INITIAL_WAIT / 1000);
      break;

    // -------------------------------------------------
    case AUTO_STARTUP_WAIT:
      if (now - autoStateEnteredAt >= INITIAL_WAIT)
      {
        ecReadingCount = 0;
        ecReadingIndex = 0;
        enterState(AUTO_SAMPLING);
        LOGLN("[Auto] Startup wait done — sampling");
      }
      break;

    // -------------------------------------------------
    case AUTO_SAMPLING:
    {
      // EC ceiling check — hold dosing; escalate to ALARM after sustained hold
      if (ecReadingCount >= EC_SAMPLES && ecAverage > ecTarget + EC_CEILING_MARGIN)
      {
        if (ceilingHoldStart == 0)
          ceilingHoldStart = millis();

        // Time-based log throttle: only log when EC changes AND enough time has passed
        unsigned long nowMs = millis();
        if (fabsf(ecAverage - lastLoggedCeilingEc) > 0.05f &&
            nowMs - lastCeilingLogMs >= EC_CEILING_LOG_INTERVAL)
        {
          LOGF("[Auto] EC above ceiling (%.2f > %.2f) — holding (%lu s)\n",
               ecAverage, ecTarget + EC_CEILING_MARGIN,
               (nowMs - ceilingHoldStart) / 1000UL);
          logDeviceActivity("dosing", ("Auto-dosing holding: EC above ceiling (" +
                            String(ecAverage, 2) + " > " +
                            String(ecTarget + EC_CEILING_MARGIN, 2) + ")").c_str());
          lastLoggedCeilingEc = ecAverage;
          lastCeilingLogMs    = nowMs;
        }

        // Escalate to ALARM if EC has been above ceiling continuously for too long
        if (nowMs - ceilingHoldStart >= EC_CEILING_HOLD_TIMEOUT)
        {
          triggerAlarm("EC above ceiling for " +
                       String(EC_CEILING_HOLD_TIMEOUT / 60000UL) + "+ min (" +
                       String(ecAverage, 2) + " > " +
                       String(ecTarget + EC_CEILING_MARGIN, 2) + ")");
          ceilingHoldStart    = 0;
          lastCeilingLogMs    = 0;
          lastLoggedCeilingEc = -1.0f;
          return;
        }

        return;
      }

      // EC is back within ceiling — reset hold timer so next episode starts fresh
      ceilingHoldStart    = 0;
      lastCeilingLogMs    = 0;
      lastLoggedCeilingEc = -1.0f;

      // Wait for full sample window
      if (ecReadingCount < EC_SAMPLES)
        return;

      // EC is acceptable — keep monitoring
      if (ecAverage >= ecMinusHys)
        return;

      // Don't interrupt a manually-running relay timer
      if (relayDurations[0] > 0 || relayTimers[0] > 0)
      {
        LOGLN("[Auto] Skipping — R1 timer already active");
        return;
      }

      // Water level minimum check — soft block, no alarm
      if (wlSensorFound && minWlDosing > 0 && (unsigned int)sensors.wl < minWlDosing)
      {
        LOGF("[Auto] Skipping — water level too low (%dmm < %dmm min)\n",
             (int)sensors.wl, minWlDosing);
        return;
      }

      // EC below threshold — prepare to dose
      preDoseEC = ecAverage;

      // Determine dose duration
      unsigned int thisDoseTime = dosingTime;
      if (smartDosing && !smartCalibrated)
      {
        // P7: scale calibration dose with dosingTime so large tanks get enough coverage
        actualCalDuration = max((unsigned int)SMART_CAL_DURATION, dosingTime);
        smartCalPhase     = true;
        wlBeforeCal       = sensors.wl;
        thisDoseTime      = actualCalDuration;
        LOGF("[Smart] Calibration dose: %ds (WL=%.0f)\n", actualCalDuration, wlBeforeCal);
      }
      else if (smartDosing && smartCalibrated && ecRiseRate > 0.0f)
      {
        float deficit = ecTarget - ecAverage;
        float computed = deficit / ecRiseRate;
        thisDoseTime  = (unsigned int)constrain((int)roundf(computed), SMART_MIN_DOSE, SMART_MAX_DOSE);

        // P1: WL correction — same dose is more potent at lower WL (dilution principle)
        // ΔEC ∝ 1/WL, so correct by (WL_now / WL_cal). R1 guard: skip if wlAtCal not set.
        if (wlSensorFound && wlAtCal > 0.0f && sensors.wl > 0.0f)
        {
          float wlFactor = constrain(sensors.wl / wlAtCal, 0.0f, SMART_WL_FACTOR_MAX);
          thisDoseTime   = (unsigned int)constrain((int)roundf(thisDoseTime * wlFactor),
                                                   SMART_MIN_DOSE, SMART_MAX_DOSE);
          LOGF("[Smart] WL correction: factor=%.3f (wl=%.0f cal_wl=%.0f) -> %ds\n",
               wlFactor, sensors.wl, wlAtCal, thisDoseTime);
        }

        computedDoseTime = thisDoseTime;
        LOGF("[Smart] Computed dose: %ds (deficit=%.3f rate=%.5f)\n",
             thisDoseTime, deficit, ecRiseRate);
      }

      LOGF("\n[Auto] EC low: avg=%.3f < %.2f | Path %s | dose=%ds\n",
           ecAverage, ecMinusHys, autoMixing ? "B (mix)" : "A (no mix)", thisDoseTime);

      ecReadingCount = 0;
      ecReadingIndex = 0;

      activeDoseTime = thisDoseTime;

      if (autoMixing)
      {
        writeRelay(2, true);
        enterState(AUTO_PRE_MIX);
        LOGF("[Auto] PRE_MIX: R2 ON for %lus\n", PRE_MIX_DURATION / 1000);
      }
      else
      {
        relayDurations[0] = thisDoseTime;
        relayTimers[0]    = now;
        writeRelay(1, true);
        enterState(AUTO_DOSING);
        LOGF("[Auto] DOSING: R1 ON for %ds\n", thisDoseTime);
      }
      break;
    }

    // -------------------------------------------------
    case AUTO_PRE_MIX:  // Path B only
      if (now - autoStateEnteredAt >= PRE_MIX_DURATION)
      {
        // Use computedDoseTime if smart dosing, otherwise fall back to dosingTime
        unsigned int thisDoseTime = (smartDosing && computedDoseTime > 0) ? computedDoseTime
                                  : (smartDosing && smartCalPhase)        ? actualCalDuration
                                  : dosingTime;
        activeDoseTime    = thisDoseTime;
        relayDurations[0] = thisDoseTime;
        relayTimers[0]    = now;
        relayDurations[1] = thisDoseTime + POST_MIX_DURATION / 1000;
        relayTimers[1]    = now;
        writeRelay(1, true);
        enterState(AUTO_DOSING);
        LOGF("[Auto] DOSING: R1+R2 ON for %ds\n", thisDoseTime);
      }
      break;

    // -------------------------------------------------
    case AUTO_DOSING:
      // P0: abort dose early if EC already exceeded target + margin
      // Guards against stale calibration overshooting before time elapses.
      // In Path B, both relays are stopped so R2 is not left running uncontrolled (R3).
      if (ecSensorFound && sensors.ec > ecTarget + DOSE_ABORT_MARGIN)
      {
        writeRelay(1, false);
        relayDurations[0] = 0;
        relayTimers[0]    = 0;
        if (autoMixing)
        {
          writeRelay(2, false);
          relayDurations[1] = 0;
          relayTimers[1]    = 0;
        }
        actualDoseElapsed = (unsigned int)((now - autoStateEnteredAt) / 1000UL);
        lastDoseAborted   = true;
        doseEndTime       = now;
        lastDoseTimestamp = time(nullptr);
        {
          struct tm ti;
          getLocalTime(&ti);
          if (ti.tm_mday != lastDoseDay) { dosesToday = 0; lastDoseDay = ti.tm_mday; }
        }
        dosesToday++;
        stabiliseSkipCount = 0;
        LOGF("[Auto] Dose aborted: EC %.2f exceeded target+margin (%.2f)\n",
             sensors.ec, ecTarget + DOSE_ABORT_MARGIN);
        {
          String wlPart = wlSensorFound ? (", WL " + String((int)sensors.wl) + "mm") : "";
          logDeviceActivity("dosing", ("Dose aborted: " + String(actualDoseElapsed) + "s"
                            + " | EC " + String(sensors.ec, 2) + " > " + String(ecTarget + DOSE_ABORT_MARGIN, 2) + " ceiling"
                            + wlPart
                            + " (#" + String(dosesToday) + ")").c_str());
        }
        enterState(AUTO_COOLDOWN);
        break;
      }

      if (now - autoStateEnteredAt >= (unsigned long)activeDoseTime * 1000UL)
      {
        writeRelay(1, false);
        relayDurations[0] = 0;
        relayTimers[0]    = 0;
        doseEndTime       = now;
        lastDoseTimestamp = time(nullptr);
        {
          struct tm ti;
          getLocalTime(&ti);
          if (ti.tm_mday != lastDoseDay) {
            dosesToday  = 0;
            lastDoseDay = ti.tm_mday;
          }
        }
        dosesToday++;
        actualDoseElapsed = activeDoseTime;
        lastDoseAborted   = false;

        LOGF("[Auto] Dose complete. doses_today=%d\n", dosesToday);
        {
          String wlPart  = wlSensorFound ? (", WL " + String((int)sensors.wl) + "mm") : "";
          String doseNum = " (#" + String(dosesToday) + ")";
          String doseMsg;
          if (smartDosing && smartCalPhase)
          {
            doseMsg = "Smart-dose calibration: " + String(activeDoseTime) + "s"
                    + " | EC " + String(preDoseEC, 2) + " mS/cm"
                    + wlPart + doseNum;
          }
          else if (smartDosing && smartCalibrated && computedDoseTime > 0)
          {
            doseMsg = "Smart-dose executed: " + String(activeDoseTime) + "s"
                    + " | EC " + String(preDoseEC, 2) + " -> " + String(ecTarget, 1) + " target"
                    + wlPart + doseNum;
          }
          else
          {
            doseMsg = "Auto-dose: " + String(activeDoseTime) + "s"
                    + " | EC " + String(preDoseEC, 2) + " -> " + String(ecTarget, 1) + " target"
                    + wlPart + doseNum;
          }
          logDeviceActivity("dosing", doseMsg.c_str());
        }

        if (autoMixing)
        {
          enterState(AUTO_POST_MIX);
          LOGF("[Auto] POST_MIX: R2 ON for %lus\n", POST_MIX_DURATION / 1000);
        }
        else
        {
          stabiliseSkipCount = 0;
          enterState(AUTO_COOLDOWN);
          LOGF("[Auto] COOLDOWN (no mix): %lus\n", POST_DOSE_DELAY_NO_MIX / 1000);
        }
      }
      break;

    // -------------------------------------------------
    case AUTO_POST_MIX:  // Path B only
      if (now - autoStateEnteredAt >= POST_MIX_DURATION)
      {
        writeRelay(2, false);
        relayDurations[1] = 0;
        relayTimers[1]    = 0;
        stabiliseSkipCount = 0;
        enterState(AUTO_COOLDOWN);
        LOGF("[Auto] COOLDOWN (mix): %lus\n", POST_DOSE_DELAY_MIX / 1000);
      }
      break;

    // -------------------------------------------------
    case AUTO_COOLDOWN:
    {
      unsigned long delay = autoMixing ? POST_DOSE_DELAY_MIX : POST_DOSE_DELAY_NO_MIX;
      if (now - doseEndTime >= delay)
      {
        enterState(AUTO_STABILISING);
        LOGF("[Auto] STABILISING: skipping %d readings\n",
             autoMixing ? STABILISE_SKIP_MIX : STABILISE_SKIP_NO_MIX);
      }
      break;
    }

    // -------------------------------------------------
    case AUTO_STABILISING:
    {
      int skipTarget = autoMixing ? STABILISE_SKIP_MIX : STABILISE_SKIP_NO_MIX;
      if (stabiliseSkipCount < skipTarget)
        return; // readings are being skipped in readSensors via the guard

      // Check dose response
      if (ecReadingCount >= EC_SAMPLES)
      {
        float ecRise = ecAverage - preDoseEC;
        LOGF("[Auto] Response check: pre=%.3f now=%.3f rise=%.3f\n",
             preDoseEC, ecAverage, ecRise);

        // P8: save before the block clears it — needed to guard ineffective dose counter below
        bool wasCalPhase = smartCalPhase;

        // Smart dosing calibration / accuracy check
        if (smartCalPhase)
        {
          if (ecRise >= DOSE_RESPONSE_THRESHOLD)
          {
            // P9: use actual calibration dose duration, not the compile-time constant
            ecRiseRate = ecRise / (float)actualCalDuration;
            if (wlSensorFound)
            {
              float wlDrop = wlBeforeCal - sensors.wl;
              if (wlDrop > 0.0f) wlDropRate = wlDrop / (float)actualCalDuration;
            }
            wlAtCal         = wlBeforeCal;  // P10: update in-memory value immediately
            saveSmartCalibration();
            smartCalibrated = true;
            calRetryCount   = 0;
            LOGF("[Smart] Calibrated: ec_rate=%.5f wl_at_cal=%.0f\n", ecRiseRate, wlAtCal);
            char calMsg[96];
            snprintf(calMsg, sizeof(calMsg),
                     "Smart dosing calibrated: ec_rate=%.5f rise=%.3f mS/cm",
                     ecRiseRate, ecRise);
            logDeviceActivity("dosing", calMsg);
          }
          else
          {
            // P8: track calibration retries separately — high WL produces low rise, not a pump fault
            calRetryCount++;
            LOGF("[Smart] Calibration had no EC rise — retry %d/%d\n",
                 calRetryCount, SMART_CAL_MAX_RETRIES);
            if (calRetryCount >= SMART_CAL_MAX_RETRIES)
            {
              triggerAlarm("Smart calibration failed after " + String(calRetryCount) + " attempts");
              return;
            }
          }
          smartCalPhase = false;
        }
        else if (!lastDoseAborted && smartDosing && smartCalibrated && computedDoseTime > 0)
        {
          // Skip accuracy check on aborted doses — actual dose time < planned time guarantees
          // false error, which would invalidate good calibration data.
          float predicted = ecRiseRate * (float)computedDoseTime;
          if (predicted > 0.0f)
          {
            float error = fabsf(ecRise - predicted) / predicted;
            if (error > SMART_ERROR_THRESHOLD)
            {
              LOGF("[Smart] Prediction error %.0f%% — re-calibrating next cycle\n",
                   error * 100.0f);
              smartCalibrated  = false;
              computedDoseTime = 0;
            }
          }
        }
        lastDoseAborted = false;

        // P8: calibration dose failures are not pump failures — use separate counter above
        if (ecRise < DOSE_RESPONSE_THRESHOLD)
        {
          if (!wasCalPhase)
          {
            consecutiveIneffectiveDoses++;
            LOGF("[Auto] Ineffective dose #%d\n", consecutiveIneffectiveDoses);
            if (consecutiveIneffectiveDoses >= MAX_INEFFECTIVE_DOSES)
            {
              triggerAlarm("No EC response after " + String(MAX_INEFFECTIVE_DOSES) + " doses");
              return;
            }
          }
        }
        else
        {
          consecutiveIneffectiveDoses = 0;  // any successful dose (cal or production) resets counter
        }

        enterState(AUTO_SAMPLING);
        LOGLN("[Auto] Back to SAMPLING");
      }
      break;
    }

    // -------------------------------------------------
    case AUTO_ALARM:
      // Stays in ALARM until autoDosing is re-enabled from dashboard
      break;
  }
}

// Called from readSensors() to advance stabilise skip counter
void tickStabiliseSkip()
{
  if (autoState == AUTO_STABILISING)
    stabiliseSkipCount++;
}
