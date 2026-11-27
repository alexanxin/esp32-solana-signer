#include "chart.h"

#include <stdio.h>
#include <string.h>

#include "portfolio.h"  // kWrappedSolMint

namespace {

const char* priceMint(const char* mint) {
  return mint == nullptr || mint[0] == '\0' ? kWrappedSolMint : mint;
}

}  // namespace

void chartTokenPath(const char* mint, char* out, size_t cap) {
  snprintf(out, cap, "/api/v2/networks/solana/tokens/%s", priceMint(mint));
}

void chartTokenFilter(JsonDocument& filter) {
  filter.clear();
  // "data.relationships.top_pools.data[].id": the [] part is written as one element, which
  // ArduinoJson's filter applies to every element of the array.
  filter["data"]["relationships"]["top_pools"]["data"][0]["id"] = true;
}

bool chartPoolFromToken(JsonVariantConst answer, char* pool, size_t cap) {
  // The first pool is the busiest. Its id is "solana_<address>": network, "_", address.
  const char* id = answer["data"]["relationships"]["top_pools"]["data"][0]["id"] | "";
  const char* underscore = strchr(id, '_');
  const char* address = underscore != nullptr ? underscore + 1 : id;
  if (strlen(address) < 32) return false;  // a Solana address is 32-44 characters
  snprintf(pool, cap, "%s", address);
  return true;
}

void chartOhlcvPath(const char* pool, const char* mint, ChartRange range, char* out, size_t cap) {
  // 1D: 24 hourly prices. 1W: 42 prices of 4 hours. 1M: 30 daily prices.
  const char* step = range == ChartRange::k1M ? "day" : "hour";
  int aggregate = range == ChartRange::k1W ? 4 : 1;
  int limit = range == ChartRange::k1D ? 24 : range == ChartRange::k1W ? 42 : 30;
  snprintf(out, cap,
           "/api/v2/networks/solana/pools/%s/ohlcv/%s?aggregate=%d&limit=%d&currency=usd&token=%s",
           pool, step, aggregate, limit, priceMint(mint));
}

void chartOhlcvFilter(JsonDocument& filter) {
  filter.clear();
  filter["data"]["attributes"]["ohlcv_list"] = true;
}

bool chartFromOhlcv(JsonVariantConst answer, Chart* c) {
  JsonArrayConst list = answer["data"]["attributes"]["ohlcv_list"];
  int n = 0;
  float newestFirst[kChartPoints];
  for (JsonArrayConst row : list) {  // [time, open, high, low, close, volume]
    if (n >= kChartPoints) break;
    float close = row[4] | 0.0f;
    if (close > 0) newestFirst[n++] = close;
  }
  c->count = 0;
  c->ok = false;
  if (n < 2) return false;
  for (int i = 0; i < n; i++) c->close[i] = newestFirst[n - 1 - i];
  c->count = (uint8_t)n;
  c->ok = true;
  return true;
}
