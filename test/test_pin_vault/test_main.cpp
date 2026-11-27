// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "pin_vault.h"

// Made-up seed and "random" bytes, so every run is the same. Tests only.
static uint8_t gSeed[64];
static uint8_t gRandom[kVaultRandomSize];
static uint8_t gVault[kVaultSize];

void setUp() {
  for (int i = 0; i < 64; i++) gSeed[i] = (uint8_t)(i * 7 + 1);
  for (int i = 0; i < (int)kVaultRandomSize; i++) gRandom[i] = (uint8_t)(200 - i);
  TEST_ASSERT_TRUE(vaultSeal(gSeed, "123456", gRandom, gVault));
}
void tearDown() {}

// The vault must not contain the seed in readable form.
static void test_seal_hides_the_seed() {
  TEST_ASSERT_EQUAL_UINT8(1, gVault[0]);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(gVault + 59, gSeed, 64));
}

// Your TODO: the right PIN gives back the exact seed.
static void test_right_pin_opens() {
  uint8_t out[64];
  TEST_ASSERT_TRUE_MESSAGE(vaultOpen(gVault, "123456", out),
                           "right PIN refused: finish the TODO in pin_vault.cpp");
  TEST_ASSERT_EQUAL_HEX8_ARRAY(gSeed, out, 64);
}

// A wrong PIN is refused and leaves nothing behind.
static void test_wrong_pin_refused() {
  uint8_t out[64];
  memset(out, 0xAA, sizeof out);
  TEST_ASSERT_FALSE(vaultOpen(gVault, "123457", out));
  uint8_t zeros[64] = {0};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(zeros, out, 64);
}

// Flip one bit of the stored vault: the seal check catches it.
static void test_tampered_vault_refused() {
  uint8_t out[64];
  gVault[80] ^= 0x01;
  TEST_ASSERT_FALSE(vaultOpen(gVault, "123456", out));
}

// Weakening the Argon2 settings in the header is caught too (they are sealed).
static void test_weaker_settings_refused() {
  uint8_t out[64];
  gVault[2] = 1;  // passes 8 -> 1
  TEST_ASSERT_FALSE(vaultOpen(gVault, "123456", out));
}

static void test_pin_rules() {
  TEST_ASSERT_TRUE(pinAllowed("1234"));
  TEST_ASSERT_TRUE(pinAllowed("12345678"));
  TEST_ASSERT_FALSE(pinAllowed("123"));
  TEST_ASSERT_FALSE(pinAllowed("123456789"));
  TEST_ASSERT_FALSE(pinAllowed("12a4"));
  uint8_t v[kVaultSize];
  TEST_ASSERT_FALSE(vaultSeal(gSeed, "12", gRandom, v));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_seal_hides_the_seed);
  RUN_TEST(test_right_pin_opens);
  RUN_TEST(test_wrong_pin_refused);
  RUN_TEST(test_tampered_vault_refused);
  RUN_TEST(test_weaker_settings_refused);
  RUN_TEST(test_pin_rules);
  return UNITY_END();
}
