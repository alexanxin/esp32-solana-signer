#ifdef BLEND_CYD
#include "rgb_led.h"

#include <Arduino.h>
#include <Preferences.h>

// The usual CYD (ESP32-2432S028R) wiring: one LED with three colours inside. Its three
// legs share a "+" (a "common anode"), so a colour lights when its pin is pulled LOW, not
// HIGH: "on" is 0 and "off" is 255. That's why the duty is turned around below.
// If the colours come out wrong on your board, these three numbers are what to change.
static const int kPins[3] = {4, 17, 16};  // red, green, blue
// PWM channels 1-3 (channel 0 dims the screen, see backlight.cpp). Same speed and steps as
// the backlight: neighbouring channels share a timer, so they must agree.
static const int kChannels[3] = {1, 2, 3};
static const int kFreq = 5000;
static const int kBits = 8;

static LedLook gLook = {LedMode::kOff, {0, 0, 0}, 0};
static LedBlink gBlink = {{0, 0, 0}, 0, 0};
static LedLevel gLevel = LedLevel::kDim;
static Rgb gShown = {1, 1, 1};  // not black: the first ledService() writes the pins

void ledBegin() {
  Preferences prefs;
  prefs.begin("ui", true);  // the same "folder" as the screen brightness
  gLevel = (LedLevel)prefs.getUChar("led", (uint8_t)LedLevel::kDim);
  prefs.end();
  if (gLevel > LedLevel::kNormal) gLevel = LedLevel::kDim;
  for (int i = 0; i < 3; i++) {
    ledcSetup(kChannels[i], kFreq, kBits);
    ledcAttachPin(kPins[i], kChannels[i]);
  }
  ledService();  // dark until a screen says otherwise
}

void ledSetLook(LedMode mode, Rgb color, uint16_t periodMs) {
  gLook = {mode, color, periodMs};
}

void ledBlink(Rgb color, uint8_t times) {
  gBlink = {color, times, millis()};
}

void ledService() {
  Rgb c = ledCompute(gLook, gBlink, millis(), gLevel);
  if (c.r == gShown.r && c.g == gShown.g && c.b == gShown.b) return;  // nothing to change
  gShown = c;
  ledcWrite(kChannels[0], 255 - c.r);  // LOW = on: 255 - brightness
  ledcWrite(kChannels[1], 255 - c.g);
  ledcWrite(kChannels[2], 255 - c.b);
}

LedLevel ledLevel() { return gLevel; }

void ledSetLevel(LedLevel level) {
  gLevel = level;
  Preferences prefs;
  prefs.begin("ui", false);
  prefs.putUChar("led", (uint8_t)level);
  prefs.end();
  ledService();
}
#endif
