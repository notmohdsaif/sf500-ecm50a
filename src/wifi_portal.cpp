// =====================================================
// WIFI_PORTAL.CPP
// WiFi captive portal and credential storage
// =====================================================

#include "wifi_portal.h"
const char *PUBLIC_URL = "https://iot.redtone.com/sf500/";
// =====================================================
// PORTAL HELPERS
// =====================================================

String escapeHtml(const String &s)
{
  String t = s;
  t.replace("&", "&amp;");
  t.replace("<", "&lt;");
  t.replace(">", "&gt;");
  t.replace("\"", "&quot;");
  t.replace("'", "&#39;");
  return t;
}

void sendHtmlResponse(const String &html)
{
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send(200, "text/html; charset=utf-8", html);
}

void redirectToRoot()
{
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "");
}

void doWiFiScan()
{
  scanList.clear();
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++)
  {
    String s = WiFi.SSID(i);
    if (!s.length())
      continue;
    int32_t r = WiFi.RSSI(i);
    bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    int idx = -1;
    for (size_t k = 0; k < scanList.size(); ++k)
    {
      if (scanList[k].ssid == s)
      {
        idx = (int)k;
        break;
      }
    }
    if (idx < 0)
      scanList.push_back({s, r, sec});
    else if (r > scanList[idx].rssi)
    {
      scanList[idx].rssi = r;
      scanList[idx].secure = sec;
    }
  }
  std::sort(scanList.begin(), scanList.end(),
            [](const NetItem &a, const NetItem &b)
            { return a.rssi > b.rssi; });
}

String buildPortalPage()
{
  String opts = "<option value=\"\">-- Select SSID --</option>";
  for (auto &it : scanList)
    opts += "<option value=\"" + escapeHtml(it.ssid) + "\">" + escapeHtml(it.ssid) + "</option>";

  String h;
  h.reserve(4000);
  h += F(
      "<!doctype html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>SF-500 Setup</title>"
      "<style>"
      "*,*:before,*:after{box-sizing:border-box}"
      "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:20px;background:#f5f5f5}"
      ".card{max-width:380px;margin:auto;border:1px solid #ccc;border-radius:12px;padding:14px;box-shadow:0 2px 6px rgba(0,0,0,.1);background:#fff}"
      "h3{color:#333;margin:4px 0 10px}"
      "label{font-size:14px;color:#333;margin-top:6px;display:block}"
      "input[type=text],input[type=password]{"
      "height:44px;line-height:44px;padding:0 12px;font-size:16px;width:100%;"
      "margin:6px 0;border-radius:8px;border:1px solid #ccc;"
      "-webkit-appearance:none;appearance:none;outline:none}"
      "input[type=text]:focus,input[type=password]:focus{"
      "border-color:#0a84ff;box-shadow:0 0 0 2px rgba(10,132,255,.25)}"
      "select,button{height:44px;padding:0 12px;font-size:16px;width:100%;"
      "margin:6px 0;border-radius:8px;border:1px solid #ccc}"
      "button{background:#0a84ff;color:#fff;border-color:#0a84ff;cursor:pointer}"
      "button:hover{background:#0070e0}"
      "button.secondary{background:#eee;color:#111;border-color:#ddd}"
      "button.secondary:hover{background:#ddd}"
      ".row{display:flex;gap:8px}.row>*{flex:1}"
      ".toggle{display:flex;align-items:center;gap:8px;margin:4px 0 2px;color:#444;font-size:13px}"
      ".toggle input{width:1.1rem;height:1.1rem;margin:0;border:none;padding:0}"
      ".info{font-size:12px;color:#666;margin-top:10px;text-align:center}"
      "</style></head><body>"
      "<div class='card'><h3>SF-500 WiFi Setup</h3>"
      "<p class='info'>Device: ");
  h += deviceName;
  h += F(
      "</p>"
      "<label>Nearby networks</label>"
      "<select id='ssidSel' onchange=\"document.getElementById('ssid').value=this.value;\">");
  h += opts;
  h += F(
      "</select>"
      "<form action='/save' method='POST' autocomplete='on'>"
      "<label>SSID</label><input type='text' id='ssid' name='ssid' required autofocus>"
      "<label>Password</label><input id='pass' name='pass' type='password' placeholder='(blank if open)'>"
      "<label class='toggle'><input id='showPW' type='checkbox' "
      "onclick=\"document.getElementById('pass').type=this.checked?'text':'password'\">"
      "<span>Show password</span></label>"
      "<div class='row'>"
      "<button type='submit'>Connect</button>"
      "<button class='secondary' type='button' onclick=\"location.href='/rescan'\">Rescan</button>"
      "</div>"
      "</form>"
      "</div></body></html>");
  return h;
}

void setupCaptiveRoutes()
{
  // Use absolute 302 redirect — relative URLs break when the OS browser
  // resolves the path against the captive-check domain (e.g. gstatic.com).
  auto toRoot = []()
  { redirectToRoot(); };
  portalServer.on("/hotspot-detect.html", HTTP_GET, toRoot);
  portalServer.on("/generate_204", HTTP_GET, toRoot);
  portalServer.on("/ncsi.txt", HTTP_GET, toRoot);
  portalServer.on("/connecttest.txt", HTTP_GET, toRoot);
  portalServer.on("/canonical.html", HTTP_GET, toRoot);
  portalServer.on("/success.txt", HTTP_GET, toRoot);
}

