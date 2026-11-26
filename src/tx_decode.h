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

  // Programs the transaction talks to (Compute Budget left out), for the "unknown" screen.
  size_t programCount;   // how many distinct programs in total
  uint8_t programs[kMaxListedPrograms][32];
};

bool decodeTransaction(const uint8_t* message, size_t len, TxSummary* out);

// "System Program", "Token Program", ... or nullptr if we don't know it.
const char* knownProgramName(const uint8_t id[32]);

// Solana address text (base58). out must hold 45 bytes (44 letters + the ending zero).
void base58Encode(const uint8_t key[32], char out[45]);

// "0.01" from 10,000,000 lamports. out must hold 24 bytes.
void formatSol(uint64_t lamports, char out[24]);
