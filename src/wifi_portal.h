#pragma once
#include "globals.h"

// =====================================================
// WIFI_PORTAL.H
// WiFi captive portal and credential storage
// =====================================================

void startWiFiPortal();
void handlePortalLoop();

// Internal helpers (used within wifi_portal.cpp)
void   doWiFiScan();
String buildPortalPage();
String escapeHtml(const String &s);
void   sendHtmlResponse(const String &html);
void   redirectToRoot();
void   setupCaptiveRoutes();
