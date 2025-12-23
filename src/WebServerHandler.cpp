#include "WebServerHandler.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "www.h"
#include "WebSerial.h"

extern Settings settings;
extern WiFiManager wifiManager;

// -------------------- Non-blocking WiFi scan cache --------------------
namespace NetScanCache
{
  static bool scanRunning = false;
  static uint32_t cacheTs = 0;
  static String cacheJson;
  static const uint32_t CACHE_MS = 10000;

  static bool cacheValid()
  {
    if (cacheTs == 0) return false;
    return (millis() - cacheTs) < CACHE_MS && cacheJson.length() > 0;
  }

  static void startAsyncScanIfNeeded(bool force)
  {
    if (!force && cacheValid()) return;

    int sc = WiFi.scanComplete();
    if (sc == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }

    // If results available (>=0), let collectIfFinished() harvest them.
    // Otherwise start a new async scan.
    if (sc < 0) {
      int rc = WiFi.scanNetworks(true /* async */, true /* show hidden */);
      scanRunning = (rc == WIFI_SCAN_RUNNING);
    } else {
      scanRunning = false;
    }
  }

  static void collectIfFinished()
  {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      scanRunning = true;
      return;
    }
    if (n < 0) {
      scanRunning = false;
      return;
    }

    scanRunning = false;

    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"]  = WiFi.SSID(i);
      o["rssi"]  = WiFi.RSSI(i);
      o["enc"]   = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      o["bssid"] = WiFi.BSSIDstr(i);
    }

    WiFi.scanDelete();

    cacheJson = "";
    serializeJson(doc, cacheJson);
    cacheTs = millis();
  }

  static const String& json()
  {
    return cacheJson;
  }
} // namespace NetScanCache

// -------------------- Restart scheduling (no delay in handlers) --------------------
static void bb_restart_cb(void* arg)
{
  (void)arg;
  ESP.restart();
}

static void scheduleRestart(uint32_t delayMs)
{
  esp_timer_handle_t t = nullptr;
  esp_timer_create_args_t args = {};
  args.callback = &bb_restart_cb;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "bb_restart";

  if (esp_timer_create(&args, &t) == ESP_OK && t) {
    esp_timer_start_once(t, (uint64_t)delayMs * 1000ULL);
  } else {
    ESP.restart();
  }
}

WebServerHandler::WebServerHandler(AsyncWebServer& s) : server(s) {}

bool WebServerHandler::isAuthorized(AsyncWebServerRequest* req) {
  // If user is empty => no auth
  if (!settings.get.webUIuser() || !*settings.get.webUIuser()) return true;
  return req->authenticate(settings.get.webUIuser(), settings.get.webUIPass());
}

void WebServerHandler::sendGz(AsyncWebServerRequest* req, const uint8_t* data, size_t len, const char* mime) {
  AsyncWebServerResponse* r = req->beginResponse(200, mime, data, len);
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-store");
  req->send(r);
}

void WebServerHandler::handleNetlist(AsyncWebServerRequest* req) {
  // Never run synchronous WiFi scans inside AsyncTCP handlers.
  // Trigger async scan and return cached results immediately.
  NetScanCache::startAsyncScanIfNeeded(false);
  NetScanCache::collectIfFinished();

  if (NetScanCache::cacheValid()) {
    req->send(200, "application/json", NetScanCache::json());
    return;
  }

  // No cache yet -> respond with empty list (HTML remains unchanged).
  req->send(200, "application/json", "{\"networks\":[]}");
}

void WebServerHandler::handleSubmitConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  settings.set.deviceName(getP("devicename"));

  settings.set.wifiSsid0(getP("ssid0"));
  settings.set.wifiPass0(getP("password0"));
  settings.set.wifiBssid0(getP("bssid0"));

  // FIX: ssid1 must go to wifiSsid1 (was wrongly written to wifiSsid0)
  settings.set.wifiSsid1(getP("ssid1"));
  settings.set.wifiPass1(getP("password1"));

  settings.set.staticIP(getP("ip"));
  settings.set.staticSN(getP("subnet"));
  settings.set.staticGW(getP("gateway"));
  settings.set.staticDNS(getP("dns"));

  settings.set.webUIuser(getP("webUser"));
  settings.set.webUIPass(getP("webPass"));

  settings.save();

  req->send(200, "application/json", "{\"success\":true}");

  // Do not block inside async request handlers
  scheduleRestart(600);
}

void WebServerHandler::begin() {
  // Root
  server.on("/", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, Status_html_gz, Status_html_gz_len, Status_html_gz_mime);
  });

  // WiFi setup should always be reachable in AP mode without login
  server.on("/wifisetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    // Start scan aggressively when entering setup page
    NetScanCache::startAsyncScanIfNeeded(true);
    sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
  });

  // webserial
  server.on("/webserial", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();
    sendGz(req, WebSerial_html_gz, WebSerial_html_gz_len, WebSerial_html_gz_mime);
  });

  server.on("/style.css", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, Style_css_gz, Style_css_gz_len, Style_css_gz_mime);
  });

  server.on("/backgroundCanvas.js", HTTP_GET, [&](AsyncWebServerRequest* req) {
    sendGz(req, backgroundCanvas_js_gz, backgroundCanvas_js_gz_len, backgroundCanvas_js_gz_mime);
  });

  server.on("/netlist", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleNetlist(req);
  });

  server.on("/submitConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitConfig(req);
  });

  server.on("/netconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["ssid0"] = settings.get.wifiSsid0();
    doc["pass0"] = settings.get.wifiPass0();
    doc["bssid0"] = settings.get.wifiBssid0();
    doc["ssid1"] = settings.get.wifiSsid1();
    doc["pass1"] = settings.get.wifiPass1();
    doc["ip"] = settings.get.staticIP();
    doc["subnet"] = settings.get.staticSN();
    doc["gateway"] = settings.get.staticGW();
    doc["dns"] = settings.get.staticDNS();
    doc["webUser"] = settings.get.webUIuser();
    // FIX: return the password, not the user name
    doc["webPass"] = settings.get.webUIPass();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/info.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!isAuthorized(req)) return req->requestAuthentication();

    JsonDocument doc;
    doc["deviceName"] = settings.get.deviceName();
    doc["mode"] = wifiManager.isApMode() ? "AP" : "STA";
    doc["ip"] = wifiManager.isApMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.onNotFound([&](AsyncWebServerRequest* req) {
    // Nice fallback: if in AP mode, redirect everything to setup page
    if (wifiManager.isApMode()) {
      req->redirect("/wifisetup");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  webSerial.println("[WEB] Server started");
}
