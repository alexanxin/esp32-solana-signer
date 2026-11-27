#pragma once
#include <stddef.h>
#include <stdint.h>

// Reads a Solana transaction message (the bytes the wallet asks us to sign) and pulls out
// what a human needs to see before approving: what kind of transaction, how much, to whom,
// and the network fee.
//
// For now we understand one kind: a plain SOL transfer (System Program "Transfer"), with or
// without the Compute Budget instructions wallets add for priority fees. Everything else is
// "unknown": we still list which programs it talks to, and the screen warns the human.

constexpr size_t kMaxListedPrograms = 4;

// A priority fee above this is almost always a trick (or a mistake): 0.01 SOL.
constexpr uint64_t kHighFeeLamports = 10000000;

// Red flags: instructions that can hand your funds to someone else without a plain
// "send X SOL to Y". We spot them when the transaction calls the program directly.
// A program can still do these things inside itself (a "CPI"), which we can't see:
// that's why unknown transactions stay blocked unless you turn them on in Settings.
enum TxRisk : uint8_t {
  kRiskApprove = 1 << 0,    // Token Approve: someone else may spend your tokens later
  kRiskAuthority = 1 << 1,  // Token SetAuthority / nonce authority: hands over control
  kRiskAssign = 1 << 2,     // System Assign: gives your account to another program
  kRiskNonce = 1 << 3,      // durable nonce: the signature never expires
  kRiskHighFee = 1 << 4,    // fee above kHighFeeLamports
};

struct TxSummary {
  enum Kind { kUnknown, kSolTransfer };

  bool valid;            // false = the bytes don't look like a Solana message at all
  Kind kind;
  bool versioned;        // "v0" message (may use address lookup tables)

  // kSolTransfer only:
  uint64_t lamports;     // 1 SOL = 1,000,000,000 lamports
  uint8_t from[32];
  uint8_t to[32];

  uint64_t feeLamports;  // base fee (5000 per signature) + priority fee
  uint8_t risks;         // TxRisk bits, 0 = none spotted

  // Programs the transaction talks to (Compute Budget left out), for the "unknown" screen.
  size_t programCount;   // how many distinct programs in total
  uint8_t programs[kMaxListedPrograms][32];
};

bool decodeTransaction(const uint8_t* message, size_t len, TxSummary* out);

// True when Sigly can't show what this does (anything but a plain SOL transfer, or bytes
// that aren't a transaction at all). Signing it is "blind signing": off unless the user
// turned it on in Settings.
inline bool needsBlindSigning(const TxSummary& tx) {
  return !(tx.valid && tx.kind == TxSummary::kSolTransfer);
}

// One short line for a risk bit, for the screen ("Lets someone spend your tokens").
const char* riskText(TxRisk risk);

// "System Program", "Token Program", ... or nullptr if we don't know it.
const char* knownProgramName(const uint8_t id[32]);

// Solana address text (base58). out must hold 45 bytes (44 letters + the ending zero).
void base58Encode(const uint8_t key[32], char out[45]);
// Any length up to 64 bytes (a signature is 64 -> up to 88 letters). outSize must be >= 90.
void base58EncodeBytes(const uint8_t* data, size_t len, char* out, size_t outSize);

// "0.01" from 10,000,000 lamports. out must hold 24 bytes.
void formatSol(uint64_t lamports, char out[24]);
