#include "tx_decode.h"

#include <stdio.h>
#include <string.h>

namespace {

// Program ids are 32-byte public keys. These are the well-known ones, as raw bytes.
const uint8_t kSystemProgram[32] = {0};  // 11111111111111111111111111111111
const uint8_t kComputeBudget[32] = {
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, 0xff, 0xec, 0xad, 0xba, 0x72, 0xc3, 0x9b, 0xe7,
    0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b, 0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00};
const uint8_t kTokenProgram[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9};

constexpr uint64_t kLamportsPerSignature = 5000;
constexpr uint64_t kDefaultUnitsPerInstruction = 200000;
constexpr uint64_t kMaxUnits = 1400000;

// A tiny cursor over the message bytes. Every read checks that the bytes are really there,
// so a broken or hostile message can never make us read past the end.
struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok;

  uint8_t u8() {
    if (left < 1) { ok = false; return 0; }
    left--;
    return *p++;
  }
  const uint8_t* bytes(size_t n) {
    if (left < n) { ok = false; return nullptr; }
    const uint8_t* start = p;
    p += n;
    left -= n;
    return start;
  }
  // Solana's "compact-u16": 7 bits per byte, high bit = "another byte follows".
  uint16_t compactU16() {
    uint32_t value = 0;
    for (int shift = 0; shift < 21; shift += 7) {
      uint8_t b = u8();
      value |= (uint32_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) break;
    }
    if (value > 0xffff) ok = false;
    return (uint16_t)value;
  }
};

uint64_t readLe(const uint8_t* p, int n) {  // little-endian: lowest byte first
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

}  // namespace

const char* knownProgramName(const uint8_t id[32]) {
  if (memcmp(id, kSystemProgram, 32) == 0) return "System Program";
  if (memcmp(id, kTokenProgram, 32) == 0) return "Token Program";
  if (memcmp(id, kComputeBudget, 32) == 0) return "Compute Budget";
  return nullptr;
}

bool decodeTransaction(const uint8_t* message, size_t len, TxSummary* out) {
  memset(out, 0, sizeof *out);
  out->kind = TxSummary::kUnknown;
  Reader r{message, len, true};

  // Header. A first byte with the top bit set means "versioned message" (v0).
  uint8_t first = r.u8();
  if (first & 0x80) {
    if ((first & 0x7f) != 0) return false;  // only v0 exists today
    out->versioned = true;
    first = r.u8();
  }
  uint8_t numSignatures = first;
  r.u8();  // read-only signed accounts (not needed here)
  r.u8();  // read-only unsigned accounts

  uint16_t numKeys = r.compactU16();
  const uint8_t* keys = r.bytes((size_t)numKeys * 32);
  r.bytes(32);  // recent blockhash
  uint16_t numInstructions = r.compactU16();
  if (!r.ok || numSignatures == 0 || numKeys == 0) return false;

  uint64_t unitPrice = 0;  // micro-lamports per compute unit
  uint64_t unitLimit = 0;  // 0 = not set
  size_t otherInstructions = 0;
  bool lookupNeeded = false;
  bool transferSeen = false;
  bool transferOk = false;

  for (uint16_t i = 0; i < numInstructions; i++) {
    uint8_t programIndex = r.u8();
    uint16_t numAccounts = r.compactU16();
    const uint8_t* accounts = r.bytes(numAccounts);
    uint16_t dataLen = r.compactU16();
    const uint8_t* data = r.bytes(dataLen);
    if (!r.ok) return false;

    if (programIndex >= numKeys) {  // program comes from a lookup table: can't name it
      lookupNeeded = true;
      otherInstructions++;
      continue;
    }
    const uint8_t* program = keys + programIndex * 32;

    if (memcmp(program, kComputeBudget, 32) == 0) {
      if (dataLen == 5 && data[0] == 2) unitLimit = readLe(data + 1, 4);  // SetComputeUnitLimit
      if (dataLen == 9 && data[0] == 3) unitPrice = readLe(data + 1, 8);  // SetComputeUnitPrice
      continue;
    }

    otherInstructions++;

    // Remember each distinct program, for the "unknown transaction" screen.
    bool listed = false;
    for (size_t k = 0; k < out->programCount && k < kMaxListedPrograms; k++) {
      if (memcmp(out->programs[k], program, 32) == 0) listed = true;
    }
    if (!listed) {
      if (out->programCount < kMaxListedPrograms) memcpy(out->programs[out->programCount], program, 32);
      out->programCount++;
    }

    // System Program "Transfer": data = [2,0,0,0][lamports u64], accounts = [from, to].
    if (memcmp(program, kSystemProgram, 32) == 0 && dataLen == 12 && readLe(data, 4) == 2 &&
        numAccounts == 2) {
      transferSeen = true;
      if (accounts[0] < numKeys && accounts[1] < numKeys) {
        out->lamports = readLe(data + 4, 8);
        memcpy(out->from, keys + accounts[0] * 32, 32);
        memcpy(out->to, keys + accounts[1] * 32, 32);
        transferOk = true;
      }
    }
  }
  if (!r.ok) return false;
  out->valid = true;

  // A SOL transfer only if the transfer is the ONE thing this transaction does.
  if (transferSeen && transferOk && otherInstructions == 1 && !lookupNeeded) {
    out->kind = TxSummary::kSolTransfer;
  }

  // Fee = 5000 lamports per signature + priority fee (price x units, price in micro-lamports).
  if (unitLimit == 0) unitLimit = otherInstructions * kDefaultUnitsPerInstruction;
  if (unitLimit > kMaxUnits) unitLimit = kMaxUnits;
  uint64_t priority = (unitPrice * unitLimit + 999999) / 1000000;  // round up, like the network
  out->feeLamports = numSignatures * kLamportsPerSignature + priority;
  return true;
}

void base58Encode(const uint8_t key[32], char out[45]) {
  static const char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t digits[45] = {0};  // base-58 digits, lowest first
  size_t count = 0;
  for (int i = 0; i < 32; i++) {  // "multiply by 256 and add the next byte", in base 58
    uint32_t carry = key[i];
    for (size_t j = 0; j < count; j++) {
      carry += (uint32_t)digits[j] << 8;
      digits[j] = carry % 58;
      carry /= 58;
    }
    while (carry) {
      digits[count++] = carry % 58;
      carry /= 58;
    }
  }
  size_t n = 0;
  for (int i = 0; i < 32 && key[i] == 0; i++) out[n++] = '1';  // each leading zero byte = '1'
  for (size_t j = count; j > 0; j--) out[n++] = kAlphabet[digits[j - 1]];
  out[n] = '\0';
}

void formatSol(uint64_t lamports, char out[24]) {
  uint64_t whole = lamports / 1000000000ull;
  uint64_t frac = lamports % 1000000000ull;
  if (frac == 0) {
    snprintf(out, 24, "%llu", (unsigned long long)whole);
    return;
  }
  char fracText[10];
  snprintf(fracText, sizeof fracText, "%09llu", (unsigned long long)frac);
  for (int i = 8; i > 0 && fracText[i] == '0'; i--) fracText[i] = '\0';  // drop trailing zeros
  snprintf(out, 24, "%llu.%s", (unsigned long long)whole, fracText);
}
