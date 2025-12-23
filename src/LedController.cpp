#include "LedController.h"

#include "main.h"           // LED_PIN via build_flags
#include "SettingsPrefs.h"

static CRGB bootColorForSegment(uint8_t seg) {
  switch (seg) {
    case 0: 
      return CRGB::Red;              // Ring 1 (top) - red
    case 1: 
      return CRGB(255, 90, 0);      // Ring 2 (middle) - traffic amber (reduced green)
    case 2: 
      return CRGB::Green;            // Ring 3 (bottom) - green
    default: 
      return CRGB::White;
  }
}

LedController::LedController()
: _leds(nullptr),
  _perSeg(0),
  _segments(0),
  _count(0),
  _brightness(0),
  _dirty(false),
  _lastTickMs(0),
  _bootTestActive(false),
  _bootSeg(0),
  _bootPosInSeg(0),
  _bootNextMs(0),
  _st() {}

LedController::~LedController() {
  freeBuf();
}

bool LedController::alloc(uint16_t count) {
  freeBuf();
  if (count == 0) return false;
  _leds = new CRGB[count];
  if (!_leds) return false;
  _count = count;
  return true;
}

void LedController::freeBuf() {
  if (_leds) {
    delete[] _leds;
    _leds = nullptr;
  }
  _count = 0;
}

bool LedController::begin(Settings& settings) {
  _perSeg     = settings.get.LEDperSeg();
  _segments   = (uint8_t)settings.get.LEDSegments();
  _brightness = (uint8_t)settings.get.LEDBrightness();

  if (_perSeg == 0 || _segments == 0) return false;
  if (!alloc((uint16_t)_perSeg * _segments)) return false;

#ifndef LED_PIN
#error "LED_PIN must be defined via build_flags"
#endif

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(_leds, _count);
  FastLED.setBrightness(_brightness);

  clear(true);

  // OPTIONAL but highly recommended while testing:
  // Serial.printf("LEDperSeg=%u LEDSegments=%u total=%u\n", _perSeg, _segments, _count);

  uint32_t now = millis();
  startBootTest(now);
  _lastTickMs = now;
  return true;
}

void LedController::applySettingsFrom(Settings& settings) {
  uint8_t newBright = (uint8_t)settings.get.LEDBrightness();
  if (newBright != _brightness) {
    _brightness = newBright;
    FastLED.setBrightness(_brightness);
    markDirty();
  }
}

void LedController::ingestBambuReport(JsonObjectConst report, uint32_t nowMs) {
  (void)report;
  _st.hasMqtt = true;
  _st.lastMqttMs = nowMs;
  markDirty();
}

void LedController::startSelfTest() {
  startBootTest(millis());
}

void LedController::setBrightness(uint8_t b) {
  _brightness = b;
  FastLED.setBrightness(_brightness);
  markDirty();
}

void LedController::clear(bool showNow) {
  if (!_leds) return;
  fill_solid(_leds, _count, CRGB::Black);
  _dirty = true;
  if (showNow) FastLED.show();
}

void LedController::setPixel(uint16_t idx, const CRGB& c, bool showNow) {
  if (!_leds || idx >= _count) return;
  _leds[idx] = c;
  markDirty();
  if (showNow) FastLED.show();
}

void LedController::setSegmentColor(uint8_t seg, const CRGB& c, bool showNow) {
  if (!_leds || seg >= _segments) return;
  for (uint16_t i = segStart(seg); i < segEnd(seg); i++)
    _leds[i] = c;
  markDirty();
  if (showNow) FastLED.show();
}

void LedController::showIfDirty() {
  if (!_dirty) return;
  _dirty = false;
  FastLED.show();
}

void LedController::setGlobalIdle() {
  clear(false);
  if (_segments >= 1)
    setSegmentColor(0, CRGB::White, false);
  markDirty();
}

void LedController::setNoConnection() {
  clear(false);
  markDirty();
}

/* ================= Boot Selftest (Ampel, segmentweise) ================= */

void LedController::startBootTest(uint32_t nowMs) {
  if (!_leds) return;

  _bootTestActive = true;
  _bootSeg = 0;
  _bootPosInSeg = 0;
  _bootNextMs = nowMs;

  fill_solid(_leds, _count, CRGB::Black);
  markDirty();
}

void LedController::tickBootTest(uint32_t nowMs) {
  if (!_bootTestActive || !_leds) return;

  const uint32_t STEP_MS = 80; // ruhig
  if ((int32_t)(nowMs - _bootNextMs) < 0) return;

  if (_bootSeg >= _segments) {
    // Done -> no flash, go to default
    _bootTestActive = false;
    setGlobalIdle();
    markDirty();
    return;
  }

  // Turn on next LED inside current segment and keep previous ones on
  if (_bootPosInSeg < _perSeg) {
    uint16_t idx = segStart(_bootSeg) + _bootPosInSeg;
    if (idx < _count) {
      _leds[idx] = bootColorForSegment(_bootSeg);
    }
    _bootPosInSeg++;
    _bootNextMs = nowMs + STEP_MS;
    markDirty();
    return;
  }

  // Segment finished -> next segment
  _bootSeg++;
  _bootPosInSeg = 0;
  _bootNextMs = nowMs + STEP_MS;
}

/* ================= Core ================= */

void LedController::deriveStateFromReport(JsonObjectConst report, uint32_t nowMs) {
  (void)report;
  _st.hasMqtt = true;
  _st.lastMqttMs = nowMs;
}

void LedController::render(uint32_t nowMs) {
  (void)nowMs;
  if (!_st.hasMqtt) setNoConnection();
  else setGlobalIdle();
}

void LedController::tick(uint32_t nowMs) {
  if (_bootTestActive) {
    tickBootTest(nowMs);
    return;
  }
  render(nowMs);
}

void LedController::loop() {
  if (!_leds) return;

  uint32_t now = millis();
  if ((uint32_t)(now - _lastTickMs) >= 25) {
    _lastTickMs = now;
    tick(now);
  }
  showIfDirty();
}
