// Runs on your computer, no board needed:  pio test -e native
#include <unity.h>

#include "led_pattern.h"

void setUp() {}
void tearDown() {}

static const LedBlink kNoBlink = {{0, 0, 0}, 0, 0};

void test_off_level_is_always_dark() {
  LedLook look = {LedMode::kSolid, kLedUp, 0};
  LedBlink blink = {kLedNewTx, 3, 0};
  Rgb c = ledCompute(look, blink, 50, LedLevel::kOff);
  TEST_ASSERT_EQUAL_UINT8(0, c.r + c.g + c.b);
}

void test_solid_is_scaled_by_level() {
  LedLook look = {LedMode::kSolid, {255, 0, 100}, 0};
  Rgb dim = ledCompute(look, kNoBlink, 1234, LedLevel::kDim);
  Rgb normal = ledCompute(look, kNoBlink, 1234, LedLevel::kNormal);
  TEST_ASSERT_EQUAL_UINT8(20, dim.r);
  TEST_ASSERT_EQUAL_UINT8(0, dim.g);
  TEST_ASSERT_EQUAL_UINT8(90, normal.r);
  TEST_ASSERT_TRUE(normal.b > dim.b);
}

void test_breathe_goes_up_and_down_but_never_dark() {
  LedLook look = {LedMode::kBreathe, {255, 255, 255}, 2000};
  uint8_t start = ledCompute(look, kNoBlink, 0, LedLevel::kNormal).r;
  uint8_t peak = ledCompute(look, kNoBlink, 1000, LedLevel::kNormal).r;
  uint8_t back = ledCompute(look, kNoBlink, 1999, LedLevel::kNormal).r;
  TEST_ASSERT_TRUE(start > 0);   // never fully dark
  TEST_ASSERT_EQUAL_UINT8(90, peak);  // full strength for "normal" in the middle
  TEST_ASSERT_TRUE(back < peak / 2);
  // Rising the whole first half.
  uint8_t last = 0;
  for (uint32_t t = 0; t <= 1000; t += 100) {
    uint8_t v = ledCompute(look, kNoBlink, t, LedLevel::kNormal).r;
    TEST_ASSERT_TRUE(v >= last);
    last = v;
  }
}

void test_blink_flashes_then_returns_to_the_look() {
  LedLook look = {LedMode::kSolid, {0, 0, 255}, 0};
  LedBlink blink = {{255, 0, 0}, 2, 10000};
  TEST_ASSERT_EQUAL_UINT8(90, ledCompute(look, blink, 10000, LedLevel::kNormal).r);  // flash 1
  Rgb gap = ledCompute(look, blink, 10000 + kBlinkOnMs + 10, LedLevel::kNormal);
  TEST_ASSERT_EQUAL_UINT8(0, gap.r + gap.b);                                        // dark gap
  TEST_ASSERT_EQUAL_UINT8(90, ledCompute(look, blink, 10000 + 300 + 5, LedLevel::kNormal).r);  // flash 2
  TEST_ASSERT_TRUE(ledBlinkActive(blink, 10000 + 599));
  TEST_ASSERT_FALSE(ledBlinkActive(blink, 10000 + 600));
  Rgb after = ledCompute(look, blink, 10000 + 600, LedLevel::kNormal);  // back to blue
  TEST_ASSERT_EQUAL_UINT8(0, after.r);
  TEST_ASSERT_EQUAL_UINT8(90, after.b);
}

void test_blink_survives_the_millis_wraparound() {
  // millis() goes back to 0 after about 49.7 days; a blink started just before must still end.
  LedBlink blink = {{255, 0, 0}, 1, 0xFFFFFFF0u};
  TEST_ASSERT_TRUE(ledBlinkActive(blink, 0xFFFFFFFFu));
  TEST_ASSERT_TRUE(ledBlinkActive(blink, 100));   // 116 ms after the start
  TEST_ASSERT_FALSE(ledBlinkActive(blink, 400));  // 416 ms after: over
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_off_level_is_always_dark);
  RUN_TEST(test_solid_is_scaled_by_level);
  RUN_TEST(test_breathe_goes_up_and_down_but_never_dark);
  RUN_TEST(test_blink_flashes_then_returns_to_the_look);
  RUN_TEST(test_blink_survives_the_millis_wraparound);
  return UNITY_END();
}
