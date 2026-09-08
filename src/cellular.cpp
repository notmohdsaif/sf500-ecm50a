// =====================================================
// CELLULAR.CPP
// Onboard Quectel EC801E-CN 4G modem — detection, data session,
// and the software-TLS Supabase request path used on cellular fallback.
// =====================================================

#include "cellular.h"
#include "config.h"
#include "logger.h"
#include "cellular_cert.h"
#include <SSLClient.h>
#include <ArduinoHttpClient.h>
#include <esp_task_wdt.h>

HardwareSerial modemSerial(2); // UART2 — modem only
TinyGsm        modem(modemSerial);
TinyGsmClient  cellularClient(modem, 0);   // mux 0 — carries MQTT

// mux 1 — raw TCP for HTTPS, wrapped by the software-TLS client below. A
// SEPARATE mux from MQTT so a Supabase call doesn't tear down the MQTT socket.
static TinyGsmClient cellularHttpsRaw(modem, 1);

// Software TLS (ESP32 mbedTLS) over the modem's raw TCP — the modem's own
// AT+QSSL commands are unsupported on this firmware build. File-scope: only
// cellularSupabaseRequest() below uses it.
static SSLClient cellularSecureClient(&cellularHttpsRaw);

// Sends one AT command, returns true on "OK", false on "ERROR" or timeout.
static bool sendModemAT(const char *cmd, unsigned long timeoutMs = 2000)
{
  while (modemSerial.available())
    modemSerial.read();
  modemSerial.print(cmd);
  modemSerial.print("\r\n");

  String resp;
  unsigned long start = millis();
  while (millis() - start < timeoutMs)
  {
    while (modemSerial.available())
      resp += (char)modemSerial.read();
    if (resp.indexOf("OK") >= 0)
      return true;
    if (resp.indexOf("ERROR") >= 0)
      return false;
    delay(20);
  }
  return false;
}

bool detectCellularModem()
{
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(5000); // modem cold-boot to AT-ready — proven necessary during the spike

  for (int i = 0; i < 3; i++)
    if (sendModemAT("AT"))
      return true;

  digitalWrite(MODEM_PWR_PIN, LOW); // no modem present — don't leave the pin driven
  return false;
}

// One data-session attempt against the modem's current state. Fast-fails on a
// not-ready SIM (absent / PIN-locked) so we don't sit 30s in waitForNetwork().
static bool bringUpDataSession(const char *apn, uint32_t netTimeoutMs)
{
  SimStatus sim = modem.getSimStatus();
  if (sim != SIM_READY)
  {
    LOGF("[Cellular] SIM not ready (status %d)\n", (int)sim);
    return false;
  }
  if (!modem.waitForNetwork(netTimeoutMs))
  {
    LOGLN("[Cellular] no network registration");
    return false;
  }
  if (!modem.gprsConnect(apn, "", ""))
  {
    LOGLN("[Cellular] GPRS/PDP attach failed");
    return false;
  }
  return true;
}

bool connectCellularData(const char *apn)
{
  LOGF("[Cellular] Bringing up data session (APN=%s)...\n", apn);

  // modem.init() / waitForNetwork / gprsConnect (QIACT waits up to 150s
  // internally) / modem.restart() each block far past the 60s task watchdog
  // and cannot be fed mid-call. Drop off the WDT for the duration and re-arm
  // on every exit — an AT-level hang still bottoms out on the modem's own
  // response timeouts, so this can't wedge the loop indefinitely.
  esp_task_wdt_delete(NULL);

  esp_task_wdt_reset();
  modem.init();
  bool ok = bringUpDataSession(apn, 30000);

  // Retry once behind a full modem reboot. AT+CFUN=1,1 re-scans the SIM
  // (needed after a hot-swap — modem.init() alone won't notice a re-inserted
  // card) and recovers from AT+CFUN=0. Rate-limited so a permanently SIM-less
  // unit doesn't reboot the modem on every 60s retry.
  if (!ok)
  {
    static unsigned long lastRestart = 0;
    unsigned long nowMs = millis();
    if (lastRestart == 0 || nowMs - lastRestart >= 120000UL)
    {
      lastRestart = nowMs;
      LOGLN("[Cellular] Rebooting modem and retrying...");
      esp_task_wdt_reset();
      modem.restart();
      esp_task_wdt_reset();
      ok = bringUpDataSession(apn, 20000);
    }
  }

  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  if (!ok)
  {
    LOGLN("[Cellular] FAIL: no data session");
    return false;
  }

  cellularSecureClient.setCACert(CELLULAR_CA_CERT);
  cellularSecureClient.setTimeout(15000);   // bound the software-TLS handshake + socket reads
  LOGF("[Cellular] Data connected, IP=%s\n", modem.localIP().toString().c_str());
  return true;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian). Howard Hinnant's
