#pragma once
// What the CYD's RGB status light shows at a given moment: a colour, steady or "breathing",
// with short blinks on top. Plain C++ (no Arduino) so it is tested on the computer:
// pio test -e native. rgb_led.cpp turns the result into PWM on the three LED pins.
#include <stdint.h>

struct Rgb {
  uint8_t r, g, b;
};

// The light's brightness setting (Settings > Status light). This LED is very bright at full
// power, so "normal" is already well below its maximum.
enum class LedLevel : uint8_t { kOff, kDim, kNormal };

enum class LedMode : uint8_t {
  kOff,
  kSolid,    // the colour, steady
  kBreathe,  // fades between dim and full, once per period
};

// The steady look: what the light says when nothing just happened.
struct LedLook {
  LedMode mode;
  Rgb color;
  uint16_t periodMs;  // kBreathe: one full breath (in and out)
};

// A few quick flashes on top of the look, e.g. "a new transaction arrived".
struct LedBlink {
  Rgb color;
  uint8_t times;     // 0 = no blink going on
  uint32_t startMs;  // when it began
};

constexpr uint16_t kBlinkOnMs = 120;   // each flash: 120 ms on...
constexpr uint16_t kBlinkOffMs = 180;  // ...then 180 ms off

// The colour to show at time nowMs (any millisecond counter, it may wrap around).
Rgb ledCompute(const LedLook& look, const LedBlink& blink, uint32_t nowMs, LedLevel level);
// True while the blink is still going on at nowMs.
bool ledBlinkActive(const LedBlink& blink, uint32_t nowMs);

// The colours Sigly uses, at full strength (ledCompute scales them by the level).
constexpr Rgb kLedUp = {40, 255, 0};        // portfolio up over 24 h (lime)
constexpr Rgb kLedDown = {255, 60, 0};      // portfolio down (orange)
constexpr Rgb kLedNewTx = {255, 255, 255};  // blink: a new transaction
constexpr Rgb kLedPhone = {0, 40, 255};     // Signer: a phone is connected (blue)
constexpr Rgb kLedAsk = {255, 120, 0};      // Signer: waiting for your approval (amber)
constexpr Rgb kLedSigned = {40, 255, 0};    // blink: approved and signed
constexpr Rgb kLedRejected = {255, 0, 0};   // blink: rejected or timed out
