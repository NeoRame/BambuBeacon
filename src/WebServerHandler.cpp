#include "WebServerHandler.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SettingsPrefs.h"
#include "WiFiManager.h"
#include "www.h"
#include "WebSerial.h"
#include "bblPrinterDiscovery.h"
#include "BambuMqttClient.h"
#include "LedController.h"

extern Settings settings;
extern WiFiManager wifiManager;
extern BBLPrinterDiscovery printerDiscovery;
extern BambuMqttClient bambu;
extern LedController ledsCtrl;

static const char* kOtaReleaseUrl = "https://api.github.com/repos/softwarecrash/BambuBeacon/releases/latest";
static void scheduleRestart(uint32_t delayMs);

struct OtaState {
  bool busy = false;
  bool pendingCheck = false;
  bool pendingUpdate = false;
  bool lastCheckOk = false;
  bool updateAvailable = false;
  bool lastUpdateOk = false;
  String current;
  String latest;
  String url;
  String error;
  uint32_t lastCheckMs = 0;
};

static OtaState otaState;
static SemaphoreHandle_t otaMutex = nullptr;
static TaskHandle_t otaTaskHandle = nullptr;

static String trimTagVersion(String tag)
{
  tag.trim();
  if (tag.startsWith("v") || tag.startsWith("V")) {
    tag.remove(0, 1);
  }
  return tag;
}

static void parseVersion(const String& v, int* parts, size_t count)
{
  for (size_t i = 0; i < count; i++) parts[i] = 0;
  size_t idx = 0;
  String token;
  for (size_t i = 0; i <= v.length(); i++) {
    char c = (i < v.length()) ? v[i] : '.';
    if (c == '.' || i == v.length()) {
      if (idx < count) {
        parts[idx] = token.toInt();
        idx++;
      }
      token = "";
      continue;
    }
    if ((c >= '0' && c <= '9') || (c == '-' && token.length() == 0)) {
      token += c;
    } else if (c == '+') {
      // Ignore build metadata.
      break;
    }
  }
}

static int compareVersions(const String& a, const String& b)
{
  int pa[4], pb[4];
  parseVersion(a, pa, 4);
  parseVersion(b, pb, 4);
  for (int i = 0; i < 4; i++) {
    if (pa[i] < pb[i]) return -1;
    if (pa[i] > pb[i]) return 1;
  }
  return 0;
}

static void otaLock()
{
  if (otaMutex) xSemaphoreTake(otaMutex, portMAX_DELAY);
}

static void otaUnlock()
{
  if (otaMutex) xSemaphoreGive(otaMutex);
}

struct OtaReleaseInfo {
  bool ok = false;
  String version;
  String url;
  String error;
};

static OtaReleaseInfo fetchLatestRelease()
{
  OtaReleaseInfo info;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, kOtaReleaseUrl)) {
    info.error = "http_begin";
    return info;
  }
  http.addHeader("User-Agent", "BambuBeacon");
  const int code = http.GET();
  if (code != 200) {
    info.error = "http_" + String(code);
    http.end();
    return info;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    info.error = "json";
    return info;
  }

  String tag = doc["tag_name"] | "";
  info.version = trimTagVersion(tag);
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject a : assets) {
    String name = a["name"] | "";
    if (name.endsWith(".bin.ota")) {
      info.url = a["browser_download_url"] | "";
      break;
    }
  }

  if (info.version.length() == 0 || info.url.length() == 0) {
    info.error = "no_asset";
    return info;
  }

  info.ok = true;
  return info;
}

static bool runHttpUpdate(const String& url)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  const int len = http.getSize();
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t written = Update.writeStream(*stream);
  const bool ok = Update.end(true) && !Update.hasError() && written > 0;
  http.end();
  return ok;
}

