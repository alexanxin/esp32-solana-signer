#include "seed_store.h"

#include <Arduino.h>
#include <Preferences.h>  // simple key-value storage in flash ("NVS")
#include <esp_random.h>   // esp_fill_random(): hardware random numbers
#include <string.h>

#include "keys.h"  // wipe()
#include "pin_vault.h"

namespace {

// Where it lives in flash: a "folder" (namespace) and names inside it.
const char* kNamespace = "signer";
const char* kPlainKey = "seed";
const char* kVaultKey = "vault";
const char* kFailsKey = "fails";

constexpr uint32_t kFirstWaitMs = 30000;    // 6th wrong PIN: wait 30 s
constexpr uint32_t kLongestWaitMs = 3600000;  // never more than an hour

uint32_t gLastFailAt = 0;  // millis() of the last wrong PIN (this boot)

void logHeap(const char* when) {
  Serial.printf("[%6lu ms] %s: free heap %u, biggest block %u\n", millis(), when,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

}  // namespace

SeedState seedState() {
  Preferences prefs;
  prefs.begin(kNamespace, true);  // true = read only
  SeedState s = prefs.getBytesLength(kVaultKey) == kVaultSize ? SeedState::kVault
                : prefs.getBytesLength(kPlainKey) == 64     ? SeedState::kPlain
                                                            : SeedState::kNone;
  prefs.end();
  return s;
}

bool loadOrCreateSeed(uint8_t seed[64]) {
  Preferences prefs;
  prefs.begin(kNamespace, false);  // false = we may write

  // getBytes() copies what's saved into `seed` and tells us how many bytes it found.
  // On the first boot nothing is saved, so it returns 0.
  size_t found = prefs.getBytes(kPlainKey, seed, 64);
  if (found != 64) {
    esp_fill_random(seed, 64);            // 64 random bytes
    prefs.putBytes(kPlainKey, seed, 64);  // save them in flash
    Serial.println("new seed created");
  } else {
    Serial.println("seed loaded from flash");
  }
  // Read it back from flash, to be sure it really got saved.
  found = prefs.getBytes(kPlainKey, seed, 64);
  prefs.end();
  return found == 64;  // true = we have a seed; false = no keys, everything gets refused
}

void newRandomSeed(uint8_t seed[64]) { esp_fill_random(seed, 64); }

bool loadPlainSeed(uint8_t seed[64]) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  size_t found = prefs.getBytes(kPlainKey, seed, 64);
  prefs.end();
  return found == 64;
}

bool saveVault(const uint8_t seed[64], const char* pin) {
  uint8_t random40[kVaultRandomSize];
  esp_fill_random(random40, sizeof random40);  // fresh salt + nonce every time

  uint8_t vault[kVaultSize];
  logHeap("before Argon2");
  uint32_t t0 = millis();
  bool sealed = vaultSeal(seed, pin, random40, vault);
  Serial.printf("[%6lu ms] Argon2 took %lu ms\n", millis(), (unsigned long)(millis() - t0));
  if (!sealed) {
    Serial.println("vault: could not lock (PIN not allowed or no memory)");
    return false;
  }

  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putBytes(kVaultKey, vault, kVaultSize);

  // Read back AND open it once, before the plain seed is gone.
  uint8_t back[kVaultSize], check[64];
  bool ok = prefs.getBytes(kVaultKey, back, kVaultSize) == kVaultSize &&
            vaultOpen(back, pin, check) && memcmp(check, seed, 64) == 0;
  wipe(check, sizeof check);
  if (ok) {
    prefs.remove(kPlainKey);  // the unlocked copy goes; only the vault stays
    prefs.putUChar(kFailsKey, 0);
    Serial.println("vault: seed locked with your PIN and saved");
  } else {
    prefs.remove(kVaultKey);  // don't leave a broken vault behind
    Serial.println("vault: check after saving FAILED, nothing changed");
  }
  prefs.end();
  return ok;
}

int failedTries() {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  int n = prefs.getUChar(kFailsKey, 0);
  prefs.end();
  return n;
}

uint32_t waitBeforeNextTryMs() {
  int fails = failedTries();
  if (fails < kFreeTries) return 0;
  // 30 s, 60 s, 120 s, ... doubling with every extra wrong PIN, at most an hour.
  int extra = fails - kFreeTries;
  uint32_t wait = extra >= 7 ? kLongestWaitMs : kFirstWaitMs << extra;
  if (wait > kLongestWaitMs) wait = kLongestWaitMs;
  // After a reboot gLastFailAt is 0, so the full wait starts again from power-on:
  // pulling the plug never makes guessing faster.
  uint32_t since = millis() - gLastFailAt;
  return since >= wait ? 0 : wait - since;
}

UnlockResult unlockVault(const char* pin, uint8_t seed[64]) {
  if (waitBeforeNextTryMs() > 0) return UnlockResult::kMustWait;
  // Argon2 borrows 32 KB in one piece. Check first, so a full memory never counts as a wrong PIN.
  if (ESP.getMaxAllocHeap() < 32 * 1024 + 1024) return UnlockResult::kNoMemory;

  Preferences prefs;
  prefs.begin(kNamespace, false);
  uint8_t vault[kVaultSize];
  if (prefs.getBytes(kVaultKey, vault, kVaultSize) != kVaultSize) {
    prefs.end();
    return UnlockResult::kNoVault;
  }

  // Count the try as wrong BEFORE checking. If someone cuts the power during the check
  // to hide a wrong guess, it still counts.
  uint8_t fails = prefs.getUChar(kFailsKey, 0);
  prefs.putUChar(kFailsKey, fails < 250 ? fails + 1 : fails);

  logHeap("before Argon2");
  uint32_t t0 = millis();
  bool ok = vaultOpen(vault, pin, seed);
  Serial.printf("[%6lu ms] Argon2 took %lu ms\n", millis(), (unsigned long)(millis() - t0));

  if (ok) {
    prefs.putUChar(kFailsKey, 0);
    Serial.println("vault: unlocked");
  } else {
    gLastFailAt = millis();
    Serial.printf("vault: wrong PIN (%d wrong in a row)\n", fails + 1);
  }
  prefs.end();
  return ok ? UnlockResult::kOk : UnlockResult::kWrongPin;
}
