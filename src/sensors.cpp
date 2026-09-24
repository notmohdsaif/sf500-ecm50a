// =====================================================
// SENSORS.CPP
// Sensor init/scan, RS485/Modbus read, EC averaging, auto-dosing
// =====================================================

#include "sensors.h"
#include "logger.h"
#include "relay.h"    // writeRelay() used in checkAutoDosing
#include "cloud.h"    // logDeviceActivity()
#include "cellular.h" // modem.getSignalQuality() for the data payload's cellular block
#include "sdcard.h"   // sdHealth() / sdFreeBytesCached() for the data payload's sd block
#include "journal.h"  // journalPendingBytes()

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

  // Weather station (MET) — fixed ID, not a scan range (confirmed via bench test).
  // Retried like EC's dual-register fallback above: a single attempt is
  // vulnerable to transient bus contention from the four sensor scans that
  // just ran immediately before it on the same shared RS485 bus.
  metSensorFound = false;
  modbus.begin(MET_SENSOR_ID, Serial1);
  delay(50);
  for (uint8_t attempt = 0; attempt < MET_DETECT_RETRIES && !metSensorFound; attempt++)
  {
    if (modbus.readHoldingRegisters(MET_REG_BASE, MET_REG_COUNT) == modbus.ku8MBSuccess)
    {
      metSensorId    = MET_SENSOR_ID;
      metSensorFound = true;
      LOGF("Weather station: ID %d\n", MET_SENSOR_ID);
    }
    else
    {
      delay(30);
    }
  }
  if (!metSensorFound)
    LOGF("Weather station: not found (ID %d, %d attempts)\n", MET_SENSOR_ID, MET_DETECT_RETRIES);

  // Ambient and MET share sensors.ambTemp/ambHumid/ambLux (Ambient takes
  // priority when both are present, see readSensors()) — this is expected to
  // never happen in practice, so flag it loudly if it ever does, since MET's
  // temp/humid/lux would otherwise be silently dropped with no other signal.
  if (ambSensorFound && metSensorFound)
  {
    LOGLN("WARNING: both Ambient and MET sensors detected — MET's temp/humid/lux will be ignored (Ambient takes priority)");
    logDeviceActivity("system", "WARNING: Ambient + MET sensors both present — MET temp/humid/lux ignored");
  }

  int found = (int)ecSensorFound + (int)wlSensorFound + (int)ambSensorFound + (int)rainSensorFound + (int)metSensorFound;
  LOGF("Found: %d/5 sensors\n", found);

  String msg = "Sensor init: ";
  if (ecSensorFound)   msg += "EC(ID="   + String(ecSensorId)   + ") ";
  if (wlSensorFound)   msg += "WL(ID="   + String(wlSensorId)   + ") ";
  if (ambSensorFound)  msg += "AMB(ID="  + String(ambSensorId)  + ") ";
  if (rainSensorFound) msg += "RAIN(ID=" + String(rainSensorId) + ") ";
  if (metSensorFound)  msg += "MET(ID="  + String(metSensorId)  + ")";
  msg.trim();
  if (!ecSensorFound && !wlSensorFound && !ambSensorFound && !rainSensorFound && !metSensorFound)
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
  bool success  = false;
  bool ecReadOk = false; // EC specifically produced a trustworthy value this tick —
                         // gates the average/MQTT/upload paths instead of the shared
                         // `success` flag above, which any other sensor can also set.
  bool ambOk    = false; // Ambient specifically produced a trustworthy value this tick —
                         // hoisted to function scope so the MET block below can check
                         // this tick's actual read result instead of just "is an Ambient
                         // sensor present at all" (which stays true through a transient
                         // Ambient read failure and would silently skip MET's fallback).
  static unsigned long ecImplausibleSinceMs = 0; // 0 = not currently in a below-floor streak

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
      float ecCandidate = ecRaw / 100000.0f;
      unsigned long nowMs = millis();

      // Reject implausible reads instead of trusting them — a Modbus/probe
      // fault can ACK with a valid CRC while the register itself decodes to
      // ~0 for many consecutive seconds. Left unfiltered, that pollutes the
      // rolling average enough to fire a real (but unwarranted) auto-dose,
      // and separately gets uploaded raw to sensor_metrics where it fires a
      // false ec_low alarm. Keep the last known-good value instead.
      bool plausible = ecCandidate >= EC_MIN_PLAUSIBLE;

      // ...but don't reject forever. A genuine plain-water refill can leave
      // real EC below the floor for as long as the tank stays diluted — if a
      // below-floor reading is sustained past EC_IMPLAUSIBLE_CONFIRM_MS (well
      // beyond any glitch seen in practice, well short of a real dilution's
      // multi-minute timescale), trust it as real instead of freezing auto-
      // dosing decisions on stale pre-event data forever.
      if (!plausible && ecImplausibleSinceMs != 0 &&
          (nowMs - ecImplausibleSinceMs) >= EC_IMPLAUSIBLE_CONFIRM_MS)
      {
        plausible = true;
        ecReadingCount = 0; // restart the average from this new baseline
        ecReadingIndex = 0; // instead of slowly blending in stale pre-event samples
        LOGF("[EC] Sustained low reading confirmed real after %lus — accepting %.3f mS/cm\n",
             EC_IMPLAUSIBLE_CONFIRM_MS / 1000UL, ecCandidate);
        logDeviceActivity("system", ("EC sustained low confirmed real: " +
                          String(ecCandidate, 3) + " mS/cm").c_str());
      }

      if (plausible)
      {
        sensors.ec   = ecCandidate;
        sensors.temp = r2 / 10.0f;
        success  = true;
        ecReadOk = true;
        ecImplausibleSinceMs = 0;
      }
      else
      {
        if (ecImplausibleSinceMs == 0) ecImplausibleSinceMs = nowMs;

        static unsigned long lastImplausibleLogMs = 0;
        if (nowMs - lastImplausibleLogMs >= 60000UL)
        {
          LOGF("[EC] Rejected implausible reading: %.3f mS/cm (< %.2f floor)\n",
               ecCandidate, EC_MIN_PLAUSIBLE);
          logDeviceActivity("system", ("EC reading rejected: " + String(ecCandidate, 3) +
                            " mS/cm implausible — sensor/wiring fault?").c_str());
          lastImplausibleLogMs = nowMs;
        }
      }
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

  // --- Weather Station (MET) ---
  if (metSensorFound)
  {
    modbus.begin(metSensorId, Serial1);
    delay(10);

    if (modbus.readHoldingRegisters(MET_REG_BASE, MET_REG_COUNT) == modbus.ku8MBSuccess)
    {
      uint16_t windSpeedRaw  = modbus.getResponseBuffer(0); // 500
      // reg 501 (wind force) and 502 (wind dir sector) intentionally unused — degrees preferred
      uint16_t windDirRaw    = modbus.getResponseBuffer(3);  // 503
      uint16_t humidRaw      = modbus.getResponseBuffer(4);  // 504
      uint16_t tempRaw       = modbus.getResponseBuffer(5);  // 505
      uint16_t noiseRaw      = modbus.getResponseBuffer(6);  // 506
      uint16_t pm25Raw       = modbus.getResponseBuffer(7);  // 507
      uint16_t pm10Raw       = modbus.getResponseBuffer(8);  // 508
      // Registers 509-510 unused; 511 (lux) folded into this same block read
      // (was a separate transaction) — see MET_REG_LUX_OFFSET in config.h.
      uint16_t luxRaw        = modbus.getResponseBuffer(MET_REG_LUX_OFFSET); // 511

      sensors.windSpeed = windSpeedRaw / 100.0f;
      sensors.windDir   = (float)windDirRaw;
      // Only Ambient OR MET populates the shared temp/humid/lux fields per boot
      // (ambSensorFound and metSensorFound are mutually exclusive in practice).
      // Gated on ambOk (this tick's actual Ambient read result), not just
      // ambSensorFound (hardware present) — otherwise a transient Ambient
      // Modbus failure would skip MET's fallback too and leave these stale.
      if (!ambOk)
      {
        sensors.ambTemp  = tempRaw  / 10.0f;
        sensors.ambHumid = humidRaw / 10.0f;
        sensors.ambLux   = (float)luxRaw;
      }
      sensors.noise = noiseRaw / 10.0f;
      sensors.pm25  = (float)pm25Raw;
      sensors.pm10  = (float)pm10Raw;
      success = true;
    }
    delay(50);
  }

  if (success)
    sensors.hasData = true;

  // --- Publish via MQTT (always publish if connected; sensor fields only when available) ---
  if (mqttClient.connected())
  {
    StaticJsonDocument<2048> doc;   // +256 for the "sd" block, +256 for the "met" block

    if (success)
    {
      if (ecReadOk)
      {
        doc["ec"]   = sensors.ec;
        doc["temp"] = sensors.temp;
      }
      if (wlSensorFound)
        doc["wl"] = sensors.wl;
      if (ambSensorFound || metSensorFound)
      {
        JsonObject ambObj  = doc.createNestedObject("amb");
        ambObj["temp"]     = serialized(String(sensors.ambTemp,  1));
        ambObj["humid"]    = serialized(String(sensors.ambHumid, 1));
        ambObj["lux"]      = serialized(String(sensors.ambLux,   0));
      }
      if (metSensorFound)
      {
        JsonObject metObj = doc.createNestedObject("met");
        metObj["ws"]      = serialized(String(sensors.windSpeed, 2));
        metObj["wd"]      = (int)sensors.windDir;
        metObj["noise"]   = serialized(String(sensors.noise, 1));
        metObj["pm25"]    = (int)sensors.pm25;
        metObj["pm10"]    = (int)sensors.pm10;
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

    if (tasmotaPlugEnabled)
    {
      JsonObject plugObj   = doc.createNestedObject("plug");
      plugObj["transport"] = plugUseHttp ? "http" : "mqtt";
      plugObj["power"]     = r3State ? 1 : 0;
      if (plugUseHttp)
        plugObj["online"]  = plugHttpReachable;
    }

    // Present once the modem has been detected (i.e. after a fallback ever
    // occurred). Lets the dashboard show which transport is live even when
    // the wifi block is absent because WiFi is down.
    if (cellularCapable)
    {
      JsonObject cellObj = doc.createNestedObject("cellular");
      cellObj["active"]  = (activeTransport == TRANSPORT_CELLULAR);
      cellObj["apn"]     = cellularApn;
      if (activeTransport == TRANSPORT_CELLULAR)
      {
        // AT+CSQ is a blocking round-trip on the same UART mux MQTT rides on;
        // this builder runs every publish. Signal moves slowly — cache 30s.
        static int          csqCache = 99;
        static unsigned long csqAt   = 0;
        if (csqAt == 0 || millis() - csqAt >= 30000)
        {
          csqCache = modem.getSignalQuality();
          csqAt    = millis();
        }
        cellObj["signal"] = csqCache;
      }
    }

    doc["fw"] = FIRMWARE_VERSION;

    // microSD state — lets the fleet be checked from Supabase/MQTT after a card
    // is plugged in, with no serial console. "ok" once, plus free space and the
    // unsent-buffer size; otherwise "absent" / "unreadable" (see sdHealth()).
    {
      JsonObject sdObj = doc.createNestedObject("sd");
      switch (sdHealth())
      {
        case SD_HEALTH_OK:
          sdObj["state"]     = "ok";
          sdObj["free_mb"]   = (uint32_t)(sdFreeBytesCached() / (1024ULL * 1024ULL));
          sdObj["pending_b"] = (uint32_t)journalPendingBytes();
          break;
        case SD_HEALTH_UNREADABLE: sdObj["state"] = "unreadable"; break;
        default:                   sdObj["state"] = "absent";     break;
      }
    }

    JsonObject sensorsObj = doc.createNestedObject("sensors");
    sensorsObj["ec"]   = ecSensorFound;
    sensorsObj["wl"]   = wlSensorFound;
    // "amb" means "ambient-type data is present in this payload's amb{} object",
    // which MET populates too when Ambient itself is absent (see the "amb"/"met"
    // JSON blocks above) — mirror that here so a MET-only device doesn't report
    // amb:false while amb{temp,humid,lux} is actively streaming from MET.
    sensorsObj["amb"]  = ambSensorFound || metSensorFound;
    sensorsObj["rain"] = rainSensorFound;
    sensorsObj["met"]  = metSensorFound;

    doc["rescan_seq"] = rescanSeq;

    char buf[2048];
    serializeJson(doc, buf);
    mqttClient.publish(mqttTopicData.c_str(), buf);
  }

  // Only add to rolling average when R1 is NOT dosing and not in stabilising skip window
  if (ecReadOk && !relayStates[0])
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

// True if the R3 refill relay was ON at any point since the current dose's preDoseEC was
// captured. A scheduled/manual refill dilutes the tank while dosing runs, so the EC rise
// measured at STABILISING is not attributable to the dose — skip response-check accounting
// for that cycle instead of counting it as a failed dose. Reset when a new dose is triggered.
static bool refillActiveDuringDose = false;

// WL baseline captured alongside preDoseEC (start of the current dose cycle) and, separately,
// the moment AUTO_ALARM is entered. A rise of WL_JUMP_THRESHOLD_MM or more since either baseline
// is a plug-independent "water was added" signal — mirrors what r3State tells us on plug sites,
// but works from the WL sensor alone, so it also covers sites with no Tasmota plug at all (and
// catches a manual top-up even on a plug site). See config.h for the threshold and its rationale.
static float wlAtCycleStart  = 0.0f;
static float wlAtAlarmEntry  = 0.0f;

// EC at the start of the current consecutive-ineffective-dose streak. A dose landing just
// under DOSE_RESPONSE_THRESHOLD doesn't necessarily mean the pump has stopped working — near
// target, each dose's individual rise naturally shrinks. Comparing against the streak's start
// (not just the immediately preceding dose) tells "genuinely flat, 3 doses running" apart from
// "still climbing, just slowly" before alarming. Set whenever consecutiveIneffectiveDoses goes
// 0 -> 1; only meaningful while a streak is active.
static float ecAtStreakStart = 0.0f;

// Total ineffective doses since the last genuinely effective one — unlike
// consecutiveIneffectiveDoses, this is NOT cleared by a streak-cumulative
// reset. Caps how long a series of near-threshold (possibly noise-driven)
// doses can keep deferring the alarm via MAX_TOTAL_INEFFECTIVE_DOSES.
static int totalIneffectiveDoses = 0;

static void enterState(AutoDosingState next)
{
  autoState          = next;
  autoStateEnteredAt = millis();
}

static void triggerAlarm(const String& reason, AutoDosingAlarmReason reasonCode)
{
  LOGLNS("\n[Auto] ALARM: " + reason);
  // Do NOT set autoDosing = false here — the config fetch would re-enable it automatically.
  // Instead, block all activity via AUTO_ALARM state; user must toggle off→on to reset.
  writeRelay(1, false);
  writeRelay(2, false);
  lastAlarmReason = reasonCode;
  wlAtAlarmEntry   = sensors.wl; // baseline for the WL-jump auto-recovery check below
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

  // Track refill activity across the whole dose cycle (PRE_MIX..STABILISING), not just
  // at the instant of the response check — a refill that toggles off before STABILISING
  // still diluted the tank during dosing.
  if (r3State)
    refillActiveDuringDose = true;

  // Auto-recovery: if a refill just finished (R3 went ON->OFF — either an
  // ESP32-commanded stop or the Tasmota plug's own status broadcast, both of
  // which update r3State) and the tank settled above the dosing minimum,
  // clear a "No EC response" AUTO_ALARM automatically instead of requiring a
  // manual toggle. The other two AUTO_ALARM reasons (EC ceiling-hold, smart
  // calibration failed) are left alone — a refill doesn't address either.
  static bool          prevR3State        = false;
  static bool          refillResetPending = false;
  static unsigned long refillOffAtMs      = 0;

  // Only arm on a genuine refill: plugMode "fertigate"/"custom" also drive R3
  // off for reasons that have nothing to do with topping up the tank, and
  // shouldn't be able to clear a real "pump isn't responding" alarm just
  // because WL happens to already be above the dosing minimum.
  if (prevR3State && !r3State && plugMode == "refill")
  {
    refillResetPending = true;
    refillOffAtMs      = now;
  }
  else if (r3State)
  {
    // Refill resumed before the settle window elapsed (another pulse in a
    // multi-stage refill) — wait for the next OFF edge instead.
    refillResetPending = false;
  }
  prevR3State = r3State;

  if (refillResetPending && (now - refillOffAtMs >= REFILL_RESET_DELAY))
  {
    if (autoState != AUTO_ALARM || lastAlarmReason != ALARM_REASON_NO_EC_RESPONSE)
    {
      refillResetPending = false; // nothing to recover from
    }
    else
    {
      bool wlOk = !wlSensorFound || minWlDosing == 0 ||
                  (unsigned int)sensors.wl > minWlDosing;
      if (wlOk)
      {
        refillResetPending = false;
        LOGLN("[Auto] Refill completed, WL above minimum — resetting from ALARM");
        logDeviceActivity("dosing", "Auto-dosing reset: refill completed, WL above minimum");
        lastAlarmReason = ALARM_REASON_NONE;
        enterState(AUTO_IDLE);
      }
      // else: WL hasn't settled above the minimum yet — keep retrying every
      // tick until it does, or until R3 turns back on (which cancels this
      // pending reset above).
    }
  }

  // Plug-independent auto-recovery: same "No EC response" reset as above, but keyed off a WL
  // rise instead of an R3 edge — covers sites with no Tasmota plug at all (or a manual top-up
  // at a plug site the plug never saw). No settle delay needed: unlike R3, there's no relay
  // bounce to debounce, and AUTO_ALARM already forces both relays off, so nothing but a real
  // top-up can move WL while we're checking.
  if (wlSensorFound && autoState == AUTO_ALARM && lastAlarmReason == ALARM_REASON_NO_EC_RESPONSE &&
      (sensors.wl - wlAtAlarmEntry) >= WL_JUMP_THRESHOLD_MM &&
      (minWlDosing == 0 || (unsigned int)sensors.wl > minWlDosing))
  {
    LOGLN("[Auto] WL jump detected post-alarm, WL above minimum — resetting from ALARM");
    logDeviceActivity("dosing", "Auto-dosing reset: WL jump detected, WL above minimum");
    lastAlarmReason = ALARM_REASON_NONE;
    enterState(AUTO_IDLE);
  }

  switch (autoState)
  {
    // -------------------------------------------------
    case AUTO_IDLE:
      ecReadingCount = 0;
      ecReadingIndex = 0;
      consecutiveIneffectiveDoses = 0;
      totalIneffectiveDoses = 0;
      stabiliseSkipCount = 0;
      calRetryCount = 0;
      lastAlarmReason = ALARM_REASON_NONE;
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
      // Stall guard: ecReadingCount only advances via updateECAverage() on a
      // successful, plausible EC read (readSensors()'s ecReadOk gate) — the
      // same dependency that can strand AUTO_STABILISING. Here there's no
      // relay/dose in progress to resolve, so it's not unsafe to just keep
      // waiting, but a probe that never produces usable data would otherwise
      // sit here silently forever with zero visibility. Surface it instead.
      if (ecReadingCount < EC_SAMPLES && now - autoStateEnteredAt >= SAMPLING_STALL_TIMEOUT_MS)
      {
        triggerAlarm("No usable EC data for " + String(SAMPLING_STALL_TIMEOUT_MS / 60000UL) +
                     "+ min (" + String(ecReadingCount) + "/" + String(EC_SAMPLES) +
                     " samples) — check EC probe/wiring", ALARM_REASON_EC_DATA_UNAVAILABLE);
        return;
      }

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
                       String(ecTarget + EC_CEILING_MARGIN, 2) + ")",
                       ALARM_REASON_EC_CEILING);
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
      refillActiveDuringDose = r3State; // start this cycle's window fresh
      wlAtCycleStart = sensors.wl;      // baseline for this cycle's WL-jump check

      // Determine dose duration
      unsigned int thisDoseTime = dosingTime;
      if (smartDosing && !smartCalibrated)
      {
        // P7: scale calibration dose with dosingTime so large tanks get enough coverage
        actualCalDuration = max((unsigned int)SMART_CAL_DURATION, dosingTime);
        smartCalPhase     = true;
        wlBeforeCal       = sensors.wl;

        // P11B: cap calibration dose to 70% of available EC headroom when a prior rate exists.
        // Prevents re-calibration from overshooting when EC is already near target.
        // Skipped when ecRiseRate == 0 (first-ever calibration — no rate to estimate from).
        // Conservative floor: use max(effectiveRate, SMART_RATE_MAX) so a dramatically
        // underestimated stored rate cannot produce a falsely large safeTime.
        if (ecRiseRate > 0.0f)
        {
          float headroom      = (ecTarget + EC_CEILING_MARGIN) - preDoseEC;
          float effectiveRate = ecRiseRate *
                                (wlAtCal > 0.0f ? wlAtCal / wlBeforeCal : 1.0f);
          float conservativeRate = max(effectiveRate, SMART_RATE_MAX);
          if (headroom > 0.0f)
          {
            unsigned int safeTime = (unsigned int)(headroom * 0.7f / conservativeRate);
            actualCalDuration     = max((unsigned int)SMART_MIN_DOSE,
                                        min(actualCalDuration, safeTime));
            LOGF("[Smart] Headroom cap: %.2f mS/cm headroom rate=%.5f -> safe cal %ds\n",
                 headroom, conservativeRate, actualCalDuration);
          }
        }

        thisDoseTime = actualCalDuration;
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

      // Stall guard: a flaky EC probe (failed Modbus reads, or an implausible
      // value still waiting out EC_IMPLAUSIBLE_CONFIRM_MS) can starve both
      // stabiliseSkipCount and ecReadingCount below, since neither advances
      // without a successful read — without this, that strands auto-dosing
      // here until a manual reset. Route the timeout through the SAME
      // ineffective-dose accounting as a real "no rise detected" result
      // (rather than a free pass straight back to SAMPLING) so a probe that's
      // just healthy enough to keep triggering doses, but unreliable enough to
      // fail specifically during every stabilising window, still eventually
      // reaches ALARM_REASON_NO_EC_RESPONSE instead of dosing forever with the
      // safety net silently defeated.
      if (now - autoStateEnteredAt >= STABILISE_TIMEOUT_MS)
      {
        LOGF("[Auto] STABILISING timed out after %lus (skip %d/%d, samples %d/%d) — check EC probe\n",
             STABILISE_TIMEOUT_MS / 1000UL, stabiliseSkipCount, skipTarget,
             ecReadingCount, EC_SAMPLES);

        bool wlJumpDuringDose = wlSensorFound &&
                                (sensors.wl - wlAtCycleStart) >= WL_JUMP_THRESHOLD_MM;
        if (refillActiveDuringDose || wlJumpDuringDose)
        {
          LOGLN("[Auto] Timed-out response check skipped — refill active during dose cycle");
          logDeviceActivity("dosing",
            "Stabilising timed out, response check skipped — refill active during cycle");
        }
        else if (smartCalPhase)
        {
          calRetryCount++;
          LOGF("[Smart] Calibration had no EC data — retry %d/%d\n", calRetryCount, SMART_CAL_MAX_RETRIES);
          if (calRetryCount >= SMART_CAL_MAX_RETRIES)
          {
            triggerAlarm("Smart calibration failed after " + String(calRetryCount) +
                         " attempts (no EC data)", ALARM_REASON_SMART_CAL_FAILED);
            return;
          }
          smartCalPhase = false;
          logDeviceActivity("dosing", "Stabilising timed out during smart-cal — no EC data, retrying");
        }
        else
        {
          if (consecutiveIneffectiveDoses == 0)
            ecAtStreakStart = preDoseEC;
          consecutiveIneffectiveDoses++;
          totalIneffectiveDoses++;
          LOGF("[Auto] Ineffective dose (no EC data) #%d (total %d)\n",
               consecutiveIneffectiveDoses, totalIneffectiveDoses);
          logDeviceActivity("dosing", ("Stabilising timed out — no EC data to confirm dose #" +
                            String(dosesToday) + " (" + String(consecutiveIneffectiveDoses) +
                            " consecutive)").c_str());

          if (totalIneffectiveDoses >= MAX_TOTAL_INEFFECTIVE_DOSES)
          {
            triggerAlarm("No EC response after " + String(totalIneffectiveDoses) +
                         " doses (total, incl. missing-data cycles)", ALARM_REASON_NO_EC_RESPONSE);
            return;
          }
          if (consecutiveIneffectiveDoses >= MAX_INEFFECTIVE_DOSES)
          {
            // No trustworthy ecAverage this cycle — can't evaluate the
            // cumulative-streak grace period real ineffective doses get
            // (sensors.cpp's streakRise check below), so alarm straight away.
            triggerAlarm("No EC response after " + String(MAX_INEFFECTIVE_DOSES) +
                         " doses (missing EC data)", ALARM_REASON_NO_EC_RESPONSE);
            return;
          }
        }

        enterState(AUTO_SAMPLING);
        LOGLN("[Auto] Back to SAMPLING (stabilising timed out)");
        break;
      }

      if (stabiliseSkipCount < skipTarget)
        return; // readings are being skipped in readSensors via the guard

      // Check dose response
      if (ecReadingCount >= EC_SAMPLES)
      {
        float ecRise = ecAverage - preDoseEC;
        LOGF("[Auto] Response check: pre=%.3f now=%.3f rise=%.3f\n",
             preDoseEC, ecAverage, ecRise);

        // Refill ran during this dose cycle — incoming fresh water dilutes the tank
        // independently of the dose, so ecRise doesn't reflect the dose's true effect.
        // Don't count it toward consecutiveIneffectiveDoses either way; just re-sample
        // once the tank has settled. Detected via R3 (plug sites) or a WL rise since
        // wlAtCycleStart (any site with a WL sensor — see config.h WL_JUMP_THRESHOLD_MM).
        bool wlJumpDuringDose = wlSensorFound &&
                                (sensors.wl - wlAtCycleStart) >= WL_JUMP_THRESHOLD_MM;
        if (refillActiveDuringDose || wlJumpDuringDose)
        {
          LOGF("[Auto] Response check skipped — refill active during dose cycle (R3=%d, WL jump=%d)\n",
               refillActiveDuringDose, wlJumpDuringDose);
          logDeviceActivity("dosing", "Dose response check skipped — refill active during cycle");
          enterState(AUTO_SAMPLING);
          LOGLN("[Auto] Back to SAMPLING");
          break;
        }

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
              triggerAlarm("Smart calibration failed after " + String(calRetryCount) + " attempts",
                           ALARM_REASON_SMART_CAL_FAILED);
              return;
            }
          }
          smartCalPhase = false;
        }
        else if (!lastDoseAborted && smartDosing && smartCalibrated && computedDoseTime > 0)
        {
          // Skip accuracy check on aborted doses — actual dose time < planned time guarantees
          // false error, which would invalidate good calibration data.
          //
          // P11A: WL-normalise the prediction before comparing.
          // computedDoseTime was extended by the WL correction factor (wl/wlAtCal), so
          // multiplying by ecRiseRate (calibrated at wlAtCal) double-inflates the prediction.
          // Dividing back by the same factor gives the true expected rise.
          float wlNorm    = (wlAtCal > 0.0f && sensors.wl > 0.0f)
                            ? wlAtCal / sensors.wl : 1.0f;
          float predicted = ecRiseRate * (float)computedDoseTime * wlNorm;
          if (predicted > 0.0f)
          {
            float error = fabsf(ecRise - predicted) / predicted;
            if (error > SMART_ERROR_THRESHOLD)
            {
              LOGF("[Smart] Prediction error %.0f%% (pred=%.3f act=%.3f) — re-calibrating\n",
                   error * 100.0f, predicted, ecRise);
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
            if (consecutiveIneffectiveDoses == 0)
              ecAtStreakStart = preDoseEC; // mark this streak's baseline

            consecutiveIneffectiveDoses++;
            totalIneffectiveDoses++;
            LOGF("[Auto] Ineffective dose #%d (total %d)\n",
                 consecutiveIneffectiveDoses, totalIneffectiveDoses);

            // Hard ceiling — independent of the streak-cumulative check below.
            // Caps how long a run of individually-marginal doses (possibly just
            // EC probe noise, not real progress) can keep deferring the alarm.
            if (totalIneffectiveDoses >= MAX_TOTAL_INEFFECTIVE_DOSES)
            {
              triggerAlarm("No EC response after " + String(totalIneffectiveDoses) +
                           " doses (total)", ALARM_REASON_NO_EC_RESPONSE);
              return;
            }

            if (consecutiveIneffectiveDoses >= MAX_INEFFECTIVE_DOSES)
            {
              // Individually marginal doses can still add up to real progress near
              // target (diminishing returns), which isn't the same as the pump
              // genuinely not responding. Check the streak as a whole before alarming.
              float streakRise = ecAverage - ecAtStreakStart;
              if (streakRise >= DOSE_RESPONSE_THRESHOLD)
              {
                LOGF("[Auto] Ineffective streak reset — cumulative rise %.3f over %d doses\n",
                     streakRise, consecutiveIneffectiveDoses);
                logDeviceActivity("dosing", ("Ineffective-dose streak reset: cumulative rise " +
                                  String(streakRise, 3) + " mS/cm over " +
                                  String(consecutiveIneffectiveDoses) + " doses").c_str());
                consecutiveIneffectiveDoses = 0; // totalIneffectiveDoses keeps accumulating
              }
              else
              {
                triggerAlarm("No EC response after " + String(MAX_INEFFECTIVE_DOSES) + " doses",
                             ALARM_REASON_NO_EC_RESPONSE);
                return;
              }
            }
          }
        }
        else
        {
          // Any genuinely effective dose (cal or production) clears both counters —
          // real progress means the pump is demonstrably working.
          consecutiveIneffectiveDoses = 0;
          totalIneffectiveDoses       = 0;
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
