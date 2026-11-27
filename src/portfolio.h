#pragma once
// What the Dashboard shows: your tokens, what they are worth, and how that changed in 24 h.
// Plain C++ (no Arduino, no Wi-Fi) so it is tested on the computer: pio test -e native.
//
// Where the numbers come from (all free, no account or key):
//   - balances: the public Solana RPC (getBalance + getTokenAccountsByOwner)
//   - prices and 24 h change: CoinGecko's free price API
// The board never trusts these for anything but display: the Signer decodes every
// transaction itself and never looks at this.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxHoldings = 24;  // kept while pricing; the screens show the first few

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
};

// ---- Building it, one answer at a time ---------------------------------------------------

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

// The CoinGecko URL path that prices everything in p: SOL by id, tokens by mint.
// tokensOnly=false: "/api/v3/simple/price?ids=solana&vs_currencies=usd&include_24hr_change=true"
// tokensOnly=true: "/api/v3/simple/token_price/solana?contract_addresses=<mints>&..."
// Returns false if there is nothing to ask for (no tokens).
bool coinGeckoPath(const Portfolio& p, bool tokensOnly, char* out, size_t cap);

// Applies a CoinGecko answer ({"solana":{"usd":..,"usd_24h_change":..}} or the same keyed
// by mint) to the matching holdings.
void portfolioApplyPrices(JsonVariantConst answer, Portfolio* p);

// Last step: hides unpriced tokens (hideUnpriced: mainnet), sorts, and adds up the totals.
void portfolioFinish(Portfolio* p, bool hideUnpriced);

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
