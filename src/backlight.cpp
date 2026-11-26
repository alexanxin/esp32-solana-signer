#ifdef BLEND_CYD
#include "backlight.h"

#include <Arduino.h>
#include <Preferences.h>

// How dimming works: an LED is either on or off. PWM ("pulse width modulation") switches
// it on and off thousands of times a second, faster than the eye can follow. The eye sees
// the average: on half the time looks about half as bright. The ESP32 has a hardware block
// for this called LEDC, so the CPU doesn't have to do the switching.
//
// "Duty" is how much of each pulse is "on". With 8 bits it goes from 0 (always off)
// to 255 (always on).

static const int kPin = 27;     // the backlight pin (TFT_BL in platformio.ini)
static const int kChannel = 0;  // LEDC has 16 channels; we use the first
static const int kFreq = 5000;  // 5000 pulses a second: no visible flicker
static const int kBits = 8;     // duty range 0..255

static uint8_t gPercent = 70;   // default: a bit dimmer than full, as you asked

void backlightSet(uint8_t percent) {
  if (percent < 10) percent = 10;  // never fully dark: you'd think the board is dead
  if (percent > 100) percent = 100;
  gPercent = percent;

  uint32_t duty = percent * 255 / 100;
  ledcWrite(kChannel, duty);
}

uint8_t backlightPercent() { return gPercent; }

void backlightBegin() {
  Preferences prefs;
  prefs.begin("ui", true);  // true = read only
  gPercent = prefs.getUChar("bright", 70);  // 70 if nothing saved yet
  prefs.end();

  ledcSetup(kChannel, kFreq, kBits);  // set up the PWM channel...
  ledcAttachPin(kPin, kChannel);      // ...and connect it to the backlight pin
  backlightSet(gPercent);
}

// Flash wears out after many writes, so we save once when the finger lifts,
// not on every step while the slider moves.
void backlightSave() {
  Preferences prefs;
  prefs.begin("ui", false);
  prefs.putUChar("bright", gPercent);
  prefs.end();
}
#endif
