// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include <vector>

#include "tx_decode.h"

// Public key of the PUBLIC test phrase "abandon ... about", 44'/501'/0'/0' (devnet only).
static const uint8_t kPayer[32] = {0xf0, 0x36, 0x27, 0x62, 0x46, 0xa7, 0x5b, 0x9d, 0xe3, 0x34, 0x9e,
                                   0xd4, 0x2b, 0x15, 0xe2, 0x32, 0xf6, 0x51, 0x8f, 0xc2, 0x0f, 0x5f,
                                   0xcd, 0x4f, 0x1d, 0x64, 0xe8, 0x1f, 0x9b, 0xd2, 0x58, 0xf7};
static const uint8_t kComputeBudget[32] = {
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, 0xff, 0xec, 0xad, 0xba, 0x72, 0xc3, 0x9b, 0xe7,
    0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b, 0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00};
static const uint8_t kTokenProgram[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9};

using Bytes = std::vector<uint8_t>;

static void add(Bytes& m, const uint8_t* p, size_t n) { m.insert(m.end(), p, p + n); }
static void addLe(Bytes& m, uint64_t v, int n) {
  for (int i = 0; i < n; i++) m.push_back((v >> (8 * i)) & 0xff);
}

// Same shape as the transfer Phantom sent us on 2026-11-26: priority price, unit limit,
// then a 0.01 SOL System transfer. keys: payer, recipient, System, ComputeBudget[, Token].
static Bytes phantomTransfer(bool versioned, bool extraTokenInstruction) {
  Bytes m;
  if (versioned) m.push_back(0x80);
  m.push_back(1);  // signatures
  m.push_back(0);
  m.push_back(extraTokenInstruction ? 3 : 2);
  m.push_back(extraTokenInstruction ? 5 : 4);  // number of keys
  add(m, kPayer, 32);
  uint8_t recipient[32];
  memset(recipient, 0x11, 32);
  add(m, recipient, 32);
  uint8_t system[32] = {0};
  add(m, system, 32);
  add(m, kComputeBudget, 32);
  if (extraTokenInstruction) add(m, kTokenProgram, 32);
  uint8_t blockhash[32];
  memset(blockhash, 0xaa, 32);
  add(m, blockhash, 32);

  m.push_back(extraTokenInstruction ? 4 : 3);  // instructions
  m.push_back(3); m.push_back(0); m.push_back(9); m.push_back(3); addLe(m, 150000000, 8);
  m.push_back(3); m.push_back(0); m.push_back(5); m.push_back(2); addLe(m, 500, 4);
  m.push_back(2); m.push_back(2); m.push_back(0); m.push_back(1);
  m.push_back(12); addLe(m, 2, 4); addLe(m, 10000000, 8);
  if (extraTokenInstruction) { m.push_back(4); m.push_back(1); m.push_back(0); m.push_back(1); m.push_back(9); }
  if (versioned) m.push_back(0);  // no address lookup tables
  return m;
}

// One instruction to one program, with the payer as its only account.
// keys: payer, program[, ComputeBudget]. `unitPrice` > 0 adds a priority fee (500 000 units).
static Bytes singleInstruction(const uint8_t program[32], const Bytes& data, uint64_t unitPrice = 0) {
  Bytes m;
  m.push_back(1);
  m.push_back(0);
  m.push_back(unitPrice ? 2 : 1);
  m.push_back(unitPrice ? 3 : 2);
  add(m, kPayer, 32);
  add(m, program, 32);
  if (unitPrice) add(m, kComputeBudget, 32);
  uint8_t blockhash[32];
  memset(blockhash, 0xaa, 32);
  add(m, blockhash, 32);
  m.push_back(unitPrice ? 3 : 1);
  if (unitPrice) {
    m.push_back(2); m.push_back(0); m.push_back(9); m.push_back(3); addLe(m, unitPrice, 8);
    m.push_back(2); m.push_back(0); m.push_back(5); m.push_back(2); addLe(m, 500000, 4);
  }
  m.push_back(1); m.push_back(1); m.push_back(0);
  m.push_back((uint8_t)data.size());
  m.insert(m.end(), data.begin(), data.end());
  return m;
}

static uint8_t risksOf(const Bytes& m) {
  TxSummary s;
  TEST_ASSERT_TRUE(decodeTransaction(m.data(), m.size(), &s));
  return s.risks;
}

void setUp() {}
void tearDown() {}

