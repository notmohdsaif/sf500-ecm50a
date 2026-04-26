// =====================================================
// ESP32-S3 ECM50-A SF500 Hydroponics Controller
// Version: 2.1 (WiFi Portal + Optimized)
// =====================================================

#include "globals.h"
#include "wifi_portal.h"
#include "cloud.h"
#include "mqtt_handler.h"
#include "sensors.h"
#include "relay.h"
#include "ota.h"

// =====================================================
// GLOBAL VARIABLE DEFINITIONS
// (declared extern in globals.h)
// =====================================================

WiFiClient espClient;
WiFiClientSecure secureClient;
PubSubClient mqttClient(espClient);
ModbusMaster modbus;
Preferences wifiPrefs;
WebServer portalServer(80);
DNSServer dnsServer;

WiFiState wifiState = STATE_PORTAL;
std::vector<NetItem> scanList;
bool portalMode = false;
unsigned long portalConnectStartMs = 0;

String deviceMAC;
String deviceName;
String lastSix;
bool isRegistered = false;

String mqttTopicData;
String topicRelayUpdate;
String topicRelayStatus;
String topicWifiCmd;
bool pendingWifiForget = false;
bool pendingWifiPortal = false;

uint8_t ecSensorId = 0;
uint8_t wlSensorId = 0;
bool ecSensorFound = false;
bool wlSensorFound = false;
SensorData sensors;

bool relayStates[2] = {false, false};
unsigned long relayTimers[2] = {0, 0};
unsigned int relayDurations[2] = {0, 0};

bool autoDosing = false;
bool autoMixing = false;
float ecTarget = 1.5f;
float ecMinusHys = 0.0f;
unsigned int dosingTime = 30;
float ecReadings[EC_SAMPLES];
int ecReadingIndex = 0;
int ecReadingCount = 0;
float ecAverage = 0.0f;

AutoDosingState autoState                = AUTO_IDLE;
unsigned long   autoStateEnteredAt       = 0;
float           preDoseEC                = 0.0f;
int             consecutiveIneffectiveDoses = 0;
int             dosesToday               = 0;
time_t          lastDoseTimestamp        = 0;
unsigned long   doseEndTime              = 0;
int             stabiliseSkipCount       = 0;
unsigned long   autoDosingStartTime      = 0;

unsigned long lastSensorRead = 0;
unsigned long lastSensorUpload = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastConfigCheck = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastScheduleFetch = 0;
unsigned long lastOTACheck = 0;

Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
unsigned long lastTriggeredTime[MAX_SCHEDULES] = {0};

unsigned long startupTime = 0;
bool startupComplete = false;

// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(9600);
  delay(2000);
  Serial.println("\n\n=== ESP32-S3 ECM50-A SF500 System v2.1 ===\n");

  // --- Relays ---
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  Serial.println("Relays initialized (OFF)");

  // --- RS485 ---
  Serial1.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  // --- WiFi init ---
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);

  // --- Device identity from MAC ---
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  sprintf(macStr, "%02x:%02x:%02x:%02x:%02x:%02x",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceMAC = String(macStr);

  lastSix = deviceMAC.substring(9);
  lastSix.replace(":", "");
  lastSix.toLowerCase();
  deviceName = "sf500_" + lastSix;

  // --- MQTT topics ---
  mqttTopicData    = "sf500/" + lastSix + "/data";
  topicRelayUpdate = "sf500/" + lastSix + "/relay_update";
  topicRelayStatus = "sf500/" + lastSix + "/relay_status";
  topicWifiCmd     = "sf500/" + lastSix + "/wifi_cmd";

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.println("Device: " + deviceName + " (" + deviceMAC + ")\n");

  // --- Auto-connect with saved credentials ---
  WiFi.persistent(false);

  wifiPrefs.begin("wifi", true);
  String savedSSID = wifiPrefs.getString("ssid", "");
  String savedPass = wifiPrefs.getString("pass", "");
  wifiPrefs.end();

  Serial.printf("[WiFi] Saved SSID: '%s'\n",
                savedSSID.length() > 0 ? savedSSID.c_str() : "(empty)");

  if (savedSSID.length() > 0)
  {
    Serial.printf("[WiFi] Auto-connecting to '%s'...\n", savedSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < AUTO_CONNECT_TIMEOUT_MS)
    {
      delay(200);
      Serial.print(".");
      checkRelayTimers();
      handleSerialCommands();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiState = STATE_ONLINE;
      portalMode = false;
      Serial.printf("\n[WiFi] Connected, IP=%s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
      Serial.println("\n[WiFi] Auto-connect failed, starting portal");
      startWiFiPortal();
    }
  }
  else
  {
    Serial.println("[WiFi] No saved credentials, starting portal");
    startWiFiPortal();
  }

  // --- Online initialization ---
  if (wifiState == STATE_ONLINE)
  {
    secureClient.setInsecure();
    delay(500);

    syncTimeWithNTP();
    registerDevice();

    if (isRegistered)
    {
      markAppValid();
      logDeviceActivity("system", "Device booted: v" FIRMWARE_VERSION);
      checkForOTAUpdate();
      initSensors();
      uploadSensorConfig();
      fetchDeviceConfig();

      Serial.println("Commands: R1ON/OFF, R2ON/OFF, ALLON/OFF, WIFIINFO, HELP\n");
      startupTime = millis();
    }
  }
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
  unsigned long now = millis();

  checkRelayTimers();
  handleSerialCommands();

  // --- Portal mode ---
  // handlePortalLoop() sets portalMode=false when the AP is torn down
  if (portalMode)
  {
    handlePortalLoop();
    return;
  }

  // One-time initialization after portal completes (wifiState==STATE_ONLINE, not yet registered)
  if (wifiState == STATE_ONLINE && !isRegistered && startupTime == 0)
  {
    Serial.println("[WiFi] Portal complete, initializing...");
    secureClient.setInsecure();
    delay(500);

    syncTimeWithNTP();
    registerDevice();

    if (isRegistered)
    {
      markAppValid();
      logDeviceActivity("system", "Device booted: v" FIRMWARE_VERSION);
      checkForOTAUpdate();
      initSensors();
      uploadSensorConfig();
      fetchDeviceConfig();
      startupTime = millis();
    }
  }

  // --- WiFi reconnect if dropped ---
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WiFi] Connection lost, reconnecting...");

    wifiPrefs.begin("wifi", true);
    String savedSSID = wifiPrefs.getString("ssid", "");
    String savedPass = wifiPrefs.getString("pass", "");
    wifiPrefs.end();

    if (savedSSID.length() > 0)
    {
      WiFi.begin(savedSSID.c_str(), savedPass.c_str());

      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
      {
        delay(200);
        checkRelayTimers();
        handleSerialCommands();
      }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      startWiFiPortal();
      return;
    }

    Serial.println("[WiFi] Reconnected: " + WiFi.localIP().toString());
  }

  if (!isRegistered)
  {
    delay(1000);
    return;
  }

  // --- Pending WiFi commands (deferred from MQTT callback to avoid re-entrancy) ---
  if (pendingWifiForget)
  {
    pendingWifiForget = false;
    wifiPrefs.begin("wifi", false);
    wifiPrefs.clear();
    wifiPrefs.end();
    Serial.println("[WiFi] Credentials forgotten, restarting...");
    delay(500);
    ESP.restart();
  }
  if (pendingWifiPortal)
  {
    pendingWifiPortal = false;
    Serial.println("[WiFi] Starting portal on remote command...");
    startWiFiPortal();
  }

  // --- MQTT keepalive ---
  if (!mqttClient.connected())
    reconnectMQTT();
  mqttClient.loop();

  // --- Periodic tasks ---
  if (now - lastSensorRead >= SENSOR_READ_INTERVAL)
  {
    readSensors();
    lastSensorRead = now;
    if (autoDosing && ecSensorFound)
      checkAutoDosing();
  }

  if (now - lastSensorUpload >= SENSOR_UPLOAD_INTERVAL)
  {
    if (sensors.hasData)
      uploadSensorReadings();
    lastSensorUpload = now;
  }

  if (now - lastStatusUpdate >= STATUS_UPDATE_INTERVAL)
  {
    updateDeviceStatus("online");
    lastStatusUpdate = now;
  }

  if (now - lastConfigCheck >= CONFIG_CHECK_INTERVAL)
  {
    fetchDeviceConfig();
    lastConfigCheck = now;
  }

  if (now - lastScheduleFetch >= SCHEDULE_FETCH_INTERVAL)
  {
    fetchSchedules();
    lastScheduleFetch = now;
  }

  if (now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL)
  {
    checkSchedules();
    lastScheduleCheck = now;
  }

  if (now - lastOTACheck >= OTA_CHECK_INTERVAL)
  {
    checkForOTAUpdate();
    lastOTACheck = now;
  }

  delay(10);
}
