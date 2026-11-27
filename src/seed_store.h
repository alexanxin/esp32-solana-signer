#pragma once
#include <stdint.h>

// Where the board's 64-byte seed lives in flash ("NVS", namespace "signer").
//
//   "vault"  the seed locked with your PIN (pin_vault.h). The CYD uses this.
//   "seed"   the seed in plain bytes. Older firmware saved it like this; the C3 still
//            does, because it has no screen to type a PIN on.
//
// Every function here that touches the random generator must run AFTER Bluetooth is
// started: the ESP32's random generator is only truly random while the radio is on.

enum class SeedState {
  kNone,   // first boot: nothing saved yet
  kPlain,  // an old plain seed: the CYD moves it into a vault once you choose a PIN
  kVault,  // locked with a PIN
};

SeedState seedState();

// C3 (no screen): loads the plain seed, or makes and saves one on the first boot.
// Returns false if there is still no seed (then the board refuses all key requests).
bool loadOrCreateSeed(uint8_t seed[64]);

// CYD first boot: a fresh random seed, kept in RAM only until saveVault() stores it.
void newRandomSeed(uint8_t seed[64]);

// CYD, moving an old plain seed into the vault.
bool loadPlainSeed(uint8_t seed[64]);

// Locks `seed` with `pin` and saves it. Opens it again right away to be sure it worked,
// and only then deletes any old plain seed (losing the seed means losing the funds).
bool saveVault(const uint8_t seed[64], const char* pin);

// Wrong-PIN counter, kept in flash so pulling the power doesn't reset it.
// After kFreeTries wrong PINs, every next try has to wait a bit longer (no wipe yet:
// there is no backup of the seed, so wiping would lose the funds for good).
constexpr int kFreeTries = 5;
int failedTries();
uint32_t waitBeforeNextTryMs();  // 0 = you may try now

enum class UnlockResult { kOk, kWrongPin, kMustWait, kNoVault, kNoMemory };

// Checks `pin`. kOk: `seed` is filled in and the counter goes back to 0.
UnlockResult unlockVault(const char* pin, uint8_t seed[64]);