static void test_sol_transfer() {
  Bytes m = phantomTransfer(false, false);
  TxSummary s;
  TEST_ASSERT_TRUE(decodeTransaction(m.data(), m.size(), &s));
  TEST_ASSERT_EQUAL(TxSummary::kSolTransfer, s.kind);
  TEST_ASSERT_EQUAL_UINT64(10000000, s.lamports);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kPayer, s.from, 32);
  TEST_ASSERT_EQUAL_HEX8(0x11, s.to[0]);
  TEST_ASSERT_EQUAL_UINT64(80000, s.feeLamports);  // 5000 + 150,000,000 x 500 / 1,000,000
  TEST_ASSERT_FALSE(s.versioned);
}

static void test_versioned_sol_transfer() {
  Bytes m = phantomTransfer(true, false);
  TxSummary s;
  TEST_ASSERT_TRUE(decodeTransaction(m.data(), m.size(), &s));
  TEST_ASSERT_EQUAL(TxSummary::kSolTransfer, s.kind);
  TEST_ASSERT_TRUE(s.versioned);
}

static void test_extra_instruction_is_unknown() {
  Bytes m = phantomTransfer(false, true);
  TxSummary s;
  TEST_ASSERT_TRUE(decodeTransaction(m.data(), m.size(), &s));
  TEST_ASSERT_EQUAL(TxSummary::kUnknown, s.kind);
  TEST_ASSERT_EQUAL_UINT(2, s.programCount);
  TEST_ASSERT_EQUAL_STRING("System Program", knownProgramName(s.programs[0]));
  TEST_ASSERT_EQUAL_STRING("Token Program", knownProgramName(s.programs[1]));
}

static void test_cut_off_message_is_invalid() {
  Bytes m = phantomTransfer(false, false);
  TxSummary s;
  for (size_t cut = 0; cut < m.size(); cut++) {  // every shorter length must be refused
    TEST_ASSERT_FALSE(decodeTransaction(m.data(), cut, &s));
  }
}

static void test_base58() {
  char text[45];
  base58Encode(kPayer, text);
  TEST_ASSERT_EQUAL_STRING("HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk", text);
  uint8_t zeros[32] = {0};
  base58Encode(zeros, text);
  TEST_ASSERT_EQUAL_STRING("11111111111111111111111111111111", text);
  base58Encode(kComputeBudget, text);
  TEST_ASSERT_EQUAL_STRING("ComputeBudget111111111111111111111111111111", text);
}

static void test_base58_signature_length() {
  uint8_t zeros[64] = {0};
  char out[90];
  base58EncodeBytes(zeros, 64, out, sizeof out);
  TEST_ASSERT_EQUAL(64, strlen(out));  // each leading zero byte is one '1'
  uint8_t ff[64];
  memset(ff, 0xFF, sizeof ff);
  base58EncodeBytes(ff, 64, out, sizeof out);
  TEST_ASSERT_EQUAL(88, strlen(out));  // the longest a signature can get
}

