#pragma once
#include <Arduino.h>

// Prints "label (n bytes): 05 00 00 ..." so we can read exactly what the wallet sent.
inline void logHex(const char* label, const uint8_t* data, size_t len) {
  Serial.printf("[%6lu ms] %s (%u bytes):", millis(), label, (unsigned)len);
  for (size_t i = 0; i < len; i++) Serial.printf(" %02x", data[i]);
  Serial.println();
}
