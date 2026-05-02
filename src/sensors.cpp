// =====================================================
// SENSORS.CPP
// Sensor init/scan, RS485/Modbus read, EC averaging, auto-dosing
// =====================================================

#include "sensors.h"
#include "relay.h"    // writeRelay() used in checkAutoDosing
#include "cloud.h"    // logDeviceActivity()

// =====================================================
// SENSOR INITIALISATION
// =====================================================

void initSensors()
{
  Serial.println("\n--- Initializing Sensors ---");

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
    Serial.println("EC Sensor: ID " + String(EC_SENSOR_DEFAULT) + " (default)");
  }
  else
  {
    Serial.println("EC Sensor: ID " + String(EC_SENSOR_DEFAULT) + " not responding — scanning IDs " +
                   String(EC_SCAN_START) + "–" + String(EC_SCAN_END));
    for (uint8_t id = EC_SCAN_START; id <= EC_SCAN_END; id++)
    {
      if (id == EC_SENSOR_DEFAULT) { delay(30); continue; } // already tried
      if (tryECId(id))
      {
        ecSensorId    = id;
        ecSensorFound = true;
        Serial.println("EC Sensor: ID " + String(id) + " (scan)");
        break;
      }
      delay(30);
    }
  }
  if (!ecSensorFound)
    Serial.println("EC Sensor: not found (tried ID " + String(EC_SENSOR_DEFAULT) +
                   " + scan " + String(EC_SCAN_START) + "–" + String(EC_SCAN_END) + ")");

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
        Serial.println("Water Level: ID " + String(id));
        break;
      }
    }
    delay(30);
  }
  if (!wlSensorFound)
    Serial.println("Water Level: not found (scanned IDs " + String(WL_SCAN_START) + "–" + String(WL_SCAN_END) + ")");

  int found = (int)ecSensorFound + (int)wlSensorFound;
  Serial.println("Found: " + String(found) + "/2 sensors\n");

  String msg = "Sensor init: ";
  if (ecSensorFound) msg += "EC(ID=" + String(ecSensorId) + ") ";
  if (wlSensorFound) msg += "WL(ID=" + String(wlSensorId) + ")";
  if (!ecSensorFound && !wlSensorFound) msg += "none found";
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
    ecRiseRate      = wifiPrefs.getFloat("ec_rate", 0.0f);
    wlDropRate      = wifiPrefs.getFloat("wl_rate", 0.0f);
    smartCalibrated = (ecRiseRate > 0.0f);
  }
  wifiPrefs.end();

  if (smartCalibrated)
    Serial.println("[Smart] Loaded calibration: ec_rate=" + String(ecRiseRate, 5) + " mS/cm/s");
  else
    Serial.println("[Smart] No calibration stored — will calibrate on first smart dose");
}

