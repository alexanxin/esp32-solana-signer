// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "apdu.h"
#include "keys.h"
#include "monocypher-ed25519.h"

// BIP39 seed of the PUBLIC test phrase "abandon ... about". Tests only.
static const uint8_t kTestSeed[64] = {
    0x5e, 0xb0, 0x0b, 0xbd, 0xdc, 0xf0, 0x69, 0x08,
    0x48, 0x89, 0xa8, 0xab, 0x91, 0x55, 0x56, 0x81,
    0x65, 0xf5, 0xc4, 0x53, 0xcc, 0xb8, 0x5e, 0x70,
    0x81, 0x1a, 0xae, 0xd6, 0xf6, 0xda, 0x5f, 0xc1,
    0x9a, 0x5a, 0xc4, 0x0b, 0x38, 0x9c, 0xd3, 0x70,
    0xd0, 0x86, 0x20, 0x6d, 0xec, 0x8a, 0xa6, 0xc4,
    0x3d, 0xae, 0xa6, 0x69, 0x0f, 0x20, 0xad, 0x3d,
    0x8d, 0x48, 0xb2, 0xd2, 0xce, 0x9e, 0x38, 0xe4
};

void setUp() { setSeed(kTestSeed); }
void tearDown() {}

static void test_app_and_version() {
  const uint8_t expected[] = {0x01, 0x06, 'S', 'o', 'l', 'a', 'n', 'a', 0x06, '1', '.',
                              '1',  '6',  '.', '0', 0x01, 0x00, 0x90, 0x00};
  uint8_t out[64];
  size_t n = buildAppAndVersion(out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(sizeof expected, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof expected);
}

static void test_app_and_version_buffer_too_small() {
  uint8_t out[8];
  TEST_ASSERT_EQUAL_UINT(0, buildAppAndVersion(out, sizeof out));
}

static void test_app_configuration() {
  const uint8_t cmd[] = {0xe0, 0x04, 0x00, 0x00, 0x00};
  const uint8_t expected[] = {0x01, 0x00, 1, 16, 0, 0x90, 0x00};
  uint8_t out[64];
  size_t n = handleApdu(cmd, sizeof cmd, out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(sizeof expected, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof expected);
}

// Same addresses Phantom shows after importing "abandon ... about" (devnet!).
static void test_public_key_matches_phantom() {
  // 44'/501'/0'/0'  ->  HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk
  const uint8_t cmd[] = {0xe0, 0x05, 0x00, 0x00, 17,   4,    0x80, 0x00, 0x00, 0x2c, 0x80,
                         0x00, 0x01, 0xf5, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};
  const uint8_t expected[32] = {0xf0, 0x36, 0x27, 0x62, 0x46, 0xa7, 0x5b, 0x9d, 0xe3, 0x34, 0x9e,
                                0xd4, 0x2b, 0x15, 0xe2, 0x32, 0xf6, 0x51, 0x8f, 0xc2, 0x0f, 0x5f,
                                0xcd, 0x4f, 0x1d, 0x64, 0xe8, 0x1f, 0x9b, 0xd2, 0x58, 0xf7};
  uint8_t out[64];
  size_t n = handleApdu(cmd, sizeof cmd, out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(34, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 32);
}

static void test_public_key_short_path() {
  // 44'/501'  ->  D2PPQSYFe83nDzk96FqGumVU8JA7J8vj2Rhjc2oXzEi5 (starts b2 a7 22 dc)
  const uint8_t cmd[] = {0xe0, 0x05, 0x00, 0x00, 9, 2, 0x80, 0x00, 0x00, 0x2c, 0x80, 0x00, 0x01, 0xf5};
  uint8_t out[64];
  TEST_ASSERT_EQUAL_UINT(34, handleApdu(cmd, sizeof cmd, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(0xb2, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xa7, out[1]);
}

static bool alwaysYes(const uint8_t*, size_t) { return true; }
static bool alwaysNo(const uint8_t*, size_t) { return false; }

// e0 06 01 00 <Lc> [01][04 path 44'/501'/0'/0'][message]
static size_t buildSign(uint8_t p2, const uint8_t* msg, size_t msgLen, uint8_t* cmd) {
  const uint8_t head[] = {0xe0, 0x06, 0x01, p2, 0, 0x01, 0x04, 0x80, 0x00, 0x00, 0x2c, 0x80, 0x00,
                          0x01, 0xf5, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};
  memcpy(cmd, head, sizeof head);
  memcpy(cmd + sizeof head, msg, msgLen);
  cmd[4] = (uint8_t)(sizeof head - 5 + msgLen);
  return sizeof head + msgLen;
}

static const uint8_t kPubkey[32] = {0xf0, 0x36, 0x27, 0x62, 0x46, 0xa7, 0x5b, 0x9d, 0xe3, 0x34, 0x9e,
                                    0xd4, 0x2b, 0x15, 0xe2, 0x32, 0xf6, 0x51, 0x8f, 0xc2, 0x0f, 0x5f,
                                    0xcd, 0x4f, 0x1d, 0x64, 0xe8, 0x1f, 0x9b, 0xd2, 0x58, 0xf7};

static void test_sign_is_refused_without_approval() {
  setApprovalCallback(alwaysNo);
  const uint8_t msg[] = {1, 2, 3};
  uint8_t cmd[64], out[80];
  size_t n = handleApdu(cmd, buildSign(0x00, msg, sizeof msg, cmd), out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_HEX8(0x69, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0x85, out[1]);
}

static void test_sign_produces_valid_signature() {
  setApprovalCallback(alwaysYes);
  const uint8_t msg[] = "pretend this is a Solana transaction message";
  uint8_t cmd[128], out[80];
  size_t n = handleApdu(cmd, buildSign(0x00, msg, sizeof msg, cmd), out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(66, n);
  TEST_ASSERT_EQUAL_HEX8(0x90, out[64]);
  // 0 = the signature checks out against HAgk14...'s public key
  TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_check(out, kPubkey, msg, sizeof msg));
}

static void test_sign_in_two_parts() {
  setApprovalCallback(alwaysYes);
  const uint8_t whole[] = "first half|second half";
  uint8_t cmd[128], out[80];
  // part 1: "more" bit (0x02), carries the path + first 11 bytes
  size_t n = handleApdu(cmd, buildSign(0x02, whole, 11, cmd), out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_HEX8(0x90, out[0]);
  // part 2: "extend" bit (0x01), only message bytes
  uint8_t cmd2[64] = {0xe0, 0x06, 0x01, 0x01, (uint8_t)(sizeof whole - 11)};
  memcpy(cmd2 + 5, whole + 11, sizeof whole - 11);
  n = handleApdu(cmd2, 5 + sizeof whole - 11, out, sizeof out);
  TEST_ASSERT_EQUAL_UINT(66, n);
  TEST_ASSERT_EQUAL_INT(0, crypto_ed25519_check(out, kPubkey, whole, sizeof whole));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_app_and_version);
  RUN_TEST(test_app_and_version_buffer_too_small);
  RUN_TEST(test_app_configuration);
  RUN_TEST(test_public_key_matches_phantom);
  RUN_TEST(test_public_key_short_path);
  RUN_TEST(test_sign_is_refused_without_approval);
  RUN_TEST(test_sign_produces_valid_signature);
  RUN_TEST(test_sign_in_two_parts);
  return UNITY_END();
}
