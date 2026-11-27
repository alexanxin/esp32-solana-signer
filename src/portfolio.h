#pragma once
// What the Dashboard shows: your tokens, what they are worth, and how that changed in 24 h.
// Plain C++ (no Arduino, no Wi-Fi) so it is tested on the computer: pio test -e native.
//
// Where the numbers come from (all free, no account or key):
//   - balances: the public Solana RPC (getBalance + getTokenAccountsByOwner)
//   - prices and 24 h change: Jupiter's free Price API (lite-api.jup.ag/price/v3), the same
//     source the Tally app uses. One request prices SOL and every token by mint.
// The board never trusts these for anything but display: the Signer decodes every
// transaction itself and never looks at this.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxHoldings = 24;  // kept while pricing; the screens show the first few
constexpr int kWatchCount = 6;    // the Market grid: BTC ETH SOL JUP BONK WIF (held or not)

struct Holding {
  char symbol[12];  // "SOL", "USDC", or a short mint "Ab3x..." for tokens we don't know
  char mint[45];    // base58 mint address ("" for SOL itself)
  double amount;    // in whole tokens (6.12 SOL, not lamports)
  double usdPrice;  // per token, 0 if unknown
  float change24h;  // price change in the last 24 h, in percent (1.8 = +1.8%)
  bool priced;      // usdPrice is real
  bool hasChange;   // change24h is real
};

struct Portfolio {
  Holding items[kMaxHoldings];
  uint8_t count;    // used entries in items, most valuable first after portfolioFinish()
  uint16_t hidden;  // tokens left out: no price on mainnet (almost always spam), or no room
  double totalUsd;  // sum of the priced holdings
  double totalUsd24hAgo;  // the same holdings at yesterday's prices (for "today" change)
  bool anyPriced;   // false = no dollar value at all
  // Market prices to watch whether you hold them or not (amount stays 0). Filled in by
  // portfolioStart() and priced by the same request as the holdings.
  Holding watch[kWatchCount];
};

// ---- Building it, one answer at a time ---------------------------------------------------

// Fills in the watchlist's names (no prices yet). portfolioStart() does this too; the screen
// calls it to draw the watchlist before the first answer arrives.
void portfolioInitWatch(Portfolio* p);

// Starts an empty portfolio with SOL from getBalance's "result" ({"value": lamports}).
// Returns false if the answer has no balance.
bool portfolioStart(JsonVariantConst getBalanceResult, Portfolio* out);

// Tells ArduinoJson which parts of a whole getTokenAccountsByOwner (jsonParsed) answer to
// keep. The full answer repeats owner, program, rent... per account; the filter drops that
// while the bytes arrive, so only mint and amount use RAM.
void tokenAccountsFilter(JsonDocument& filter);

// Adds the token accounts from getTokenAccountsByOwner's "result" (empty ones are skipped,
// two accounts of the same mint are added up). Returns false if it isn't such an answer.
bool portfolioAddTokens(JsonVariantConst result, Portfolio* p);

// SOL has no mint of its own; price lists use wrapped SOL's mint for it.
extern const char* const kWrappedSolMint;

// The Jupiter Price API path that prices everything in p:
// "/price/v3?ids=So111...112,<watchlist mints>,<mint>,<mint>..." (SOL first, then the
// watchlist, then each token, up to what fits in cap; the rest stay unpriced this time).
void pricePath(const Portfolio& p, char* out, size_t cap);

// Applies a Jupiter Price API answer
// ({"<mint>":{"usdPrice":142.3,"priceChange24h":1.8,...},...}) to the matching holdings.
// Mints Jupiter doesn't know are simply missing from the answer.
void portfolioApplyPrices(JsonVariantConst answer, Portfolio* p);

// When the price request failed: gives every holding and watchlist coin that has no price
// the one it had in `old` (the last refresh), like wallet apps keep showing the last known
// price instead of none. Call before portfolioFinish().
void portfolioCarryPrices(const Portfolio& old, Portfolio* p);

// Last step: hides unpriced tokens (hideUnpriced: mainnet), sorts, and adds up the totals.
void portfolioFinish(Portfolio* p, bool hideUnpriced);

// ---- Finding one token (the detail screen) ----------------------------------------------

// Your holding of `mint`, or nullptr if you hold none. SOL can be asked for as "" or as
// wrapped SOL's mint (the watchlist uses that one), and is found either way.
const Holding* portfolioFindHeld(const Portfolio& p, const char* mint);
// The Market watchlist's entry for `mint` (same SOL rule), or nullptr.
const Holding* portfolioFindWatch(const Portfolio& p, const char* mint);

// ---- Text for the screen -----------------------------------------------------------------

// Symbol for a well-known mainnet mint ("USDC"), or nullptr.
const char* knownTokenSymbol(const char* mint);
// $1,284.50 / $12,840 (no cents from $10,000) / $0.42 / $0.000019
void formatUsd(double usd, char* out, size_t cap);
// 6.12 / 210 / 1.2M / 0.0042 (at most ~6 characters)
void formatAmount(double amount, char* out, size_t cap);
// "▲ $32.10 (2.6%)" or "▼ $3.00 (0.4%)"; *up says which colour to use. now/ref in USD.
void formatChange(double now, double ref, char* out, size_t cap, bool* up);
// "▲ 1.8%" / "▼ 0.9%" / "0.0%"; *up as above (true for 0).
void formatPercent(float pct, char* out, size_t cap, bool* up);