static void otaTask(void* arg)
{
  (void)arg;
  for (;;) {
    bool doCheck = false;
    bool doUpdate = false;

    otaLock();
    doCheck = otaState.pendingCheck;
    doUpdate = otaState.pendingUpdate;
    if (doCheck || doUpdate) {
      otaState.busy = true;
      otaState.pendingCheck = false;
      otaState.pendingUpdate = false;
    }
    otaUnlock();

    if (doCheck) {
      OtaReleaseInfo info = fetchLatestRelease();
      otaLock();
      otaState.lastCheckOk = info.ok;
      otaState.latest = info.ok ? info.version : "";
      otaState.url = info.ok ? info.url : "";
      otaState.error = info.ok ? "" : info.error;
      otaState.updateAvailable = info.ok && (compareVersions(info.version, otaState.current) > 0);
      otaState.lastCheckMs = millis();
      otaState.busy = false;
      otaUnlock();
    } else if (doUpdate) {
      OtaReleaseInfo info = fetchLatestRelease();
      bool ok = false;
      bool updateAvailable = false;
      String err = "";
      if (info.ok) {
        updateAvailable = (compareVersions(info.version, otaState.current) > 0);
        if (updateAvailable) {
          ok = runHttpUpdate(info.url);
        } else {
          err = "no_update";
        }
      } else {
        err = info.error;
      }

      otaLock();
      otaState.lastCheckOk = info.ok;
      otaState.latest = info.ok ? info.version : "";
      otaState.url = info.ok ? info.url : "";
      otaState.error = err;
      otaState.updateAvailable = info.ok && updateAvailable;
      otaState.lastCheckMs = millis();
      otaState.lastUpdateOk = ok;
      otaState.busy = false;
      otaUnlock();

      if (ok) {
        scheduleRestart(600);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}

static void otaEnsureInit()
{
  if (!otaMutex) {
    otaMutex = xSemaphoreCreateMutex();
  }
  if (!otaTaskHandle) {
#ifdef STRVERSION
    otaState.current = STRVERSION;
#else
    otaState.current = "0.0.0";
#endif
#if CONFIG_FREERTOS_UNICORE
    xTaskCreate(otaTask, "otaTask", 8192, nullptr, 1, &otaTaskHandle);
#else
    xTaskCreatePinnedToCore(otaTask, "otaTask", 8192, nullptr, 1, &otaTaskHandle, 1);
#endif
  }
}

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

void WebServerHandler::handlePrinterDiscovery(AsyncWebServerRequest* req) {
  if (req->hasParam("rescan")) {
    printerDiscovery.forceRescan(0);
  }

  JsonDocument doc;
  JsonArray arr = doc["printers"].to<JsonArray>();

  const int n = printerDiscovery.knownCount();
  const BBLPrinter* printers = printerDiscovery.knownPrinters();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["usn"] = printers[i].usn;
    o["ip"] = printers[i].ip.toString();
  }

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void WebServerHandler::handleSubmitPrinterConfig(AsyncWebServerRequest* req) {
  auto getP = [&](const char* name) -> String {
    if (!req->hasParam(name, true)) return "";
    return req->getParam(name, true)->value();
  };

  settings.set.printerIP(getP("printerip"));
  settings.set.printerUSN(getP("printerusn"));
  settings.set.printerAC(getP("printerac"));
  settings.save();

  bambu.reloadFromSettings();
  if (WiFi.status() == WL_CONNECTED) bambu.connect();

  req->send(200, "application/json", "{\"success\":true}");
}

void WebServerHandler::begin() {
  auto captivePortalResponse = [&](AsyncWebServerRequest* req) {
    if (wifiManager.isApMode()) {
      sendGz(req, WiFiSetup_html_gz, WiFiSetup_html_gz_len, WiFiSetup_html_gz_mime);
      return;
    }
    req->send(404, "text/plain", "Not found");
  };

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

  // Captive portal detection endpoints (Android/iOS/Windows)
  server.on("/generate_204", HTTP_GET, captivePortalResponse);
  server.on("/gen_204", HTTP_GET, captivePortalResponse);
  server.on("/hotspot-detect.html", HTTP_GET, captivePortalResponse);
  server.on("/library/test/success.html", HTTP_GET, captivePortalResponse);
  server.on("/ncsi.txt", HTTP_GET, captivePortalResponse);
  server.on("/connecttest.txt", HTTP_GET, captivePortalResponse);
  server.on("/fwlink", HTTP_GET, captivePortalResponse);

  server.on("/printersetup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, PrinterSetup_html_gz, PrinterSetup_html_gz_len, PrinterSetup_html_gz_mime);
  });

  server.on("/maintenance", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    sendGz(req, Maintenance_html_gz, Maintenance_html_gz_len, Maintenance_html_gz_mime);
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

  server.on("/bblprinterdiscovery", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handlePrinterDiscovery(req);
  });

  server.on("/submitConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitConfig(req);
  });

  server.on("/submitPrinterConfig", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }
    handleSubmitPrinterConfig(req);
  });

  server.on("/config/backup", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    const bool pretty = req->hasParam("pretty");
    const String out = settings.backup(pretty);
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", out);
    r->addHeader("Content-Disposition", "attachment; filename=bambubeacon-backup.json");
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  server.on("/config/restore", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) {
          if (req->_tempObject) {
            delete (String*)req->_tempObject;
            req->_tempObject = nullptr;
          }
          return req->requestAuthentication();
        }
      }

      String* body = (String*)req->_tempObject;
      const bool ok = body && settings.restore(*body, true, true);
      if (body) {
        delete body;
        req->_tempObject = nullptr;
      }

      if (ok) {
        req->send(200, "application/json", "{\"success\":true}");
        scheduleRestart(600);
      } else {
        req->send(400, "application/json", "{\"success\":false}");
      }
    },
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      String* body = (String*)req->_tempObject;
      if (!body) {
        body = new String();
        body->reserve(total);
        req->_tempObject = body;
      }
      body->concat((const char*)data, len);
    }
  );

  server.on("/update", HTTP_POST,
    [&](AsyncWebServerRequest* req) {
      if (!wifiManager.isApMode()) {
        if (!isAuthorized(req)) return req->requestAuthentication();
      }
      const bool ok = !Update.hasError();
      req->send(ok ? 200 : 500, "application/json", ok ? "{\"success\":true}" : "{\"success\":false}");
      if (ok) scheduleRestart(600);
    },
    [&](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
      (void)filename;
      if (!wifiManager.isApMode() && !isAuthorized(req)) return;
      if (index == 0) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
      if (final) {
        if (!Update.end(true)) {
          Update.printError(Serial);
        }
      }
    }
  );

  server.on("/ota/check", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    otaEnsureInit();

    otaLock();
    if (!otaState.busy) {
      otaState.pendingCheck = true;
    }
    OtaState snap = otaState;
    otaUnlock();

    JsonDocument doc;
    doc["ok"] = snap.lastCheckOk;
    doc["current"] = snap.current;
    doc["latest"] = snap.latest;
    doc["url"] = snap.url;
    doc["updateAvailable"] = snap.updateAvailable;
    doc["pending"] = snap.pendingCheck || snap.busy;
    doc["error"] = snap.error;

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/ota/update", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    if (WiFi.status() != WL_CONNECTED) {
      req->send(503, "application/json", "{\"success\":false,\"error\":\"offline\"}");
      return;
    }

    otaEnsureInit();

    otaLock();
    if (otaState.busy || otaState.pendingUpdate) {
      otaUnlock();
      req->send(409, "application/json", "{\"success\":false,\"error\":\"busy\"}");
      return;
    }
    otaState.pendingUpdate = true;
    otaUnlock();

    req->send(202, "application/json", "{\"success\":true,\"queued\":true}");
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

  server.on("/printerconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["printerIP"] = settings.get.printerIP();
    doc["printerUSN"] = settings.get.printerUSN();
    doc["printerAC"] = settings.get.printerAC();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/ledconf.json", HTTP_GET, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    JsonDocument doc;
    doc["ledBrightness"] = settings.get.LEDBrightness();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/setLedBrightness", HTTP_POST, [&](AsyncWebServerRequest* req) {
    if (!wifiManager.isApMode()) {
      if (!isAuthorized(req)) return req->requestAuthentication();
    }

    if (!req->hasParam("brightness", true)) {
      req->send(400, "application/json", "{\"success\":false}");
      return;
    }

    const String v = req->getParam("brightness", true)->value();
    long b = v.toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    settings.set.LEDBrightness((uint16_t)b);
    settings.save();
    ledsCtrl.setBrightness((uint8_t)b);

    req->send(200, "application/json", "{\"success\":true}");
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