// algorithm — avoids timegm(), which ESP32 newlib doesn't provide.
static long daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= (m <= 2);
  long     era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool syncTimeFromModem()
{
  int   y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  float tz = 0.0f;

  // AT+QLTS pulls the time the network already pushed via NITZ (AT+CTZU=1 is
  // set during modem init). If the carrier didn't send NITZ, fall back to an
  // explicit NTP sync over the data session, then read it back.
  bool ok = modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz);
  if (!ok || y < 2024)
  {
    LOGLN("[Cellular] No network time yet — trying AT+QNTP...");
    modem.NTPServerSync("pool.ntp.org");
    delay(1000);
    ok = modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz);
  }

  if (!ok || y < 2024 || y > 2100)
  {
    LOGLN("[Cellular] Modem time sync failed");
    return false;
  }

  // getNetworkTime() returns LOCAL wall-clock + tz offset (hours). The rest of
  // the firmware expects time(nullptr) to be UTC epoch (configTime does the
  // same), so back the offset out.
  long   epochLocal = daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s;
  time_t utc        = (time_t)(epochLocal - (long)lround(tz * 3600.0));

  struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  // The WiFi path's configTime() also sets the TZ so localtime_r() yields
  // UTC+8 (the firmware stamps a hardcoded +08:00 everywhere). settimeofday()
  // alone doesn't, so do it explicitly. POSIX sign is inverted: UTC-8 == +8h.
  setenv("TZ", "UTC-8", 1);
  tzset();

  LOGF("[Cellular] Time set from modem: %04d-%02d-%02d %02d:%02d:%02d UTC%+.1f\n",
       y, mo, d, h, mi, s, tz);
  return true;
}

void setModemRadio(bool on)
{
  if (!on)
    modem.gprsDisconnect();
  modem.sendAT(on ? "+CFUN=1" : "+CFUN=0");
  modem.waitResponse(10000L);
  LOGF("[Cellular] Modem radio %s\n", on ? "ON" : "OFF");
}

int cellularSupabaseRequest(const char *method, const String &url,
                            const String &reqBody, const char *contentType,
                            const char *prefer, String &outBody)
{
  outBody = "";

  // Split "https://host/path" into host + path.
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0)
    return HTTP_ERROR_API;
  int hostStart = schemeEnd + 3;
  int pathStart = url.indexOf('/', hostStart);
  String host = (pathStart < 0) ? url.substring(hostStart)
                                : url.substring(hostStart, pathStart);
  String path = (pathStart < 0) ? String("/") : url.substring(pathStart);

  HttpClient http(cellularSecureClient, host, 443);
  http.setHttpResponseTimeout(15000);

  esp_task_wdt_reset();   // the TLS handshake in startRequest() can take seconds
  http.beginRequest();
  int ret = http.startRequest(path.c_str(), method, contentType,
                              reqBody.length() > 0 ? (int)reqBody.length() : -1,
                              nullptr);
  esp_task_wdt_reset();
  if (ret != HTTP_SUCCESS)
  {
    http.stop();
    return ret;
  }
  http.sendHeader("apikey", SUPABASE_KEY);
  http.sendHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  if (prefer)
    http.sendHeader("Prefer", prefer);
  http.endRequest();
  if (reqBody.length() > 0)
    http.print(reqBody);

  int code = http.responseStatusCode();
  outBody = http.responseBody();
  http.stop();
  return code;
}
