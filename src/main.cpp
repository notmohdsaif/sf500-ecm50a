// =====================================================
// ESP32-S3 ECM50-A SF500 Hydroponics Controller
// Version: 2.1 (WiFi Portal + Optimized)
// =====================================================

#include "globals.h"
#include "logger.h"
#include "wifi_portal.h"
#include "cloud.h"
#include "mqtt_handler.h"
#include "sensors.h"
#include "relay.h"
#include "ota.h"
#include "cellular.h"
#include <esp_task_wdt.h>

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
bool cellularCapable = false;
bool cellularDetectAttempted = false;
NetworkTransport activeTransport = TRANSPORT_WIFI;
String cellularApn = "";
bool forcePortalGesture = false;   // set by the 3x power-cycle check in setup()
std::vector<NetItem> scanList;
bool portalMode = false;
unsigned long portalConnectStartMs = 0;
unsigned long portalStartedAt      = 0;

String deviceMAC;
String deviceName;
String lastSix;
bool isRegistered = false;

String mqttTopicData;
String topicRelayUpdate;
String topicRelayStatus;
String topicWifiCmd;
String topicDeviceCmd;
bool pendingWifiForget = false;
bool pendingWifiPortal = false;
bool pendingRescan = false;
bool pendingAutoDosingReset = false;
uint32_t rescanSeq = 0;

uint8_t ecSensorId   = 0;
uint8_t wlSensorId   = 0;
uint8_t ambSensorId  = 0;
uint8_t rainSensorId = 0;
int     lastRainResetDay = -1;
bool ecSensorFound   = false;
bool wlSensorFound   = false;
bool ambSensorFound  = false;
bool rainSensorFound = false;
SensorData sensors;

bool relayStates[2] = {false, false};
unsigned long relayTimers[2] = {0, 0};
unsigned int relayDurations[2] = {0, 0};

String        tasmotaPlugTopic   = "";
bool          tasmotaPlugEnabled = false;
bool          r3State            = false;
unsigned long r3Timer            = 0;
unsigned int  r3Duration         = 0;
String        plugMode           = "custom";
float         refillCutoffMm     = 0.0f;

String        plugHttpHost        = "";
bool          plugUseHttp         = false;
IPAddress     plugHttpHostIp;                 // 0.0.0.0 until resolved
unsigned long lastPlugHostResolve = 0;
bool          plugHttpReachable   = false;
uint8_t       plugHttpFailStreak  = 0;
unsigned long lastPlugHttpPoll    = 0;

bool autoDosing = false;
bool autoMixing = false;
float ecTarget = 1.5f;
float ecMinusHys = ecTarget - EC_HYSTERESIS;
unsigned int dosingTime = 30;

bool         smartDosing       = false;
bool         smartCalibrated   = false;
bool         smartCalPhase     = false;
float        ecRiseRate        = 0.0f;
float        wlDropRate        = 0.0f;
float        wlBeforeCal       = 0.0f;
float        wlAtCal           = 0.0f;
unsigned int computedDoseTime  = 0;
unsigned int actualCalDuration = 0;
int          calRetryCount     = 0;
float ecReadings[EC_SAMPLES];
int ecReadingIndex = 0;
int ecReadingCount = 0;
float ecAverage = 0.0f;

AutoDosingState autoState                = AUTO_IDLE;
AutoDosingAlarmReason lastAlarmReason    = ALARM_REASON_NONE;
unsigned long   autoStateEnteredAt       = 0;
float           preDoseEC                = 0.0f;
int             consecutiveIneffectiveDoses = 0;
int             dosesToday               = 0;
int             lastDoseDay              = -1;
time_t          lastDoseTimestamp        = 0;
unsigned long   doseEndTime              = 0;
int             stabiliseSkipCount       = 0;
unsigned int    activeDoseTime           = 0;
unsigned int    minWlDosing              = 0;
bool            lastDoseAborted          = false;
unsigned int    actualDoseElapsed        = 0;

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

#ifdef ENABLE_OTA_LOGS
String        topicLogs;
unsigned long lastLogPublish = 0;
#endif

