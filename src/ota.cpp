// =====================================================
// OTA.CPP
// GitHub Releases OTA update check and flash
// =====================================================

#include "ota.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include "cloud.h"
#include "logger.h"

// =====================================================
// MARK APP VALID
// Call once after successful boot to cancel rollback.
// If new firmware crashes before this is called, ESP32
// automatically reverts to the previous partition on
// the next reboot.
// =====================================================

void markAppValid()
{
  esp_ota_mark_app_valid_cancel_rollback();
  LOGLN("[OTA] App marked valid");
}

// =====================================================
// CHECK FOR OTA UPDATE
// Queries GitHub API for the latest release, compares
// tag against FIRMWARE_VERSION, downloads and flashes
// if a newer version is found. Device reboots on success.
// =====================================================

void checkForOTAUpdate()
{
  if (WiFi.status() != WL_CONNECTED) return;

  LOGLN("[OTA] Checking — current: v" FIRMWARE_VERSION);

  // --- Step 1: Query GitHub releases/latest ---
  WiFiClientSecure apiClient;
  apiClient.setInsecure();

  HTTPClient http;
  String apiUrl = "https://api.github.com/repos/"
                  GITHUB_USER "/" GITHUB_REPO "/releases/latest";

  if (!http.begin(apiClient, apiUrl))
  {
    LOGLN("[OTA] Cannot connect to GitHub API");
    return;
  }

  http.addHeader("User-Agent", "ESP32-SF500");
  http.addHeader("Accept",     "application/vnd.github.v3+json");
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200)
  {
    LOGF("[OTA] GitHub API returned %d\n", code);
    http.end();
    return;
  }

  // --- Step 2: Parse JSON (stream to avoid large heap copy) ---
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err)
  {
    LOGF("[OTA] JSON error: %s\n", err.c_str());
    return;
  }

  String latestTag = doc["tag_name"].as<String>();
  if (latestTag.startsWith("v")) latestTag = latestTag.substring(1);

  LOGLNS("[OTA] Latest: v" + latestTag);

  if (latestTag == FIRMWARE_VERSION)
  {
    LOGLN("[OTA] Up to date");
    return;
  }

  // --- Step 3: Find .bin asset URL ---
  String downloadUrl;
  for (JsonObject asset : doc["assets"].as<JsonArray>())
  {
    if (String(asset["name"].as<String>()).endsWith(".bin"))
    {
      downloadUrl = asset["browser_download_url"].as<String>();
      break;
    }
  }

  if (downloadUrl.isEmpty())
  {
    LOGLNS("[OTA] No .bin asset in release v" + latestTag);
    return;
  }

  LOGLNS("[OTA] Updating to v" + latestTag);

  String logMsg = "OTA update: v" FIRMWARE_VERSION " -> v" + latestTag;
  logDeviceActivity("system", logMsg.c_str());

  // --- Step 4: Download and flash ---
  WiFiClientSecure otaClient;
  otaClient.setInsecure();

  httpUpdate.setLedPin(-1);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return result = httpUpdate.update(otaClient, downloadUrl);

  switch (result)
  {
    case HTTP_UPDATE_OK:
      LOGLN("[OTA] Success — rebooting");
      break;
    case HTTP_UPDATE_FAILED:
      LOGF("[OTA] Failed (%d): %s\n",
           httpUpdate.getLastError(),
           httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      LOGLN("[OTA] Server reports no update");
      break;
  }
}
