#include "seed_store.h"

#include <Arduino.h>
#include <Preferences.h>  // simple key-value storage in flash ("NVS")
#include <esp_random.h>   // esp_fill_random(): hardware random numbers

// Where it lives in flash: a "folder" (namespace) and a name inside it.
static const char* kNamespace = "signer";
static const char* kKey = "seed";

bool loadOrCreateSeed(uint8_t seed[64]) {
  Preferences prefs;
  prefs.begin(kNamespace, false);  // false = we may write

  // getBytes() copies what's saved into `seed` and tells us how many bytes it found.
  // On the first boot nothing is saved, so it returns 0.
  size_t found = prefs.getBytes(kKey, seed, 64);

  // TODO(human): handle the first boot.
  //
  // If `found` is not 64 (nothing saved yet):
  //   1. fill `seed` with 64 random bytes:   esp_fill_random(seed, 64);
  //   2. save them so the next boot finds them:   prefs.putBytes(kKey, seed, 64);
  //   3. print a line like "new seed created" so you can see it happened
  // Otherwise print a line like "seed loaded from flash".
  //
  // The C pieces:  if (found != 64) { ... } else { ... }
  // NEVER print the seed itself: anything on the serial log should be treated as public.

if (found != 64) {
  esp_fill_random(seed, 64);       // 64 random bytes
  prefs.putBytes(kKey, seed, 64);  // save them in flash
  Serial.println("new seed created");
} else {
  Serial.println("seed loaded from flash");
}
  // Read it back from flash, to be sure it really got saved.
  found = prefs.getBytes(kKey, seed, 64);
  prefs.end();
  return found == 64;  // true = we have a seed; false = no keys, everything gets refused
}
