#pragma once
#include <stddef.h>
#include <stdint.h>

// The "vault": the 64-byte seed, locked with a key that only the PIN can recreate.
//
//   PIN + salt  --Argon2id (slow on purpose)-->  32-byte key
//   key + seed  --XChaCha20-Poly1305-->          encrypted seed + 16-byte seal (MAC)
//
// The PIN itself is never stored anywhere. A wrong PIN gives a wrong key, the seal check
// fails, and that is how we know the PIN was wrong.
//
// Layout of the 123 bytes saved in flash (the "recipe is written on the box", so the
// Argon2 settings can change later without breaking vaults made earlier):
//
//   [0]       version (1)
//   [1]       Argon2 memory in KB (= blocks)
//   [2]       Argon2 passes
//   [3..18]   salt   (16 random bytes: two boards with the same PIN get different keys)
//   [19..42]  nonce  (24 random bytes: never lock twice with the same key + nonce)
//   [43..58]  MAC    (the tamper seal)
//   [59..122] encrypted seed
//
// No Arduino code in here, so the unit tests run it on your computer: pio test -e native

constexpr size_t kVaultSize = 123;
constexpr size_t kVaultRandomSize = 40;  // salt + nonce
constexpr size_t kPinMin = 4;
constexpr size_t kPinMax = 8;

// Locks `seed` with `pin` (digits only, kPinMin..kPinMax long).
// `random40` must be fresh random bytes (esp_fill_random on the board).
// Returns false if the PIN is not allowed or memory ran out.
bool vaultSeal(const uint8_t seed[64], const char* pin, const uint8_t random40[kVaultRandomSize],
               uint8_t vault[kVaultSize]);

// Opens the vault. true = right PIN, `seed` is filled in.
// false = wrong PIN, tampered vault, or no memory; `seed` is all zeros then.
bool vaultOpen(const uint8_t vault[kVaultSize], const char* pin, uint8_t seed[64]);

// True if this vault was locked with older (different) Argon2 settings than this firmware
// uses. After a successful unlock the board locks it again with the current ones.
bool vaultNeedsUpgrade(const uint8_t vault[kVaultSize]);

// How much memory vaultOpen() borrows for this vault, in bytes.
size_t vaultWorkBytes(const uint8_t vault[kVaultSize]);

// Digits only, kPinMin..kPinMax long.
bool pinAllowed(const char* pin);
