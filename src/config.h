#pragma once

// =====================================================
// CONFIG.H — All compile-time constants
// =====================================================

// IMPORTANT:
// Secrets (AP password / Supabase key) are defined in `config_secrets.h`,
// which is intentionally NOT committed to git.
#include "config_secrets.h"

// WiFi Portal Settings
#define AP_CH                    6
#define DNS_PORT                 53
#define PORTAL_DOMAIN            "sfconnect.com"
#define PORTAL_ROOT              "http://sfconnect.com/"
#define CONNECT_TIMEOUT_MS       30000UL
#define AUTO_CONNECT_TIMEOUT_MS  15000UL

// Supabase
#define SUPABASE_URL "https://qkqeysggrqhxizkdmbhx.supabase.co"

// MQTT
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT   1883

// Hardware Pins (ECM50-A)
#define RX_PIN     18   // RS485 UART1 RX
#define TX_PIN     17   // RS485 UART1 TX
#define RELAY1_PIN 15   // DO1 - Dosing Pumps A+B
#define RELAY2_PIN 16   // DO2 - Mixing Pump

// Sensor Configuration
#define EC_SENSOR_ID  3    // Fixed Modbus ID
#define WL_SCAN_START 10   // Water level scan range start
#define WL_SCAN_END   19   // Water level scan range end

// Timing Constants (milliseconds)
#define SENSOR_READ_INTERVAL    1000UL
#define SENSOR_UPLOAD_INTERVAL  (5UL * 60UL * 1000UL)  // 5 minutes
#define STATUS_UPDATE_INTERVAL  30000UL
#define CONFIG_CHECK_INTERVAL   10000UL
#define SCHEDULE_CHECK_INTERVAL 1000UL
#define SCHEDULE_FETCH_INTERVAL 60000UL

// EC Automation
#define EC_SAMPLES      30
#define EC_HYSTERESIS   0.05f
#define INITIAL_WAIT    60000UL
#define POST_DOSE_DELAY 60000UL

// Schedules
#define MAX_SCHEDULES 100