void saveSmartCalibration()
{
  wifiPrefs.begin("smartdose", false);
  wifiPrefs.putUChar("calibrated", 1);
  wifiPrefs.putFloat("ec_rate", ecRiseRate);
  wifiPrefs.putFloat("wl_rate", wlDropRate);
  wifiPrefs.end();
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

  if (success)
    sensors.hasData = true;

  // --- Publish via MQTT (always publish if connected; sensor fields only when available) ---
  if (mqttClient.connected())
  {
    StaticJsonDocument<768> doc;

    if (success)
    {
      if (ecSensorFound)
      {
        doc["ec"]   = sensors.ec;
        doc["temp"] = sensors.temp;
      }
      if (wlSensorFound)
        doc["wl"] = sensors.wl;
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
    sensorsObj["ec"] = ecSensorFound;
    sensorsObj["wl"] = wlSensorFound;

    char buf[768];
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
      Serial.print("[Auto] state:" + String(stateNames[autoState]));
      Serial.print(" EC:" + String(sensors.ec, 2));
      if (ecReadingCount > 0)
        Serial.print(" Avg:" + String(ecAverage, 2));
      Serial.println(" samples:" + String(ecReadingCount) + "/" + String(EC_SAMPLES));
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

static void enterState(AutoDosingState next)
{
  autoState          = next;
  autoStateEnteredAt = millis();
}

static void triggerAlarm(const String& reason)
{
  Serial.println("\n[Auto] ALARM: " + reason);
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
      enterState(AUTO_STARTUP_WAIT);
      Serial.println("[Auto] Starting — waiting " + String(INITIAL_WAIT / 1000) + "s");
      break;

    // -------------------------------------------------
    case AUTO_STARTUP_WAIT:
      if (now - autoStateEnteredAt >= INITIAL_WAIT)
      {
        ecReadingCount = 0;
        ecReadingIndex = 0;
        enterState(AUTO_SAMPLING);
        Serial.println("[Auto] Startup wait done — sampling");
      }
      break;

    // -------------------------------------------------
    case AUTO_SAMPLING:
    {
      // EC ceiling check
      if (ecReadingCount >= EC_SAMPLES && ecAverage > ecTarget + EC_CEILING_MARGIN)
      {
        triggerAlarm("EC above ceiling (" + String(ecAverage, 2) +
                     " > " + String(ecTarget + EC_CEILING_MARGIN, 2) + ")");
        return;
      }

      // Wait for full sample window
      if (ecReadingCount < EC_SAMPLES)
        return;

      // EC is acceptable — keep monitoring
      if (ecAverage >= ecMinusHys)
        return;

      // Don't interrupt a manually-running relay timer
      if (relayDurations[0] > 0 || relayTimers[0] > 0)
      {
        Serial.println("[Auto] Skipping — R1 timer already active");
        return;
      }

      // EC below threshold — prepare to dose
      preDoseEC = ecAverage;

      // Determine dose duration
      unsigned int thisDoseTime = dosingTime;
      if (smartDosing && !smartCalibrated)
      {
        smartCalPhase = true;
        wlBeforeCal   = sensors.wl;
        thisDoseTime  = SMART_CAL_DURATION;
        Serial.println("[Smart] Calibration dose: " + String(SMART_CAL_DURATION) + "s");
      }
      else if (smartDosing && smartCalibrated && ecRiseRate > 0.0f)
      {
        float deficit    = ecTarget - ecAverage;
        float computed   = deficit / ecRiseRate;
        thisDoseTime     = (unsigned int)constrain((int)roundf(computed), SMART_MIN_DOSE, SMART_MAX_DOSE);
        computedDoseTime = thisDoseTime;
        Serial.println("[Smart] Computed dose: " + String(thisDoseTime) + "s"
                       " (deficit=" + String(deficit, 3) +
                       " rate=" + String(ecRiseRate, 5) + ")");
      }

      Serial.println("\n[Auto] EC low: avg=" + String(ecAverage, 3) +
                     " < " + String(ecMinusHys, 2) +
                     " | Path " + String(autoMixing ? "B (mix)" : "A (no mix)") +
                     " | dose=" + String(thisDoseTime) + "s");

      ecReadingCount = 0;
      ecReadingIndex = 0;

      if (autoMixing)
      {
        writeRelay(2, true);
        enterState(AUTO_PRE_MIX);
        Serial.println("[Auto] PRE_MIX: R2 ON for " + String(PRE_MIX_DURATION / 1000) + "s");
      }
      else
      {
        relayDurations[0] = thisDoseTime;
        relayTimers[0]    = now;
        writeRelay(1, true);
        enterState(AUTO_DOSING);
        Serial.println("[Auto] DOSING: R1 ON for " + String(thisDoseTime) + "s");
      }
      break;
    }

    // -------------------------------------------------
    case AUTO_PRE_MIX:  // Path B only
      if (now - autoStateEnteredAt >= PRE_MIX_DURATION)
      {
        // Use computedDoseTime if smart dosing, otherwise fall back to dosingTime
        unsigned int thisDoseTime = (smartDosing && computedDoseTime > 0) ? computedDoseTime
                                  : (smartDosing && smartCalPhase)        ? SMART_CAL_DURATION
                                  : dosingTime;
        relayDurations[0] = thisDoseTime;
        relayTimers[0]    = now;
        relayDurations[1] = thisDoseTime + POST_MIX_DURATION / 1000;
        relayTimers[1]    = now;
        writeRelay(1, true);
        enterState(AUTO_DOSING);
        Serial.println("[Auto] DOSING: R1+R2 ON for " + String(thisDoseTime) + "s");
      }
      break;

    // -------------------------------------------------
    case AUTO_DOSING:
      if (now - autoStateEnteredAt >= (unsigned long)dosingTime * 1000UL)
      {
        writeRelay(1, false);
        relayDurations[0] = 0;
        relayTimers[0]    = 0;
        doseEndTime       = now;
        lastDoseTimestamp = time(nullptr);
        dosesToday++;

        Serial.println("[Auto] Dose complete. doses_today=" + String(dosesToday));
        {
          String doseMsg = "Auto-dose executed: " + String(dosingTime) + "s (dose #" + String(dosesToday) + " today)";
          logDeviceActivity("dosing", doseMsg.c_str());
        }

        if (autoMixing)
        {
          enterState(AUTO_POST_MIX);
          Serial.println("[Auto] POST_MIX: R2 ON for " + String(POST_MIX_DURATION / 1000) + "s");
        }
        else
        {
          stabiliseSkipCount = 0;
          enterState(AUTO_COOLDOWN);
          Serial.println("[Auto] COOLDOWN (no mix): " + String(POST_DOSE_DELAY_NO_MIX / 1000) + "s");
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
        Serial.println("[Auto] COOLDOWN (mix): " + String(POST_DOSE_DELAY_MIX / 1000) + "s");
      }
      break;

    // -------------------------------------------------
    case AUTO_COOLDOWN:
    {
      unsigned long delay = autoMixing ? POST_DOSE_DELAY_MIX : POST_DOSE_DELAY_NO_MIX;
      if (now - doseEndTime >= delay)
      {
        enterState(AUTO_STABILISING);
        Serial.println("[Auto] STABILISING: skipping " +
                       String(autoMixing ? STABILISE_SKIP_MIX : STABILISE_SKIP_NO_MIX) + " readings");
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
        Serial.println("[Auto] Response check: pre=" + String(preDoseEC, 3) +
                       " now=" + String(ecAverage, 3) +
                       " rise=" + String(ecRise, 3));

        // Smart dosing calibration / accuracy check
        if (smartCalPhase)
        {
          if (ecRise >= DOSE_RESPONSE_THRESHOLD)
          {
            ecRiseRate = ecRise / (float)SMART_CAL_DURATION;
            if (wlSensorFound)
            {
              float wlDrop = wlBeforeCal - sensors.wl;
              if (wlDrop > 0.0f) wlDropRate = wlDrop / (float)SMART_CAL_DURATION;
            }
            saveSmartCalibration();
            smartCalibrated = true;
            Serial.println("[Smart] Calibrated: ec_rate=" + String(ecRiseRate, 5) + " mS/cm/s");
            String calMsg = "Smart dosing calibrated: ec_rate=" + String(ecRiseRate, 5) +
                            " rise=" + String(ecRise, 3) + " mS/cm";
            logDeviceActivity("dosing", calMsg.c_str());
          }
          else
          {
            Serial.println("[Smart] Calibration dose had no EC rise — will retry");
          }
          smartCalPhase = false;
        }
        else if (smartDosing && smartCalibrated && computedDoseTime > 0)
        {
          float predicted = ecRiseRate * (float)computedDoseTime;
          if (predicted > 0.0f)
          {
            float error = fabsf(ecRise - predicted) / predicted;
            if (error > SMART_ERROR_THRESHOLD)
            {
              Serial.println("[Smart] Prediction error " + String(error * 100.0f, 0) +
                             "% — re-calibrating next cycle");
              smartCalibrated  = false;
              computedDoseTime = 0;
            }
          }
        }

        if (ecRise < DOSE_RESPONSE_THRESHOLD)
        {
          consecutiveIneffectiveDoses++;
          Serial.println("[Auto] Ineffective dose #" + String(consecutiveIneffectiveDoses));
          if (consecutiveIneffectiveDoses >= MAX_INEFFECTIVE_DOSES)
          {
            triggerAlarm("No EC response after " + String(MAX_INEFFECTIVE_DOSES) + " doses");
            return;
          }
        }
        else
        {
          consecutiveIneffectiveDoses = 0;
        }

        enterState(AUTO_SAMPLING);
        Serial.println("[Auto] Back to SAMPLING");
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
