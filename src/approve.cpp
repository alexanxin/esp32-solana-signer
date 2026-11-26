#include "approve.h"

#include <Arduino.h>

#include "log.h"

namespace {

// The BOOT button is on GPIO0 on the CYD (and most ESP32 boards), GPIO9 on the C3.
#ifdef BLEND_C3
constexpr int kBootButton = 9;
#else
constexpr int kBootButton = 0;
#endif

constexpr uint32_t kTimeoutMs = 30000;  // 30 seconds

}  // namespace

// For now this is "blind signing": the board can't show WHAT it signs yet, only how big
// it is. Showing recipient + amount on the CYD screen comes later.
bool waitForApproval(const uint8_t* message, size_t len) {
  logHex(">>> SIGN REQUEST, message", message, len);
  Serial.println(">>> Press BOOT within 30 s to approve, or wait to refuse.");

  pinMode(kBootButton, INPUT_PULLUP);  // pressed = LOW, released = HIGH
  uint32_t start = millis();           // the time right now, in milliseconds

  // TODO(human): wait for the button, at most kTimeoutMs.
  //
  // In plain words:
  //   while less than kTimeoutMs has passed since start:
  //       if the button reads LOW  -> print "approved" and return true
  //       wait 10 ms
  //   (the loop ended, so time ran out) -> print "timed out" and return false
  //
  // The C pieces you need:
  //   while (millis() - start < kTimeoutMs) { ... }    repeats the { } part
  //   if (digitalRead(kBootButton) == LOW) { ... }      runs the { } part only if pressed
  //   Serial.println("approved");                       prints a line
  //   delay(10);                                        waits 10 ms
  //   return true;  /  return false;                    ends the function with an answer




  (void)start;
while (millis() - start < kTimeoutMs) {
  if (digitalRead(kBootButton) == LOW) {
    Serial.println("approved");
    return true;
  }
  delay(10);
}
Serial.println("timed out");
return false;

}
