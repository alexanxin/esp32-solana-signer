#include "keys.h"

#include <string.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace {

// The 64-byte seed every key comes from. Set once at boot by setSeed(): from flash on the
// board (seed_store.cpp), or the public test phrase in the unit tests.
uint8_t gSeed[64];
bool gHaveSeed = false;

}  // namespace

// SLIP-10 for Ed25519 (what Phantom, Solflare and Ledger all use):
//   I = HMAC-SHA512(key = "ed25519 seed", data = seed)
//   for each level i:  I = HMAC-SHA512(key = right half of I, data = 0x00 || left half || i')
//   private key = left 32 bytes of the final I
void setSeed(const uint8_t seed[64]) {
  memcpy(gSeed, seed, 64);
  gHaveSeed = true;
}

bool hasSeed() { return gHaveSeed; }

void clearSeed() {
  wipe(gSeed, sizeof gSeed);
  gHaveSeed = false;
}

void deriveKey(const uint32_t* path, size_t pathLen, uint8_t privateKey[32]) {
  uint8_t I[64];
  crypto_sha512_hmac(I, (const uint8_t*)"ed25519 seed", 12, gSeed, sizeof gSeed);

  for (size_t level = 0; level < pathLen; level++) {
    uint32_t index = path[level] | 0x80000000u;  // hardened
    uint8_t data[37];
    data[0] = 0x00;
    memcpy(data + 1, I, 32);
    data[33] = index >> 24;
    data[34] = index >> 16;
    data[35] = index >> 8;
    data[36] = index;

    uint8_t chainCode[32];
    memcpy(chainCode, I + 32, 32);
    crypto_sha512_hmac(I, chainCode, 32, data, sizeof data);
    wipe(data, sizeof data);
    wipe(chainCode, sizeof chainCode);
  }

  memcpy(privateKey, I, 32);
  wipe(I, sizeof I);
}

void publicKeyFromPrivate(const uint8_t privateKey[32], uint8_t publicKey[32]) {
  uint8_t seed[32];
  memcpy(seed, privateKey, 32);  // key_pair() wipes its input, so give it a copy
  uint8_t secretKey[64];
  crypto_ed25519_key_pair(secretKey, publicKey, seed);
  wipe(secretKey, sizeof secretKey);
}

void signMessage(const uint8_t privateKey[32], const uint8_t* message, size_t len,
                 uint8_t signature[64]) {
  uint8_t seed[32];
  memcpy(seed, privateKey, 32);
  uint8_t secretKey[64];
  uint8_t publicKey[32];
  crypto_ed25519_key_pair(secretKey, publicKey, seed);
  crypto_ed25519_sign(signature, secretKey, message, len);
  wipe(secretKey, sizeof secretKey);
}

void wipe(void* secret, size_t len) { crypto_wipe(secret, len); }
