#include "led_pattern.h"

namespace {

// How much of each colour reaches the LED for each setting, out of 255. The eye notices
// small changes in a dim light much more than in a bright one, so "dim" is very low.
uint8_t levelScale(LedLevel level) {
  switch (level) {
    case LedLevel::kDim:
      return 20;
    case LedLevel::kNormal:
      return 90;
    default:
      return 0;
  }
}

Rgb scale(Rgb c, uint32_t by) {  // by: 0..255 (255 = unchanged)
  return {(uint8_t)(c.r * by / 255), (uint8_t)(c.g * by / 255), (uint8_t)(c.b * by / 255)};
}

}  // namespace

bool ledBlinkActive(const LedBlink& blink, uint32_t nowMs) {
  if (blink.times == 0) return false;
  uint32_t elapsed = nowMs - blink.startMs;  // unsigned maths: right even when millis() wraps
  return elapsed < (uint32_t)blink.times * (kBlinkOnMs + kBlinkOffMs);
}

Rgb ledCompute(const LedLook& look, const LedBlink& blink, uint32_t nowMs, LedLevel level) {
  uint8_t s = levelScale(level);
  if (s == 0) return {0, 0, 0};

  if (ledBlinkActive(blink, nowMs)) {
    uint32_t inFlash = (nowMs - blink.startMs) % (kBlinkOnMs + kBlinkOffMs);
    return inFlash < kBlinkOnMs ? scale(blink.color, s) : Rgb{0, 0, 0};
  }

  switch (look.mode) {
    case LedMode::kSolid:
      return scale(look.color, s);
    case LedMode::kBreathe: {
      if (look.periodMs == 0) return scale(look.color, s);
      // A triangle: 0 -> 255 in the first half of the period, back to 0 in the second...
      uint32_t t = nowMs % look.periodMs;
      uint32_t half = look.periodMs / 2;
      uint32_t tri = t < half ? t * 255 / half : (look.periodMs - t) * 255 / half;
      // ...squared, so it lingers when dim and swells smoothly (a straight line looks like
      // it jumps at the bottom), and never fully dark: from 10% to 100%.
      uint32_t curve = tri * tri / 255;
      uint32_t strength = 25 + curve * 230 / 255;
      return scale(scale(look.color, strength), s);
    }
    default:
      return {0, 0, 0};
  }
}
