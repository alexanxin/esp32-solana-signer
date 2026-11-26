#pragma once
#include <stdint.h>

// Loads the board's 64-byte seed from flash. On the very first boot there is none yet,
// so it makes one from the hardware random generator and saves it.
// Call AFTER Bluetooth is started: the ESP32's random generator is only truly random
// while the radio is on (it uses radio noise).
// Returns false if there is still no seed (then the board refuses all key requests).
bool loadOrCreateSeed(uint8_t seed[64]);
