// =====================================================
// CELLULAR.CPP
// Onboard Quectel EC801E-CN 4G modem — hardware detection.
// Data connection + TLS transport are added in later phases.
// =====================================================

#include "cellular.h"
#include "config.h"
#include "logger.h"

HardwareSerial modemSerial(2); // UART2 — modem only

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