// =====================================================
// PORTAL MAIN
// =====================================================

void startWiFiPortal()
{
  wifiState = STATE_PORTAL;
  portalMode = true;
  portalConnectStartMs = 0;

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(50);

  String apSsid = "sf500-" + lastSix;
  bool ok = WiFi.softAP(apSsid.c_str(), AP_PASS, AP_CH, 0, 4);
  Serial.printf("[AP] ok=%d ssid=%s pass=%s ch=%d ip=%s\n",
                ok, apSsid.c_str(), AP_PASS, AP_CH,
                WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  portalServer.on("/", HTTP_GET, []()
                  {
    if (scanList.empty()) doWiFiScan();
    sendHtmlResponse(buildPortalPage()); });

  portalServer.on("/rescan", HTTP_GET, []()
                  {
    doWiFiScan();
    redirectToRoot(); });

  portalServer.on("/save", HTTP_POST, []()
                  {
    String ssid = portalServer.arg("ssid");
    String pass = portalServer.arg("pass");
    if (!ssid.length())
    {
      portalServer.send(400, "text/plain; charset=utf-8", "SSID required");
      return;
    }

    wifiPrefs.begin("wifi", false);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
    wifiPrefs.end();

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifiState = STATE_CONNECTING;
    portalConnectStartMs = millis();
    Serial.printf("[STA] Connecting to '%s'...\n", ssid.c_str());

    // Use meta-refresh — JS is often blocked in captive portal browsers
    sendHtmlResponse(
      "<!doctype html><meta charset='utf-8'>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='2; url=http://192.168.4.1/status'>"
      "<style>body{font-family:sans-serif;text-align:center;padding:40px 20px}"
      "p{color:#555}</style>"
      "<p>Connecting to <b>" + escapeHtml(ssid) + "</b>...</p>"
      "<p><small>You will be redirected automatically.</small></p>"); });

  portalServer.on("/status", HTTP_GET, []()
                  {
    if (wifiState == STATE_ONLINE && WiFi.status() == WL_CONNECTED)
    {
      // Send the success page — AP teardown is handled by handlePortalLoop()
      // after a short delay so the browser fully receives this response first.
      sendHtmlResponse(
        "<!doctype html><meta charset='utf-8'>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;text-align:center;padding:40px 20px;background:#f5f5f5}"
        ".card{max-width:320px;margin:auto;background:#fff;border-radius:12px;padding:24px;"
        "box-shadow:0 2px 8px rgba(0,0,0,.1)}"
        "h3{color:#1a7f3c;margin:0 0 12px}.ip{font-size:1.1em;font-weight:bold}"
        "p{color:#555;font-size:14px;margin:8px 0}</style>"
        "<div class='card'>"
        "<h3>&#10003; Connected!</h3>"
        "<p>Network: <span class='ip'>" + WiFi.SSID() + "</span></p>"
        "<p>Device IP: <span class='ip'>" + WiFi.localIP().toString() + "</span></p>"
        "<p>The device is now online.</p>"
        "<a class='btn' href='" + String(PUBLIC_URL) + "'>Go to Dashboard</a>"
        "<p><small>If it doesn’t open, close this page and tap the button again after your phone switches back to WiFi.</small></p>"
        "</div>");
    }
    else if (wifiState == STATE_PORTAL)
    {
      // Connection timed out — send user back to the main page to retry
      redirectToRoot();
    }
    else
    {
      // STATE_CONNECTING — keep polling with meta-refresh (no JS needed)
      sendHtmlResponse(
        "<!doctype html><meta charset='utf-8'>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='2; url=http://192.168.4.1/status'>"
        "<style>body{font-family:sans-serif;text-align:center;padding:40px 20px}"
        "p{color:#555}</style>"
        "<p>Connecting... please wait.</p>");
    } });

  setupCaptiveRoutes();
  portalServer.onNotFound([]()
                          { redirectToRoot(); });
  portalServer.begin();

  Serial.println("[Portal] Connect to: " + apSsid + " / " + String(AP_PASS));
}

void handlePortalLoop()
{
  static unsigned long lastDebug = 0;
  static unsigned long apCloseAt = 0;

  // Update WiFi state FIRST so handleClient() serves the correct page
  if (wifiState == STATE_CONNECTING)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      wifiState = STATE_ONLINE;
      apCloseAt = millis() + 5000; // give browser 5 s to load "Connected!" page
      Serial.printf("[STA] Connected to '%s', IP=%s — AP closing in 5s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    else if (portalConnectStartMs > 0 &&
             millis() - portalConnectStartMs > CONNECT_TIMEOUT_MS)
    {
      Serial.println("[STA] Timeout -> back to portal");
      WiFi.disconnect(true, false);
      wifiState = STATE_PORTAL;
      portalConnectStartMs = 0;
      apCloseAt = 0;
    }
  }

  // Serve HTTP/DNS with up-to-date state
  dnsServer.processNextRequest();
  portalServer.handleClient();

  if (millis() - lastDebug > 5000)
  {
    lastDebug = millis();
    Serial.printf("[AP] Portal active, clients=%d, state=%d\n",
                  WiFi.softAPgetStationNum(), (int)wifiState);
  }

  // Auto-close AP after timer — then signal main loop to exit portal mode
  if (apCloseAt > 0 && millis() >= apCloseAt)
  {
    apCloseAt = 0;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portalMode = false; // main loop exits portal mode after this
    Serial.println("[AP] Closed — STA mode only");
  }
}
