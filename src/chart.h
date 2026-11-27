#pragma once
// The price chart on a token's detail screen (1 day / 1 week / 1 month).
// Plain C++ (no Arduino, no Wi-Fi) so it is tested on the computer: pio test -e native.
//
// Jupiter's free price API has today's price only, no history. The history comes from
// GeckoTerminal (api.geckoterminal.com, free, no key, 30 requests a minute), in two steps:
//   1. /networks/solana/tokens/<mint>: the token's busiest trading pools ("top_pools").
//      A price history belongs to a pool (a market where the token trades), not to a token.
//   2. /networks/solana/pools/<pool>/ohlcv/hour?...: that pool's price per hour (or day).
//      "OHLCV" = Open, High, Low, Close, Volume: five numbers per hour. We keep Close, the
//      price at the end of each hour, which is what a simple line chart shows.
// Asked only when you open a token, never in the background refresh. Display only.
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

constexpr int kChartPoints = 42;  // 1W = 42 points of 4 hours; 1D = 24, 1M = 30

enum class ChartRange : uint8_t { k1D, k1W, k1M };

struct Chart {
  char mint[45];  // which token ("" = SOL)
  ChartRange range;
  bool ok;        // false = no chart (GeckoTerminal doesn't know the token, or no answer)
  uint8_t count;  // points in close[]
  float close[kChartPoints];  // prices in USD, oldest first
};

// Step 1: the path that asks for the token's pools ("" or wrapped SOL's mint = SOL).
void chartTokenPath(const char* mint, char* out, size_t cap);
void chartTokenFilter(JsonDocument& filter);  // keeps only the first pool's id
// The busiest pool's address from that answer. False if the token has no pool.
bool chartPoolFromToken(JsonVariantConst answer, char* pool, size_t cap);

// Step 2: the path that asks for the pool's prices over `range`, priced as `mint` (a pool
// trades two tokens; this says which one's price we want, in USD).
void chartOhlcvPath(const char* pool, const char* mint, ChartRange range, char* out, size_t cap);
void chartOhlcvFilter(JsonDocument& filter);  // keeps only the list of prices
// Fills c->close (oldest first; GeckoTerminal sends newest first) and c->count.
// False if the answer has fewer than 2 prices.
bool chartFromOhlcv(JsonVariantConst answer, Chart* c);
