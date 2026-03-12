// =====================================================
// SENSORS.CPP
// Sensor init/scan, RS485/Modbus read, EC averaging, auto-dosing
// =====================================================

#include "sensors.h"
#include "relay.h"    // writeRelay() used in checkAutoDosing

// =====================================================
// SENSOR INITIALISATION
// =====================================================

void initSensors()
{
  Serial.println("\n--- Initializing Sensors ---");

  // EC Sensor (Fixed Modbus ID 3)
  modbus.begin(EC_SENSOR_ID, Serial1);
  ecSensorFound = true;
  Serial.println("EC Sensor: ID " + String(EC_SENSOR_ID) + " (fixed)");

  // Scan for Water Level Sensor (IDs 10-19)
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
        wlSensorId   = id;
        wlSensorFound = true;
        Serial.println("Water Level: ID " + String(id));
        break;
      }
    }
    delay(30);
  }

  int found = (int)ecSensorFound + (int)wlSensorFound;
  Serial.println("Found: " + String(found) + "/2 sensors\n");
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
    modbus.begin(EC_SENSOR_ID, Serial1);
    delay(10);

    if (modbus.readHoldingRegisters(0, 4) == modbus.ku8MBSuccess)
    {
      uint16_t r0 = modbus.getResponseBuffer(0);
      uint16_t r1 = modbus.getResponseBuffer(1);
      uint16_t r2 = modbus.getResponseBuffer(2);

      uint32_t ecRaw = ((uint32_t)r0 << 16) | r1;
      sensors.ec   = ecRaw / 100000.0f;
      sensors.temp = r2 / 10.0f;

      updateECAverage(sensors.ec);
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

  // --- Publish via MQTT ---
  if (success && mqttClient.connected())
  {
    StaticJsonDocument<128> doc;
    if (ecSensorFound)
    {
      doc["ec"]   = sensors.ec;
      doc["temp"] = sensors.temp;
    }
    if (wlSensorFound)
      doc["wl"] = sensors.wl;

    char buf[128];
    serializeJson(doc, buf);
    mqttClient.publish(mqttTopicData.c_str(), buf);
  }

  // --- Debug output when auto-dosing is active ---
  if (autoDosing && ecSensorFound)
  {
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug >= 10000)
    {
      Serial.print("[Auto] EC:" + String(sensors.ec, 2));
      if (ecReadingCount >= EC_SAMPLES)
      {
        Serial.print(" Avg:" + String(ecAverage, 2));
        Serial.print(" Trig:<" + String(ecMinusHys, 2));
        Serial.println(ecAverage < ecMinusHys ? " DOSING!" : " OK");
      }
      else
      {
        Serial.println(" Samples:" + String(ecReadingCount) + "/" + String(EC_SAMPLES));
      }
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
// AUTO-DOSING LOGIC
// =====================================================

void checkAutoDosing()
{
  unsigned long now = millis();

  // Wait after startup before dosing
  if (now - autoDosingStartTime < INITIAL_WAIT)
  {
    static unsigned long lastMsg = 0;
    if (now - lastMsg >= 30000)
    {
      unsigned long rem = (INITIAL_WAIT - (now - autoDosingStartTime)) / 1000;
      Serial.println("[Auto] Startup wait: " + String(rem) + "s remaining");
      lastMsg = now;
    }
    return;
  }

  // Need a full window of samples before deciding
  if (ecReadingCount < EC_SAMPLES)
    return;

  // Wait after last dose before dosing again
  if (lastDosingTime > 0 && (now - lastDosingTime < POST_DOSE_DELAY))
  {
    static unsigned long lastMsg = 0;
    if (now - lastMsg >= 10000)
    {
      unsigned long rem = (POST_DOSE_DELAY - (now - lastDosingTime)) / 1000;
      Serial.println("[Auto] Post-dose wait: " + String(rem) + "s remaining");
      lastMsg = now;
    }
    return;
  }

  // Trigger dosing if EC average is below threshold
  if (ecAverage < ecMinusHys)
  {
    Serial.println("\n--- AUTO-DOSING ---");
    Serial.println("EC Avg: " + String(ecAverage, 3) + " < " + String(ecMinusHys, 2));
    Serial.println("Dosing for " + String(dosingTime) + "s");
    Serial.println("Mixing Pump: " + String(autoMixing ? "ON" : "OFF") + "\n");

    // Activate dosing pump (R1)
    relayDurations[0] = dosingTime;
    relayTimers[0]    = now;
    writeRelay(1, true);

    // Activate mixing pump (R2) only if enabled
    if (autoMixing)
    {
      relayDurations[1] = dosingTime;
      relayTimers[1]    = now;
      writeRelay(2, true);
    }

    lastDosingTime = now;
    ecReadingCount = 0;
    ecReadingIndex = 0;
  }
}
