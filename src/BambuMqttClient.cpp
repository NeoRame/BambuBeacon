#include "BambuMqttClient.h"

const char* BambuMqttClient::kUser = "bblp";

BambuMqttClient::BambuMqttClient() {}
BambuMqttClient::~BambuMqttClient() {
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
}

bool BambuMqttClient::begin(Settings &settings) {
  _settings = &settings;
  _ready = false;

  buildFromSettings();

  if (!configLooksValid()) {
    webSerial.println("[MQTT] Missing settings (printerIP/printerUSN/printerAC). Not connecting yet.");
    // Safe: do not allocate events, loopTick will early-return.
    return false;
  }

  // Allocate bounded HMS storage
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
  _events = new HmsEvent[_eventsCap];

  _mqtt.setServer(_serverUri.c_str());
  _mqtt.setClientId(_clientId.c_str());
  _mqtt.setCredentials(kUser, _accessCode.c_str());

  // TLS insecure by design: no CA/certs configured.
  webSerial.println("[MQTT] TLS: no CA bundle configured (insecure / no-verify expected).");

  _mqtt.onConnect([this](bool sessionPresent) {
    webSerial.printf("[MQTT] Connected (session=%d)\n", (int)sessionPresent);
    _subscribed = false;
    subscribeReportOnce();
  });

  _mqtt.onDisconnect([this](bool) {
    webSerial.println("[MQTT] Disconnected");
    _subscribed = false;
  });

  // esp_mqtt_error_codes is not convertible in your toolchain -> raw dump
  _mqtt.onError([](esp_mqtt_error_codes error) {
    webSerial.println("[MQTT] Error callback triggered");
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&error);
    const size_t n = sizeof(error);

    webSerial.printf("[MQTT] Error raw (%u bytes): ", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
      webSerial.printf("%02X", p[i]);
      if (i + 1 < n) webSerial.print(" ");
    }
    webSerial.print("\n");
  });

  _mqtt.onSubscribe([](int msgId) {
    webSerial.printf("[MQTT] Subscribed (msgId=%d)\n", msgId);
  });

  _mqtt.onTopic(_topicReport.c_str(), 0,
    [this](const char* /*topic*/, const char* payload, int, int, bool) {
      handleReportJson(payload);
    });

  _ready = true;

  if (WiFi.status() == WL_CONNECTED) {
    connect();
  } else {
    webSerial.println("[MQTT] WiFi not connected yet - will connect from loopTick().");
  }

  return true;
}

void BambuMqttClient::reloadFromSettings() {
  buildFromSettings();

  if (!configLooksValid()) {
    _ready = false;

    // Free event buffer to keep state clean & avoid stale stuff
    if (_events) {
      delete[] _events;
      _events = nullptr;
    }

    webSerial.println("[MQTT] Settings reloaded but still incomplete.");
    return;
  }

  // Ensure event buffer exists
  if (_events) {
    delete[] _events;
    _events = nullptr;
  }
  _events = new HmsEvent[_eventsCap];

  _mqtt.setServer(_serverUri.c_str());
  _mqtt.setClientId(_clientId.c_str());
  _mqtt.setCredentials(kUser, _accessCode.c_str());

  _subscribed = false;
  _ready = true;

  webSerial.println("[MQTT] Settings reloaded.");
}

void BambuMqttClient::buildFromSettings() {
  // Always from settings. No alternatives.
  const char* ip  = _settings ? _settings->get.printerIP()  : "";
  const char* usn = _settings ? _settings->get.printerUSN() : "";
  const char* ac  = _settings ? _settings->get.printerAC()  : "";

  _printerIP   = (ip  && ip[0])  ? String(ip)  : "";
  _serial      = (usn && usn[0]) ? String(usn) : "";
  _accessCode  = (ac  && ac[0])  ? String(ac)  : "";

  _clientId = String("bambubeacon-") + String((uint32_t)ESP.getEfuseMac(), HEX);

  _topicReport  = String("device/") + _serial + "/report";
  _topicRequest = String("device/") + _serial + "/request";
  _serverUri    = String("mqtts://") + _printerIP + ":" + String(kPort);

  // HMS defaults (can be moved into settings later)
  _hmsTtlMs  = 20000;
  _eventsCap = 20;
  _ignoreNorm = "";

  // Do not touch _gcodeState here
}

