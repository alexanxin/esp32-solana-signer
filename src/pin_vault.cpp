#include "pin_vault.h"

#include <stdlib.h>
#include <string.h>

#include "keys.h"  // wipe()
#include "monocypher.h"

namespace {

constexpr uint8_t kVersion = 1;

// How hard Argon2 works. The CYD has no PSRAM, so memory is the tight one: 32 KB is
// borrowed for a moment and given back. Passes are cheap on memory and add time instead.
// Aim: about 1 second per PIN try on the board (the serial log prints the real time).
constexpr uint8_t kArgonKb = 32;
constexpr uint8_t kArgonPasses = 8;

// Where each part sits inside the 123 bytes (see pin_vault.h).
constexpr size_t kHeader = 3;
constexpr size_t kSaltAt = 3;
constexpr size_t kNonceAt = kSaltAt + 16;
constexpr size_t kMacAt = kNonceAt + 24;
constexpr size_t kCipherAt = kMacAt + 16;
static_assert(kCipherAt + 64 == kVaultSize, "vault layout");

// PIN + salt -> 32-byte key, with the memory/passes written in the vault header.
bool pinToKey(const char* pin, const uint8_t* header, const uint8_t salt[16], uint8_t key[32]) {
  uint32_t blocks = header[1];
  uint32_t passes = header[2];
  if (blocks < 8 || passes < 1) return false;

  void* work = malloc(blocks * 1024);  // borrowed memory for Argon2
  if (work == nullptr) return false;

  crypto_argon2_config config = {CRYPTO_ARGON2_ID, blocks, passes, 1};
  crypto_argon2_inputs inputs = {(const uint8_t*)pin, salt, (uint32_t)strlen(pin), 16};
  crypto_argon2(key, 32, work, config, inputs, crypto_argon2_no_extras);

  wipe(work, blocks * 1024);  // it held PIN-derived data
  free(work);
  return true;
}

}  // namespace

bool pinAllowed(const char* pin) {
  size_t n = strlen(pin);
  if (n < kPinMin || n > kPinMax) return false;
  for (size_t i = 0; i < n; i++) {
    if (pin[i] < '0' || pin[i] > '9') return false;
  }
  return true;
}

bool vaultSeal(const uint8_t seed[64], const char* pin, const uint8_t random40[kVaultRandomSize],
               uint8_t vault[kVaultSize]) {
  if (!pinAllowed(pin)) return false;

  vault[0] = kVersion;
  vault[1] = kArgonKb;
  vault[2] = kArgonPasses;
  memcpy(vault + kSaltAt, random40, 16);
  memcpy(vault + kNonceAt, random40 + 16, 24);

  uint8_t key[32];
  if (!pinToKey(pin, vault, vault + kSaltAt, key)) return false;

  // The header goes in as "additional data": not secret, but sealed, so nobody can
  // quietly swap the Argon2 settings to weaker ones.
  crypto_aead_lock(vault + kCipherAt, vault + kMacAt, key, vault + kNonceAt, vault, kHeader,
                   seed, 64);
  wipe(key, sizeof key);
  return true;
}

bool vaultOpen(const uint8_t vault[kVaultSize], const char* pin, uint8_t seed[64]) {
  memset(seed, 0, 64);
  if (vault[0] != kVersion || !pinAllowed(pin)) return false;

  uint8_t key[32];
  if (!pinToKey(pin, vault, vault + kSaltAt, key)) return false;

  // crypto_aead_unlock() checks the seal first. It returns 0 when the seal matches
  // (right PIN, nothing changed) and then writes the seed. Anything else: -1, seed untouched.
  int result = crypto_aead_unlock(seed, vault + kMacAt, key, vault + kNonceAt, vault, kHeader,
                                  vault + kCipherAt, 64);
  wipe(key, sizeof key);  // done with the key either way

  if (result != 0) {
    wipe(seed, 64);  // wrong PIN: leave nothing behind
    return false;
  }
  return true;
}