// =====================================================
// SETUP
// =====================================================

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW_RESTART";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    default:                return "OTHER";
  }
}

// loopTask (runs setup()/loop() and everything called from them) gets only
// 8192 bytes by default. Sensor JSON building + WiFiClientSecure/mbedTLS
// HTTPS calls run in the same task and can overflow that, corrupting the
// stack canary (Guru Meditation, no auto-reboot). This is the Arduino-ESP32
// core's official override point for the loop task's stack size.
size_t getArduinoLoopTaskStackSize(void) {
  return 20480;
}

// Cellular fallback trigger. Called from loop()'s transport state machine when
// WiFi is unreachable — whether saved creds failed to connect or there are none
// at all (fresh unit / after a wifi_cmd forget). Lazily probes the modem once
// per boot, then, if a modem and an APN are both present, brings up a cellular
// data session and moves MQTT + Supabase onto it. Rate-limited so a failing
// connect can't peg the loop.
static void tryCellularFallback()
{
  if (activeTransport != TRANSPORT_WIFI)
    return;
  if (WiFi.status() == WL_CONNECTED)
    return; // WiFi is actually up (e.g. portal creds just worked) — no fallback

  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < 60000)
    return;
  lastAttempt = millis();

  if (!cellularCapable && !cellularDetectAttempted)
  {
    cellularDetectAttempted = true;
    cellularCapable = detectCellularModem();
    LOGF("[Cellular] Modem %s\n", cellularCapable ? "detected" : "not present");
  }

  // No modem, or no APN configured -> feature stays inert (spec safety default).
  if (!cellularCapable || cellularApn.length() == 0)
    return;

  LOGLN("[Cellular] No WiFi — attempting cellular fallback...");
  esp_task_wdt_reset(); // fresh 60s for the (blocking) modem bring-up
  if (connectCellularData(cellularApn.c_str()))
  {
    activeTransport = TRANSPORT_CELLULAR;
    mqttClient.disconnect();
    mqttClient.setClient(cellularClient);
    LOGLN("[Cellular] Fallback active");
  }
  else
  {
    LOGLN("[Cellular] Fallback attempt failed — will retry");
  }
}

