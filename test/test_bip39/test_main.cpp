// Runs on your computer, no board needed:  pio test -e native
//
// The expected words and seeds are the published BIP39 test vectors (Trezor's
// vectors.json, passphrase "TREZOR"), double-checked with Python's hashlib. If these pass,
// words written down from Sigly give the same wallet in Phantom, Solflare or a Ledger.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "bip39.h"
#include "keys.h"
#include "tx_decode.h"  // base58Encode

void setUp() {}
void tearDown() {}

static void hexToBytes(const char* hex, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

// The 24 words as one string, to compare with the vectors.
static void phraseOf(const uint16_t words[kWordCount], char* out) {
  out[0] = '\0';
  for (int i = 0; i < kWordCount; i++) {
    if (i > 0) strcat(out, " ");
    strcat(out, bip39Word(words[i]));
  }
}

struct Vector {
  const char* entropy;
  const char* phrase;
  const char* seedTrezor;
};

static const Vector kVectors[] = {
    {"0000000000000000000000000000000000000000000000000000000000000000",
     "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
     "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
     "abandon art",
     "bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd3097170af7a4d73245cafa9c3cca8d5"
     "61a7c3de6f5d4a10be8ed2a5e608d68f92fcc8"},
    {"7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
     "legal winner thank year wave sausage worth useful legal winner thank year wave sausage "
     "worth useful legal winner thank year wave sausage worth title",
     "bc09fca1804f7e69da93c2f2028eb238c227f2e9dda30cd63699232578480a4021b146ad717fbb7e451ce9eb83"
     "5f43620bf5c514db0f8add49f5d121449d3e87"},
    {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo "
     "vote",
     "dd48c104698c30cfe2b6142103248622fb7bb0ff692eebb00089b32d22484e1613912f0a5b694407be899ffd31"
     "ed3992c456cdf60f5d4564b8ba3f05a69890ad"},
    {"68a79eaca2324873eacc50cb9c6eca8cc68ea5d936f98787c60c7ebc74e6ce7c",
     "hamster diagram private dutch cause delay private meat slide toddler razor book happy "
     "fancy gospel tennis maple dilemma loan word shrug inflict delay length",
     "64c87cde7e12ecf6704ab95bb1408bef047c22db4cc7491c4271d170a1b213d20b385bc1588d9c7b38f1b39d41"
     "5665b8a9030c9ec653d75e65f847d8fc1fc440"},
};

static void test_sha256() {
  uint8_t out[32], want[32];
  sha256((const uint8_t*)"abc", 3, out);
  hexToBytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 32);

  uint8_t hundred[100];
  memset(hundred, 'a', sizeof hundred);  // longer than one 64-byte block
  sha256(hundred, sizeof hundred, out);
  hexToBytes("2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e", want, 32);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 32);
}

// Random bytes -> the right 24 words, and those words pass the checksum.
static void test_entropy_to_words() {
  for (const Vector& v : kVectors) {
    uint8_t entropy[32];
    hexToBytes(v.entropy, entropy, 32);
    uint16_t words[kWordCount];
    bip39FromEntropy(entropy, words);
    char phrase[kWordCount * 9];
    phraseOf(words, phrase);
    TEST_ASSERT_EQUAL_STRING(v.phrase, phrase);
    TEST_ASSERT_TRUE(bip39ChecksumOk(words));
  }
}

// Words -> the right 64-byte seed.
static void test_words_to_seed() {
  for (const Vector& v : kVectors) {
    uint8_t entropy[32], want[64], seed[64];
    hexToBytes(v.entropy, entropy, 32);
    hexToBytes(v.seedTrezor, want, 64);
    uint16_t words[kWordCount];
    bip39FromEntropy(entropy, words);
    bip39ToSeed(words, "TREZOR", seed);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, seed, 64);
  }
}

// Sigly uses no passphrase. Checked with Python: hashlib.pbkdf2_hmac('sha512', phrase,
// b'mnemonic', 2048).
static void test_no_passphrase() {
  uint8_t want[64], seed[64];
  hexToBytes("408b285c123836004f4b8842c89324c1f01382450c0d439af345ba7fc49acf705489c6fc77dbd4"
             "e3dc1dd8cc6bc9f043db8ada1e243c4a0eafb290d399480840",
             want, 64);
  bip39PhraseToSeed(kVectors[0].phrase, "", seed);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, seed, 64);
}

// One wrong word (a typo while restoring) is caught by the checksum.
static void test_checksum_catches_a_wrong_word() {
  uint8_t entropy[32];
  hexToBytes(kVectors[3].entropy, entropy, 32);
  uint16_t words[kWordCount];
  bip39FromEntropy(entropy, words);
  int caught = 0;
  for (int pos = 0; pos < kWordCount; pos++) {
    uint16_t saved = words[pos];
    words[pos] = (saved + 1) % 2048;  // the next word in the list instead
    if (!bip39ChecksumOk(words)) caught++;
    words[pos] = saved;
  }
  // Each swap slips through 1 time in 256, so all 24 caught is what we expect here.
  TEST_ASSERT_EQUAL_INT(kWordCount, caught);
}

static void test_find_and_prefix() {
  TEST_ASSERT_EQUAL_INT(0, bip39Find("abandon"));
  TEST_ASSERT_EQUAL_INT(2047, bip39Find("zoo"));
  TEST_ASSERT_EQUAL_INT(-1, bip39Find("sigly"));
  TEST_ASSERT_EQUAL_STRING("zoo", bip39Word(2047));
  TEST_ASSERT_NULL(bip39Word(2048));

  int first;
  TEST_ASSERT_EQUAL_INT(2048, bip39Matches("", &first));
  TEST_ASSERT_EQUAL_INT(0, first);
  TEST_ASSERT_EQUAL_INT(1, bip39Matches("zoo", &first));
  TEST_ASSERT_EQUAL_INT(2047, first);
  TEST_ASSERT_EQUAL_INT(0, bip39Matches("qz", &first));

  // After "q" only "qu..." words exist (quality, quantum, quarter, question, quick, ...).
  TEST_ASSERT_EQUAL_HEX32(1u << ('u' - 'a'), bip39NextLetters("q"));
  // No word starts with "x".
  TEST_ASSERT_EQUAL_HEX32(0, bip39NextLetters("x"));
}

// The whole chain, words -> seed -> SLIP-10 -> Solana address, gives the address Phantom
// shows for these words (checked in Python with hashlib + the cryptography package).
static void test_words_to_phantom_address() {
  uint16_t words[kWordCount];
  uint8_t entropy[32];
  hexToBytes(kVectors[3].entropy, entropy, 32);
  bip39FromEntropy(entropy, words);
  uint8_t seed[64], priv[32], pub[32];
  bip39ToSeed(words, "", seed);
  setSeed(seed);
  const uint32_t account1[] = {44, 501, 0, 0};
  deriveKey(account1, 4, priv);
  publicKeyFromPrivate(priv, pub);
  clearSeed();
  char addr[45];
  base58Encode(pub, addr);
  TEST_ASSERT_EQUAL_STRING("3bDiE8194MYWrhhFQyZcwnx9DB5ZV8LYhQfFuz4e58hY", addr);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256);
  RUN_TEST(test_entropy_to_words);
  RUN_TEST(test_words_to_seed);
  RUN_TEST(test_no_passphrase);
  RUN_TEST(test_checksum_catches_a_wrong_word);
  RUN_TEST(test_find_and_prefix);
  RUN_TEST(test_words_to_phantom_address);
  return UNITY_END();
}
