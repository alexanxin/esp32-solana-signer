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

// CYD new wallet: 32 fresh random bytes, which become the 24 recovery words (bip39.h).
void newEntropy(uint8_t entropy[32]);

// CYD, moving an old plain seed into the vault.
bool loadPlainSeed(uint8_t seed[64]);

// Locks `seed` with `pin` and saves it. Opens it again right away to be sure it worked,
// and only then deletes any old plain seed (losing the seed means losing the funds).
bool saveVault(const uint8_t seed[64], const char* pin);

// Wrong-PIN counter, kept in flash so pulling the power doesn't reset it.
// The 3rd wrong PIN in a row erases Sigly (vault, words flag, counter): someone who stole
// it gets 3 guesses, not thousands. You get your wallet back from your 24 recovery words.
constexpr int kMaxTries = 3;
int failedTries();
int triesLeft();  // before the wipe: 3, 2 or 1

enum class UnlockResult { kOk, kWrongPin, kWiped, kNoVault, kNoMemory };

// Checks `pin`. kOk: `seed` is filled in and the counter goes back to 0.
// kWiped: that was the last try, everything is erased (seedState() is kNone now).
UnlockResult unlockVault(const char* pin, uint8_t seed[64]);

// Erases the vault, any plain seed, the words flag and the counter.
void wipeEverything();

// Was the saved seed made from 24 recovery words the user wrote down?
// False for seeds from older firmware (random bytes, no words exist for them).
bool hasRecoveryWords();
void setHasRecoveryWords(bool yes);