// Non-blocking WiFi reconnect attempt while the device is running on cellular
// and the portal is NOT open (when it is, handlePortalLoop() does this). Fires
// on the same cadence as the portal's own retry. The up-switch check in loop()
// tears cellular down once this associates.
static void retryWifiInBackground(unsigned long now)
{
  static unsigned long lastRetry = 0;
  if (activeTransport != TRANSPORT_CELLULAR || portalMode)
    return;
  if (lastRetry != 0 && now - lastRetry < PORTAL_SAVED_RETRY_INTERVAL_MS)
    return;
  lastRetry = now;

  wifiPrefs.begin("wifi", true);
  String ssid = wifiPrefs.getString("ssid", "");
  String pass = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  if (ssid.isEmpty())
    return;

  LOGF("[WiFi] Background retry of '%s' while on cellular...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// A human explicitly asked for the portal — open it right away, regardless of
// transport state. (1) wifi_cmd: portal, now deliverable over cellular.
// (2) 3x power-cycle gesture (forcePortalGesture, set in setup()).
static bool portalRequestedByHuman()
{
  if (pendingWifiPortal)
  {
    pendingWifiPortal = false;
    LOGLN("[Portal] Opening on remote command");
    return true;
  }
  if (forcePortalGesture)
  {
    forcePortalGesture = false;
    LOGLN("[Portal] Opening on 3x power-cycle gesture");
    return true;
  }
  return false;
}

// Open the AP because the device has no other way to be useful: a fresh unit
// that must be provisioned, or one that's genuinely offline (WiFi down AND
// cellular didn't take). Checked only AFTER the transport state machine has
// had its shot at cellular.
static bool shouldOpenPortalOffline()
{
  bool haveCell = (activeTransport == TRANSPORT_CELLULAR);
  bool wifiDown = (WiFi.status() != WL_CONNECTED);

  wifiPrefs.begin("wifi", true);
  bool haveCreds = wifiPrefs.getString("ssid", "").length() > 0;
  wifiPrefs.end();

  if (!haveCreds && !haveCell) return true; // must be provisioned on-site
  if (wifiDown && !haveCell)   return true; // genuinely offline

  return false;
}

// One-time "we have an uplink" initialisation — registration, time, sensors,
// config. Runs once per boot (guarded by startupTime == 0 at the call sites),
// the first time the device is reachable on ANY transport (WiFi or cellular).
// Extracted from what used to be duplicated in setup() and loop().
static void bringOnline()
{
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(5);  // bound stalled TLS handshakes (s)
  delay(500);

  esp_task_wdt_reset();
  if (activeTransport == TRANSPORT_CELLULAR)
    syncTimeFromModem();
  else
    syncTimeWithNTP();

  esp_task_wdt_reset();
  registerDevice();

  if (isRegistered)
  {
    markAppValid();
    logDeviceActivity("system", "Device booted: v" FIRMWARE_VERSION);
    if (activeTransport == TRANSPORT_WIFI)
      checkForOTAUpdate();   // ota.cpp uses its own WiFiClientSecure — WiFi only
    esp_task_wdt_reset();
    initSensors();
    loadSmartCalibration();
    loadRainResetState();
    esp_task_wdt_reset();
    uploadSensorConfig();
    fetchDeviceConfig();
    startupTime = millis();
    LOGLN("Commands: R1ON/OFF, R2ON/OFF, ALLON/OFF, WIFIINFO, HELP\n");
  }
}

void setup()
{
  Serial.begin(9600);
  delay(2000);
  LOGLN("\n\n=== ESP32-S3 ECM50-A SF500 System v2.1 ===\n");
  LOGF("[DIAG] Reset: %s | Heap: %d bytes\n", resetReasonStr(esp_reset_reason()), ESP.getFreeHeap());

  // --- Relays ---
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  LOGLN("Relays initialized (OFF)");

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
  topicDeviceCmd   = "sf500/" + lastSix + "/device_cmd";
#ifdef ENABLE_OTA_LOGS
  topicLogs        = "sf500/" + lastSix + "/logs";
#endif

  mqttClient.setBufferSize(4096);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  // Keepalive headroom: a single blocking HTTP call (≤6s) must not exceed the
  // keepalive window, or the broker drops us mid-loop and the dashboard sees
  // the device "go offline" until a page refresh catches the next publish.
  mqttClient.setKeepAlive(60);     // default was 15s
  mqttClient.setSocketTimeout(8);  // default was 15s

  LOGLNS("Device: " + deviceName + " (" + deviceMAC + ")\n");

  // --- Auto-connect with saved credentials ---
  WiFi.persistent(false);

  wifiPrefs.begin("wifi", true);
  String savedSSID = wifiPrefs.getString("ssid", "");
  String savedPass = wifiPrefs.getString("pass", "");
  wifiPrefs.end();

  // Cellular APN persists across reboots so fallback still works after a
  // wifi_cmd forget (which clears WiFi creds and restarts) or a power cut
  // when the device can't reach Supabase to re-fetch it.
  wifiPrefs.begin("cellular", true);
  cellularApn = wifiPrefs.getString("apn", "");
  wifiPrefs.end();
  if (cellularApn.length() > 0)
    LOGF("[Cellular] Saved APN: %s\n", cellularApn.c_str());

  // 3x rapid power-cycle -> force the captive portal (physical escape hatch,
  // no button on the board). Only counts power-on resets; loop() zeroes the
  // counter after ~8s of uptime so normal runs don't accumulate.
  {
    wifiPrefs.begin("boot", false);
    uint32_t bc = (esp_reset_reason() == ESP_RST_POWERON)
                    ? wifiPrefs.getUInt("cnt", 0) + 1
                    : 0;
    if (bc >= 3)
    {
      forcePortalGesture = true;
      bc = 0;
      LOGLN("[Boot] 3x power-cycle detected — portal will open");
    }
    wifiPrefs.putUInt("cnt", bc);
    wifiPrefs.end();
  }

  LOGF("[WiFi] Saved SSID: '%s'\n",
       savedSSID.length() > 0 ? savedSSID.c_str() : "(empty)");

  if (savedSSID.length() > 0)
  {
    LOGF("[WiFi] Auto-connecting to '%s'...\n", savedSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < AUTO_CONNECT_TIMEOUT_MS)
    {
      delay(200);
      checkRelayTimers();
      handleSerialCommands();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiState = STATE_ONLINE;
      portalMode = false;
      LOGF("\n[WiFi] Connected, IP=%s RSSI=%ddBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    else
    {
      // Don't force the portal here — loop()'s transport state machine tries
      // cellular first, and shouldOpenPortal() opens the AP only if that also
      // fails (or a human asks for it).
      LOGLN("\n[WiFi] Auto-connect failed — deferring to transport state machine");
    }
  }
  else
  {
    LOGLN("[WiFi] No saved credentials — deferring to transport state machine");
  }

  // --- Online initialisation (WiFi here; the cellular path runs it from loop()) ---
  if (wifiState == STATE_ONLINE && startupTime == 0)
    bringOnline();

  // Watchdog: if loop() freezes for >60s, hard-reset the device.
  // 60s covers worst-case WiFi reconnect (30s) + one blocking HTTP call (6s).
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop()
{
  unsigned long now = millis();
  esp_task_wdt_reset();

  checkRelayTimers();
  handleSerialCommands();

  // Clear the 3x power-cycle gesture counter once we've run long enough that
  // this clearly wasn't part of a rapid reset sequence.
  static bool bootCntCleared = false;
  if (!bootCntCleared && now > 8000)
  {
    bootCntCleared = true;
    wifiPrefs.begin("boot", false);
    wifiPrefs.putUInt("cnt", 0);
    wifiPrefs.end();
  }

  // --- Captive portal ---
  // Tracks WHY the AP is open: an auto-opened offline portal yields to cellular
  // once fallback succeeds; a human-requested one stays up for on-site setup.
  static bool portalAutoOpened = false;
  if (portalMode)
    handlePortalLoop();   // runs its own WiFi retry; clears portalMode on teardown
  else if (portalRequestedByHuman())
  {
    portalAutoOpened = false;
    startWiFiPortal();     // explicit request — open now, before any blocking work
  }

  // --- Transport state machine: WiFi <-> cellular ---
  // Down-switch: WiFi is gone and we're still nominally on WiFi. Try a few
  // quick reconnects, then hand off to cellular (no portal).
  if (WiFi.status() != WL_CONNECTED && activeTransport == TRANSPORT_WIFI)
  {
    bool reconnected = false;

    if (!portalMode)   // when the portal is up it does its own retry
    {
      wifiPrefs.begin("wifi", true);
      String savedSSID = wifiPrefs.getString("ssid", "");
      String savedPass = wifiPrefs.getString("pass", "");
      wifiPrefs.end();

      if (savedSSID.length() > 0)
      {
        LOGLN("[WiFi] Connection lost, reconnecting...");
        for (int attempt = 1; attempt <= 3 && !reconnected; attempt++)
        {
          LOGF("[WiFi] Reconnect attempt %d/3...\n", attempt);
          WiFi.disconnect(false);
          WiFi.begin(savedSSID.c_str(), savedPass.c_str());
          unsigned long start = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
          {
            delay(200);
            esp_task_wdt_reset();   // 3x10s here + stacked failing REST calls
                                    // earlier this iteration can otherwise cross
                                    // the 60s task watchdog and reboot mid-switch
            checkRelayTimers();
            handleSerialCommands();
          }
          reconnected = (WiFi.status() == WL_CONNECTED);
          if (!reconnected)
            LOGF("[WiFi] Attempt %d failed\n", attempt);
        }
      }
    }

    if (reconnected)
      LOGLNS("[WiFi] Reconnected: " + WiFi.localIP().toString());
    else
      tryCellularFallback();   // brings up cellular if a modem + APN are present
  }

  // Background WiFi retry while on cellular with no portal (non-blocking).
  retryWifiInBackground(now);

  // Up-switch: on cellular but WiFi has come back and held steady for 15s.
  static unsigned long wifiUpSince = 0;
  if (activeTransport == TRANSPORT_CELLULAR)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      if (wifiUpSince == 0) wifiUpSince = now;
      if (now - wifiUpSince >= 15000)
      {
        LOGLN("[Cellular] WiFi recovered — switching back from cellular");
        modem.gprsDisconnect();
        activeTransport = TRANSPORT_WIFI;
        mqttClient.disconnect();
        mqttClient.setClient(espClient);
        wifiUpSince = 0;
      }
    }
    else
    {
      wifiUpSince = 0;
    }
  }

  // Cellular link health. Two failure modes, both end the same way — drop the
  // transport so the down-switch block + tryCellularFallback() rebuild it (which
  // reboots the modem on retry), or shouldOpenPortalOffline() opens the AP:
  //   1. PDP session reported gone (SIM pulled, carrier dropped us, plan
  //      expired) -> modem.isGprsConnected() == false.
  //   2. "Zombie" session: the network tore the data path down but the modem
  //      still reports the context up, so isGprsConnected() lies. Caught by the
  //      MQTT link (plain TCP to the broker) staying down for minutes while we
  //      believe we are on cellular. Observed on marginal 4G in the R6c soak.
  static unsigned long cellDeadSince = 0;
  static unsigned long lastCellCheck = 0;
  static unsigned long mqttDownSince = 0;   // set below, after mqttClient.loop()
  if (activeTransport == TRANSPORT_CELLULAR && now - lastCellCheck >= 20000)
  {
    lastCellCheck = now;
    bool gprsUp     = modem.isGprsConnected();
    bool mqttZombie = (mqttDownSince != 0 && now - mqttDownSince >= CELL_UPLINK_DEAD_MS);
    if (gprsUp && !mqttZombie)
    {
      cellDeadSince = 0;
    }
    else if (cellDeadSince == 0)
    {
      cellDeadSince = now;
      LOGF("[Cellular] Link unhealthy (gprs=%d mqttDown=%lus) — watching\n",
           gprsUp, mqttDownSince ? (unsigned long)((now - mqttDownSince) / 1000) : 0UL);
    }
    else if (now - cellDeadSince >= 40000)
    {
      LOGLN("[Cellular] Link dead >40s — dropping to WiFi/portal (modem re-attaches on retry)");
      modem.gprsDisconnect();
      activeTransport = TRANSPORT_WIFI;   // modem stays powered; cellularCapable unchanged
      mqttClient.disconnect();
      mqttClient.setClient(espClient);
      cellDeadSince = 0;
      mqttDownSince = 0;
    }
  }

  // --- Become fully online on whatever transport we have (once per boot) ---
  // bringOnline() only sets startupTime on a successful registration, so if the
  // first attempt fails (marginal link at boot) this stays true — rate-limit to
  // 30s so it retries the full onboard instead of spinning it every iteration.
  static unsigned long lastBringOnlineTry = 0;
  if (startupTime == 0 &&
      (WiFi.status() == WL_CONNECTED || activeTransport == TRANSPORT_CELLULAR) &&
      (lastBringOnlineTry == 0 || now - lastBringOnlineTry >= 30000UL))
  {
    lastBringOnlineTry = now;
    if (WiFi.status() == WL_CONNECTED)
      wifiState = STATE_ONLINE;
    bringOnline();
  }

  // Cellular has no SNTP daemon; if the boot-time modem time sync failed the
  // clock stays near epoch 0 for the whole session (stamping 1970 into every
  // row). Retry every 5 min while it still looks unset.
  static unsigned long lastCellTimeRetry = 0;
  if (activeTransport == TRANSPORT_CELLULAR && time(nullptr) < 1600000000UL &&
      (lastCellTimeRetry == 0 || now - lastCellTimeRetry >= 300000UL))
  {
    lastCellTimeRetry = now;
    LOGLN("[Cellular] Clock still unset — retrying modem time sync");
    syncTimeFromModem();
  }

  // --- Open the AP if there's no other way to be useful (cellular already tried) ---
  if (!portalMode && shouldOpenPortalOffline())
  {
    portalAutoOpened = true;
    startWiFiPortal();
  }
  // An auto-opened offline portal has served its purpose once cellular is
  // carrying traffic — close it so we don't broadcast a provisioning AP for the
  // rest of the session (nothing else tears it down on the cellular path).
  else if (portalMode && portalAutoOpened && activeTransport == TRANSPORT_CELLULAR)
  {
    LOGLN("[AP] Cellular fallback active — closing the auto-opened portal");
    stopWiFiPortal();
    portalAutoOpened = false;
  }

  // No uplink at all (cellular fallback didn't take) — skip the periodic work.
  if (WiFi.status() != WL_CONNECTED && activeTransport != TRANSPORT_CELLULAR)
  {
    delay(500);
    return;
  }

  if (!isRegistered)
  {
    delay(1000);
    return;
  }

  // --- Pending WiFi commands (deferred from MQTT callback to avoid re-entrancy) ---
  // pendingWifiPortal is handled earlier by shouldOpenPortal().
  if (pendingWifiForget)
  {
    pendingWifiForget = false;
    wifiPrefs.begin("wifi", false);
    wifiPrefs.clear();
    wifiPrefs.end();
    LOGLN("[WiFi] Credentials forgotten, restarting...");
    delay(500);
    ESP.restart();
  }

  // --- MQTT keepalive ---
  if (!mqttClient.connected())
    reconnectMQTT();
  mqttClient.loop();

  // How long has MQTT been unreachable while we believe we are on cellular? The
  // link-health check above uses this to catch a zombie data session that
  // modem.isGprsConnected() misreports as still up. Clears on any reconnect or
  // transport switch so it never carries a stale WiFi-era outage onto cellular.
  if (mqttClient.connected() || activeTransport != TRANSPORT_CELLULAR)
    mqttDownSince = 0;
  else if (mqttDownSince == 0)
    mqttDownSince = now;

  // --- Pending sensor rescan (deferred from MQTT callback to avoid re-entrancy) ---
  // Checked immediately after mqttClient.loop() (which is what actually sets the flag,
  // via mqttCallback) rather than before it — otherwise a rescan command sits unhandled
  // for a full extra loop() iteration, padded by whatever periodic task is due that tick.
  if (pendingRescan)
  {
    pendingRescan = false;
    bool busy = autoDosing &&
                autoState != AUTO_IDLE &&
                autoState != AUTO_STARTUP_WAIT &&
                autoState != AUTO_SAMPLING;
    if (!busy)
    {
      LOGLN("[Rescan] Running initSensors()");
      initSensors();
      uploadSensorConfig();
      rescanSeq++;
    }
    else
    {
      LOGLN("[Rescan] Ignored — auto-dosing busy");
    }
  }

  // --- Pending auto-dosing alarm reset (deferred from MQTT callback to avoid re-entrancy) ---
  if (pendingAutoDosingReset)
  {
    pendingAutoDosingReset = false;
    if (autoState == AUTO_ALARM)
    {
      LOGLN("[Auto] Reset via dashboard command");
      logDeviceActivity("dosing", "Auto-dosing alarm reset via dashboard");
      autoState = AUTO_IDLE;
      lastAlarmReason = ALARM_REASON_NONE;
    }
    else
    {
      LOGLN("[Auto] Reset command ignored — not in ALARM");
    }
  }

  // --- Periodic tasks ---
  if (now - lastSensorRead >= SENSOR_READ_INTERVAL)
  {
    readSensors();
    lastSensorRead = now;
    checkRefillCutoff();
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

  // --- Tasmota plug state poll (HTTP transport only; inert on MQTT-transport devices) ---
  if (plugUseHttp && now - lastPlugHttpPoll >= PLUG_HTTP_POLL_INTERVAL)
  {
    pollPlugHttpState();
    lastPlugHttpPoll = now;
  }

  if (now - lastOTACheck >= OTA_CHECK_INTERVAL)
  {
    checkForOTAUpdate();
    lastOTACheck = now;
  }

#ifdef ENABLE_OTA_LOGS
  if (now - lastLogPublish >= LOG_PUBLISH_INTERVAL)
  {
    publishPendingLogs();
    lastLogPublish = now;
  }
#endif

  delay(10);
}
