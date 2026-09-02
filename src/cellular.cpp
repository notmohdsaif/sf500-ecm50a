// =====================================================
// CELLULAR.CPP
// Onboard Quectel EC801E-CN 4G modem — detection, data session,
// and the software-TLS Supabase request path used on cellular fallback.
// =====================================================

#include "cellular.h"
#include "config.h"
#include "logger.h"
#include <SSLClient.h>
#include <ArduinoHttpClient.h>

HardwareSerial modemSerial(2); // UART2 — modem only
TinyGsm        modem(modemSerial);
TinyGsmClient  cellularClient(modem);

// Software TLS (ESP32 mbedTLS) over the modem's raw TCP — the modem's own
// AT+QSSL commands are unsupported on this firmware build. File-scope: only
// cellularSupabaseRequest() below uses it.
static SSLClient cellularSecureClient(&cellularClient);

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

bool connectCellularData(const char *apn)
{
  LOGF("[Cellular] Bringing up data session (APN=%s)...\n", apn);

  modem.init();

  if (!modem.waitForNetwork(30000))
  {
    LOGLN("[Cellular] FAIL: no network registration");
    return false;
  }

  if (!modem.gprsConnect(apn, "", ""))
  {
    LOGLN("[Cellular] FAIL: GPRS/PDP attach failed");
    return false;
  }

  cellularSecureClient.setInsecure(); // TODO Task 5.1: replace with setCACert()

  LOGF("[Cellular] Data connected, IP=%s\n", modem.localIP().toString().c_str());
  return true;
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

  http.beginRequest();
  int ret = http.startRequest(path.c_str(), method, contentType,
                              reqBody.length() > 0 ? (int)reqBody.length() : -1,
                              nullptr);
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