bool BambuMqttClient::configLooksValid() const {
  return !_printerIP.isEmpty() && !_serial.isEmpty() && !_accessCode.isEmpty();
}

void BambuMqttClient::connect() {
  if (!_ready) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!configLooksValid()) {
    webSerial.println("[MQTT] Cannot connect: missing settings.");
    return;
  }

  webSerial.printf("[MQTT] Connecting to %s (clientId=%s)\n",
                   _serverUri.c_str(), _clientId.c_str());
  _mqtt.connect();
}

void BambuMqttClient::disconnect() {
  _mqtt.disconnect();
}

bool BambuMqttClient::isConnected() {
  return _mqtt.connected();
}

void BambuMqttClient::loopTick() {
  // NEW: completely safe when not configured yet
  if (!_ready || !_events) return;
  if (WiFi.status() != WL_CONNECTED) {
    // Still expire HMS so old errors do not stick forever if WiFi drops
    expireEvents(millis());
    return;
  }

  if (!_mqtt.connected()) {
    const uint32_t now = millis();
    if (now - _lastKickMs > 2000) {
      _lastKickMs = now;
      connect();
    }
  }

  expireEvents(millis());
}

bool BambuMqttClient::publishRequest(const JsonDocument& doc, bool retain) {
  if (!_ready || !_mqtt.connected()) return false;

  String out;
  serializeJson(doc, out);

  const int msgId = _mqtt.publish(_topicRequest.c_str(), 0, retain, out.c_str());
  webSerial.printf("[MQTT] Publish request msgId=%d len=%u\n", msgId, (unsigned)out.length());
  return (msgId >= 0);
}

void BambuMqttClient::onReport(ReportCallback cb) {
  _reportCb = cb;
}

const String& BambuMqttClient::topicReport() const { return _topicReport; }
const String& BambuMqttClient::topicRequest() const { return _topicRequest; }
const String& BambuMqttClient::gcodeState() const { return _gcodeState; }

void BambuMqttClient::subscribeReportOnce() {
  if (_subscribed) return;

  webSerial.printf("[MQTT] Subscribing to %s\n", _topicReport.c_str());
  _mqtt.subscribe(_topicReport.c_str(), 0);
  _subscribed = true;
}

void BambuMqttClient::handleReportJson(const char* payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    webSerial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
    return;
  }

  if (doc["print"]["gcode_state"].is<const char*>()) {
    _gcodeState = (const char*)doc["print"]["gcode_state"];
  } else if (doc["gcode_state"].is<const char*>()) {
    _gcodeState = (const char*)doc["gcode_state"];
  }

  parseHmsFromDoc(doc);

  if (_reportCb) _reportCb(doc);
}

JsonArray BambuMqttClient::findHmsArray(JsonDocument& doc) {
  if (doc["hms"].is<JsonArray>()) return doc["hms"].as<JsonArray>();
  if (doc["print"]["hms"].is<JsonArray>()) return doc["print"]["hms"].as<JsonArray>();
  if (doc["data"]["hms"].is<JsonArray>()) return doc["data"]["hms"].as<JsonArray>();
  return JsonArray();
}

void BambuMqttClient::parseHmsFromDoc(JsonDocument& doc) {
  const uint32_t now = millis();

  JsonArray arr = findHmsArray(doc);
  if (!arr) {
    expireEvents(now);
    return;
  }

  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) continue;
    JsonObject h = v.as<JsonObject>();

    if (!h["attr"].is<uint32_t>() || !h["code"].is<uint32_t>()) continue;
    const uint32_t attr = (uint32_t)h["attr"];
    const uint32_t code = (uint32_t)h["code"];

    const uint64_t full = (uint64_t(attr) << 32) | uint64_t(code);
    char codeStr[24];
    formatHmsCodeStr(full, codeStr);

    if (isIgnored(codeStr)) continue;

    upsertEvent(attr, code, now);
  }

  expireEvents(now);
}