static void test_more_program_names() {
  // The 32-byte constants must turn back into the well-known base58 ids.
  struct { const char* b58; const char* name; } cases[] = {
      {"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4", "Jupiter Aggregator"},
      {"TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "Token-2022"},
      {"ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL", "Associated Token"},
      {"MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr", "Memo"},
  };
  for (auto& c : cases) {
    // Decode the base58 id here (test-only, the firmware never needs to decode).
    uint8_t key[32] = {0};
    static const char* A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    for (const char* p = c.b58; *p; p++) {
      int carry = (int)(strchr(A, *p) - A);
      for (int i = 31; i >= 0; i--) {
        carry += key[i] * 58;
        key[i] = carry & 0xFF;
        carry >>= 8;
      }
    }
    const char* name = knownProgramName(key);
    bool found = name != nullptr && strcmp(name, c.name) == 0;
    TEST_ASSERT_TRUE_MESSAGE(found, c.name);
    char back[45];
    base58Encode(key, back);
    TEST_ASSERT_EQUAL_STRING(c.b58, back);
  }
}

static void test_plain_transfer_has_no_risks() {
  Bytes m = phantomTransfer(false, false);
  TEST_ASSERT_EQUAL_HEX8(0, risksOf(m));
}

static void test_token_approve_is_flagged() {
  Bytes approve = {4};
  addLe(approve, 1000, 8);
  TEST_ASSERT_EQUAL_HEX8(kRiskApprove, risksOf(singleInstruction(kTokenProgram, approve)));
  Bytes checked = {13};
  addLe(checked, 1000, 8);
  checked.push_back(6);  // decimals
  TEST_ASSERT_EQUAL_HEX8(kRiskApprove, risksOf(singleInstruction(kTokenProgram, checked)));
}

static void test_token_set_authority_is_flagged() {
  Bytes data = {6, 2, 0};  // SetAuthority, AccountOwner, no new authority
  TEST_ASSERT_EQUAL_HEX8(kRiskAuthority, risksOf(singleInstruction(kTokenProgram, data)));
}

static void test_harmless_token_instruction_is_not_flagged() {
  Bytes transfer = {3};  // Transfer: not a red flag by itself (still "unknown" for now)
  addLe(transfer, 1000, 8);
  TEST_ASSERT_EQUAL_HEX8(0, risksOf(singleInstruction(kTokenProgram, transfer)));
}

static void test_system_assign_and_nonce_are_flagged() {
  uint8_t system[32] = {0};
  Bytes assign;
  addLe(assign, 1, 4);
  for (int i = 0; i < 32; i++) assign.push_back(0x22);  // new owner program
  TEST_ASSERT_EQUAL_HEX8(kRiskAssign, risksOf(singleInstruction(system, assign)));
  Bytes nonce;
  addLe(nonce, 4, 4);
  TEST_ASSERT_EQUAL_HEX8(kRiskNonce, risksOf(singleInstruction(system, nonce)));
  Bytes authorize;
  addLe(authorize, 7, 4);
  for (int i = 0; i < 32; i++) authorize.push_back(0x33);
  TEST_ASSERT_EQUAL_HEX8(kRiskAuthority, risksOf(singleInstruction(system, authorize)));
}

static void test_high_fee_is_flagged() {
  Bytes memo = {'h', 'i'};
  uint8_t memoProgram[32];
  memset(memoProgram, 0x44, 32);
  // 500 000 units x 10 000 micro-lamports = 5000 lamports: normal.
  TEST_ASSERT_EQUAL_HEX8(0, risksOf(singleInstruction(memoProgram, memo, 10000)));
  // 500 000 units x 100 000 000 micro-lamports = 0.05 SOL: flagged.
  TEST_ASSERT_EQUAL_HEX8(kRiskHighFee, risksOf(singleInstruction(memoProgram, memo, 100000000)));
}

static void test_only_plain_transfers_skip_blind_signing() {
  TxSummary s;
  Bytes m = phantomTransfer(false, false);
  decodeTransaction(m.data(), m.size(), &s);
  TEST_ASSERT_FALSE(needsBlindSigning(s));
  m = phantomTransfer(false, true);
  decodeTransaction(m.data(), m.size(), &s);
  TEST_ASSERT_TRUE(needsBlindSigning(s));
  uint8_t junk[3] = {1, 2, 3};
  decodeTransaction(junk, sizeof junk, &s);
  TEST_ASSERT_TRUE(needsBlindSigning(s));  // not a transaction: never "safe"
}

static void test_risk_texts() {
  for (uint8_t bit = 1; bit <= kRiskHighFee; bit <<= 1) {
    TEST_ASSERT_TRUE(strlen(riskText((TxRisk)bit)) > 0);
    TEST_ASSERT_TRUE(strlen(riskText((TxRisk)bit)) <= 40);  // fits one line on the CYD
  }
}

static void test_format_sol() {
  char text[24];
  formatSol(10000000, text);
  TEST_ASSERT_EQUAL_STRING("0.01", text);
  formatSol(2000000000, text);
  TEST_ASSERT_EQUAL_STRING("2", text);
  formatSol(80000, text);
  TEST_ASSERT_EQUAL_STRING("0.00008", text);
  formatSol(1, text);
  TEST_ASSERT_EQUAL_STRING("0.000000001", text);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sol_transfer);
  RUN_TEST(test_versioned_sol_transfer);
  RUN_TEST(test_extra_instruction_is_unknown);
  RUN_TEST(test_cut_off_message_is_invalid);
  RUN_TEST(test_base58);
  RUN_TEST(test_base58_signature_length);
  RUN_TEST(test_more_program_names);
  RUN_TEST(test_format_sol);
  RUN_TEST(test_plain_transfer_has_no_risks);
  RUN_TEST(test_token_approve_is_flagged);
  RUN_TEST(test_token_set_authority_is_flagged);
  RUN_TEST(test_harmless_token_instruction_is_not_flagged);
  RUN_TEST(test_system_assign_and_nonce_are_flagged);
  RUN_TEST(test_high_fee_is_flagged);
  RUN_TEST(test_risk_texts);
  RUN_TEST(test_only_plain_transfers_skip_blind_signing);
  return UNITY_END();
}
