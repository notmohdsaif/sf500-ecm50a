#pragma once

// =====================================================
// CONFIG.H — All compile-time constants
// =====================================================

// IMPORTANT:
// Secrets (AP password / Supabase key) are defined in `config_secrets.h`,
// which is intentionally NOT committed to git.
#include "config_secrets.h"

// WiFi Portal Settings
#define AP_CH 6
#define DNS_PORT 53
#define PORTAL_DOMAIN "sfconnect.com"
#define PORTAL_ROOT "http://sfconnect.com/"
#define CONNECT_TIMEOUT_MS 30000UL
#define AUTO_CONNECT_TIMEOUT_MS 15000UL
#define PORTAL_SAVED_RETRY_INTERVAL_MS (5UL * 60UL * 1000UL) // retry saved creds every 5 min while portal is open

// Supabase
#define SUPABASE_URL "https://qkqeysggrqhxizkdmbhx.supabase.co"

// MQTT
#define MQTT_BROKER "broker.emqx.io"
#define MQTT_PORT 1883

// Hardware Pins (ECM50-A)
#define RX_PIN 18     // RS485 UART1 RX
#define TX_PIN 17     // RS485 UART1 TX
#define RELAY1_PIN 15 // DO1 - Dosing Pumps A+B
#define RELAY2_PIN 16 // DO2 - Mixing Pump

// Sensor Configuration — ID ranges match admin panel SENSOR_TYPES
#define EC_SENSOR_DEFAULT 3 // Try this ID first before range scan
#define EC_SCAN_START 3     // EC sensor scan range fallback
#define EC_SCAN_END 5
#define WL_SCAN_START 13 // Water level scan range (IDs 13–15)
#define WL_SCAN_END 15
#define AMB_SCAN_START 20 // Ambient sensor scan range (IDs 20–22)
#define AMB_SCAN_END 22
#define AMB_REG_HUMID 500 // Holding register: humidity
#define AMB_REG_TEMP  501 // Holding register: temperature
#define AMB_REG_LUX   507 // Holding register: lux
#define RAIN_SCAN_START 30 // Rain sensor (single fixed ID)
#define RAIN_SCAN_END 30
#define RAIN_REG_TIPS 0    // Holding register: rainfall in 0.1mm increments

// Timing Constants (milliseconds)
#define SENSOR_READ_INTERVAL 1000UL
#define SENSOR_UPLOAD_INTERVAL (5UL * 60UL * 1000UL) // 5 minutes
#define STATUS_UPDATE_INTERVAL 30000UL
#define CONFIG_CHECK_INTERVAL 10000UL
#define SCHEDULE_CHECK_INTERVAL 1000UL
#define SCHEDULE_FETCH_INTERVAL 60000UL

// EC Automation — thresholds
#define EC_SAMPLES 30
#define EC_HYSTERESIS 0.10f
#define EC_CEILING_MARGIN 0.30f
#define DOSE_RESPONSE_THRESHOLD 0.02f
#define MAX_INEFFECTIVE_DOSES 3

// Smart Dosing
#define SMART_CAL_DURATION 60        // calibration dose length (seconds) — floor only; scales with dosingTime
#define SMART_MIN_DOSE 5             // minimum computed dose (seconds)
#define SMART_MAX_DOSE 300           // maximum computed dose cap (seconds)
#define SMART_ERROR_THRESHOLD 0.30f  // re-calibrate if prediction error > 30%
#define DOSE_ABORT_MARGIN 0.05f      // abort active dose if EC exceeds target by this margin (mS/cm)
#define SMART_WL_FACTOR_MAX 1.5f     // cap upward WL correction factor (prevents over-extension at high WL)
#define SMART_CAL_MAX_RETRIES 5      // max consecutive calibration failures before alarm

// EC Automation — timing (milliseconds)
#define INITIAL_WAIT 60000UL
#define PRE_MIX_DURATION 15000UL
#define POST_MIX_DURATION 30000UL
#define POST_DOSE_DELAY_NO_MIX 120000UL
#define POST_DOSE_DELAY_MIX 60000UL
#define STABILISE_SKIP_NO_MIX 15
#define STABILISE_SKIP_MIX 10

// Schedules
#define MAX_SCHEDULES 100

// Firmware version — must match GitHub release tag (without 'v' prefix)
#define FIRMWARE_VERSION "1.1.1"

// GitHub OTA repository
#define GITHUB_USER "notmohdsaif"
#define GITHUB_REPO "sf500-ecm50a"

// OTA check interval
#define OTA_CHECK_INTERVAL (6UL * 60UL * 60UL * 1000UL)

// Remote serial logging over MQTT — comment out to disable entirely
#define ENABLE_OTA_LOGS
#define LOG_PUBLISH_INTERVAL 5000UL
