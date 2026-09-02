#pragma once
#include <Arduino.h>

// UART2 to the onboard Quectel EC801E-CN modem. Nothing else on this board
// uses UART2 (RS485/Modbus is on Serial1), so this is exclusively the modem's.
extern HardwareSerial modemSerial;

// Powers the modem (MODEM_PWR_PIN HIGH), opens UART2, and probes with AT.
// Safe to call on boards without the modem populated — the probes just time
// out and it returns false (after ~11s). Detection is lazy: this is only
// called the first time WiFi fails, never during a normal boot, so non-4G
// units never incur the cost.
bool detectCellularModem();
