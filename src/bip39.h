#pragma once
#include <stddef.h>
#include <stdint.h>

// Recovery words (BIP39), the same 24 words Phantom, Solflare and Ledger use.
//
//   32 random bytes ("entropy", 256 bits)
//     + 8 checksum bits (the first byte of SHA-256 of those 32 bytes)
//     = 264 bits = 24 pieces of 11 bits. Each 11-bit number (0..2047) picks one word.
//
//   the 24 words, as text  --PBKDF2-HMAC-SHA512, 2048 rounds-->  the 64-byte seed
//
// The seed then goes to keys.cpp (SLIP-10, m/44'/501'/...), exactly as before.
// The second step is one-way: words give the seed, but a seed never gives words back.
//
// Words are passed around as their numbers in the list (0..2047), not as text, so a
// word can never be misspelled inside the firmware.
//
// No Arduino code in here, so the unit tests run it on your computer: pio test -e native

constexpr int kWordCount = 24;
constexpr size_t kEntropySize = 32;

// Word number `index` (0..2047), e.g. 0 -> "abandon". nullptr if out of range.
const char* bip39Word(int index);

// The word's number, or -1 if it is not in the list.
int bip39Find(const char* word);

// For typing words in: how many words start with `prefix`, and the number of the first
// one (the list is sorted, so they sit next to each other). "" matches all 2048.
int bip39Matches(const char* prefix, int* first);

// Which letters can come after `prefix` and still lead to a word.
// Bit 0 = 'a', bit 1 = 'b', ... bit 25 = 'z'.
uint32_t bip39NextLetters(const char* prefix);

// 32 random bytes -> 24 word numbers (the last word carries the checksum).
void bip39FromEntropy(const uint8_t entropy[kEntropySize], uint16_t words[kWordCount]);

// True if the 24 words fit together (the checksum in the last word matches).
// A typo in any word fails this 255 times out of 256.
bool bip39ChecksumOk(const uint16_t words[kWordCount]);

// Words -> 64-byte seed. passphrase: "" (Sigly has no passphrase, "25th word", for now).
// Slow on purpose (2048 rounds): about a second on the ESP32.
void bip39ToSeed(const uint16_t words[kWordCount], const char* passphrase, uint8_t seed[64]);

// The same for any phrase given as text ("abandon abandon ... about", 12 or 24 words).
// Used by the unit tests to check against the published test vectors.
void bip39PhraseToSeed(const char* phrase, const char* passphrase, uint8_t seed[64]);

// SHA-256, needed for the checksum (Monocypher only has SHA-512). Public for its own test.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
