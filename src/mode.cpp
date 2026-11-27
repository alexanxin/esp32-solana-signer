#ifdef BLEND_CYD
#include "mode.h"

#include <Arduino.h>
#include <esp_attr.h>    // RTC_NOINIT_ATTR
#include <esp_system.h>  // esp_restart()

// The ticket lives in RTC memory: a small piece of RAM that keeps its contents through a
// restart (esp_restart) but not through a power cut. "NOINIT" tells the startup code not to
// clear it. After a power cut it holds random bits, which is why we check for a magic number
// instead of just "non-zero": random bits matching all 32 of these is about 1 in 4 billion.
// Unlike flash, RTC memory doesn't wear out, so switching modes often costs nothing.
static RTC_NOINIT_ATTR uint32_t sTicket;
static const uint32_t kSignerTicket = 0x5161E5A9;  // "SIGLE" + a few random bits

SiglyMode modeAtBoot() {
  bool signer = sTicket == kSignerTicket;
  sTicket = 0;  // torn up: the start after this one is the Dashboard again
  return signer ? SiglyMode::kSigner : SiglyMode::kDashboard;
}

void restartInto(SiglyMode mode) {
  sTicket = mode == SiglyMode::kSigner ? kSignerTicket : 0;
  Serial.printf("[%6lu ms] restarting into %s\n", millis(),
                mode == SiglyMode::kSigner ? "Signer" : "Dashboard");
  Serial.flush();
  esp_restart();
}
#endif  // BLEND_CYD
