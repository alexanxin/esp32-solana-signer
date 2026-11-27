#pragma once
// The Dashboard's Activity tab: your last few transactions, as "Received 0.5 SOL from 7xKX...".
// Plain C++ (no Arduino, no Wi-Fi) so it is tested on the computer: pio test -e native.
//
// Two steps, both standard Solana RPC (any RPC works, no Helius-only features):
//   1. getSignaturesForAddress: the newest transaction ids ("signatures") that touched your
//      address, with their time and whether they failed. Small answer, asked every refresh.
//   2. getTransaction for the ones we haven't seen before, all in ONE request (a JSON-RPC
//      "batch": an array of questions, answered by an array). From each answer we compare
//      your balances before and after: SOL went down = sent, a token went up = received, one
//      down and one up = swap.
// Like the token list, this is for display only: the Signer decodes every transaction itself.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxActivity = 6;  // what the screen shows; also what fits LVGL's memory

enum class TxKind : uint8_t {
  kPending,   // only the signature is known so far (details come with the next answer)
  kReceived,  // something came in
  kSent,      // something went out
  kSwap,      // one thing out, another in
  kApp,       // no balance changed except the fee (an app call, a vote, an NFT listing...)
  kFailed,    // the network rejected it (only the fee was paid)
};

struct TxItem {
  char sig[90];        // base58 signature (88 characters at most)
  uint32_t blockTime;  // unix time (0 = not known)
  TxKind kind;
  char symbol[12];     // what came in (received, swap) or went out (sent): "SOL", "USDC"...
  double amount;       // how much of it, always positive
  char outSymbol[12];  // swap only: what went out
  double outAmount;
  char other[45];      // the other wallet, when the transaction says ("" = not known)
  double feeSol;       // network fee, when you paid it
};

struct Activity {
  TxItem items[kMaxActivity];  // newest first
  uint8_t count;
};

// The getSignaturesForAddress request for `address` (newest kMaxActivity).
void activitySignaturesBody(const char* address, char* out, size_t cap);

// Builds the new list from getSignaturesForAddress's "result" (an array, newest first).
// Transactions already in `old` keep their details; new ones start as kPending (or kFailed:
// the list already says that). Returns how many still need details, or -1 for a wrong answer.
int activityFromSignatures(JsonVariantConst result, const Activity& old, Activity* out);

// The batch request that asks getTransaction for every kPending item ("" when none).
// Each question's "id" is the item's place in the list, so answers can come back in any order.
void activityDetailsBody(const Activity& a, char* out, size_t cap);

// Which parts of the batch answer to keep. A full getTransaction answer also has the program
// logs, inner instructions, compute units...: often 5-10 KB each. The filter drops that while
// the bytes arrive, so only balances, owners and simple transfers use RAM.
void activityDetailsFilter(JsonDocument& filter);

// Applies the batch answer (an array of {"id":N,"result":{...}}) for `owner` (your address).
void activityApplyDetails(JsonVariantConst answer, const char* owner, Activity* a);

// "now" / "5m ago" / "3h ago" / "2d ago" / "" (time unknown). now and t are unix times.
void formatAgo(uint32_t now, uint32_t t, char* out, size_t cap);
// "7xKX...9fQa": the first and last 4 characters of an address.
void shortAddress(const char* addr, char* out, size_t cap);