bool BambuMqttClient::isIgnored(const char* codeStr) const {
  if (_ignoreNorm.isEmpty()) return false;
  return (_ignoreNorm.indexOf(codeStr) >= 0);
}

BambuMqttClient::Severity BambuMqttClient::severityFromCode(uint32_t code) {
  const uint16_t s = (uint16_t)(code >> 16);
  switch (s) {
    case 1: return Severity::Fatal;
    case 2: return Severity::Error;
    case 3: return Severity::Warning;
    case 4: return Severity::Info;
    default: return Severity::None;
  }
}

void BambuMqttClient::formatHmsCodeStr(uint64_t full, char out[24]) {
  const uint16_t a = (uint16_t)(full >> 48);
  const uint16_t b = (uint16_t)(full >> 32);
  const uint16_t c = (uint16_t)(full >> 16);
  const uint16_t d = (uint16_t)(full);
  snprintf(out, 24, "HMS_%04X_%04X_%04X_%04X", a, b, c, d);
}

void BambuMqttClient::upsertEvent(uint32_t attr, uint32_t code, uint32_t nowMs) {
  if (!_events) return;

  const uint64_t full = (uint64_t(attr) << 32) | uint64_t(code);

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == full) {
      _events[i].lastSeenMs = nowMs;
      _events[i].count++;
      _events[i].active = true;
      return;
    }
  }

  int slot = -1;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == 0) { slot = i; break; }
  }

  if (slot < 0) {
    uint32_t bestAge = 0;
    for (uint8_t i = 0; i < _eventsCap; i++) {
      if (_events[i].active) continue;
      const uint32_t age = nowMs - _events[i].lastSeenMs;
      if (age >= bestAge) { bestAge = age; slot = i; }
    }
    if (slot < 0) {
      bestAge = 0;
      for (uint8_t i = 0; i < _eventsCap; i++) {
        const uint32_t age = nowMs - _events[i].lastSeenMs;
        if (age >= bestAge) { bestAge = age; slot = i; }
      }
    }
  }

  HmsEvent& e = _events[slot];
  e.full = full;
  e.attr = attr;
  e.code = code;
  formatHmsCodeStr(full, e.codeStr);
  e.severity = severityFromCode(code);
  e.firstSeenMs = nowMs;
  e.lastSeenMs = nowMs;
  e.count = 1;
  e.active = true;
}

void BambuMqttClient::expireEvents(uint32_t nowMs) {
  if (!_events) return;

  const uint32_t ttl = _hmsTtlMs ? _hmsTtlMs : 20000;

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].full == 0) continue;
    if (_events[i].active && (nowMs - _events[i].lastSeenMs > ttl)) {
      _events[i].active = false;
    }
  }
}

BambuMqttClient::Severity BambuMqttClient::computeTopSeverity() const {
  Severity top = Severity::None;
  if (!_events) return top;

  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (!_events[i].active) continue;
    if ((uint8_t)_events[i].severity > (uint8_t)top) top = _events[i].severity;
  }
  return top;
}

BambuMqttClient::Severity BambuMqttClient::topSeverity() const {
  return computeTopSeverity();
}

bool BambuMqttClient::hasProblem() const {
  return (uint8_t)topSeverity() >= (uint8_t)Severity::Warning;
}

uint16_t BambuMqttClient::countActive(Severity sev) const {
  if (!_events) return 0;

  uint16_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].active && _events[i].severity == sev) n++;
  }
  return n;
}

uint16_t BambuMqttClient::countActiveTotal() const {
  if (!_events) return 0;

  uint16_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (_events[i].active) n++;
  }
  return n;
}

size_t BambuMqttClient::getActiveEvents(HmsEvent* out, size_t maxOut) const {
  if (!_events || !out || !maxOut) return 0;

  size_t n = 0;
  for (uint8_t i = 0; i < _eventsCap; i++) {
    if (!_events[i].active) continue;
    out[n++] = _events[i];
    if (n >= maxOut) break;
  }
  return n;
}
