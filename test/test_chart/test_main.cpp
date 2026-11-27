// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "chart.h"

// Trimmed GeckoTerminal answers (the real token answer also has name, price, supply...).
static const char* kToken = R"({"data":{"id":"solana_So11111111111111111111111111111111111111112",
  "type":"token","attributes":{"symbol":"SOL","price_usd":"142.31"},
  "relationships":{"top_pools":{"data":[
    {"id":"solana_Czfq3xZZDmsdGdUyrNLtRhGc47cXcZtLG4crryfu44zE","type":"pool"},
    {"id":"solana_58oQChx4yWmvKdwLLZzBi4ChoCc2fqCUWBkwMihLYQo2","type":"pool"}]}}}})";
static const char* kNoPools = R"({"data":{"id":"solana_X","relationships":{"top_pools":{"data":[]}}}})";
// Newest first, like GeckoTerminal sends it: [time, open, high, low, close, volume].
static const char* kOhlcv = R"({"data":{"id":"x","type":"ohlcv_request_response","attributes":{
  "ohlcv_list":[[1764280800,141.9,142.6,141.5,142.31,90000],
                [1764277200,140.2,142.0,140.1,141.9,85000],
                [1764273600,139.5,140.4,139.2,140.2,70000]]}},
  "meta":{"base":{"symbol":"SOL"},"quote":{"symbol":"USDC"}}})";

void setUp() {}
void tearDown() {}

void test_token_path_uses_wrapped_sol_for_sol() {
  char path[128];
  chartTokenPath("", path, sizeof path);
  TEST_ASSERT_EQUAL_STRING(
      "/api/v2/networks/solana/tokens/So11111111111111111111111111111111111111112", path);
}

void test_pool_from_token_answer() {
  JsonDocument filter, doc;
  chartTokenFilter(filter);
  TEST_ASSERT_FALSE(deserializeJson(doc, kToken, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(doc["data"]["attributes"].isNull());  // the filter dropped it
  char pool[48];
  TEST_ASSERT_TRUE(chartPoolFromToken(doc.as<JsonVariantConst>(), pool, sizeof pool));
  TEST_ASSERT_EQUAL_STRING("Czfq3xZZDmsdGdUyrNLtRhGc47cXcZtLG4crryfu44zE", pool);

  TEST_ASSERT_FALSE(deserializeJson(doc, kNoPools));
  TEST_ASSERT_FALSE(chartPoolFromToken(doc.as<JsonVariantConst>(), pool, sizeof pool));
}

void test_ohlcv_paths_per_range() {
  char path[200];
  chartOhlcvPath("Pool", "", ChartRange::k1D, path, sizeof path);
  TEST_ASSERT_EQUAL_STRING(
      "/api/v2/networks/solana/pools/Pool/ohlcv/hour?aggregate=1&limit=24&currency=usd"
      "&token=So11111111111111111111111111111111111111112", path);
  chartOhlcvPath("Pool", "JUPmint", ChartRange::k1W, path, sizeof path);
  TEST_ASSERT_NOT_NULL(strstr(path, "/ohlcv/hour?aggregate=4&limit=42"));
  TEST_ASSERT_NOT_NULL(strstr(path, "&token=JUPmint"));
  chartOhlcvPath("Pool", "JUPmint", ChartRange::k1M, path, sizeof path);
  TEST_ASSERT_NOT_NULL(strstr(path, "/ohlcv/day?aggregate=1&limit=30"));
}

void test_prices_come_out_oldest_first() {
  JsonDocument filter, doc;
  chartOhlcvFilter(filter);
  TEST_ASSERT_FALSE(deserializeJson(doc, kOhlcv, DeserializationOption::Filter(filter)));
  static Chart c;
  TEST_ASSERT_TRUE(chartFromOhlcv(doc.as<JsonVariantConst>(), &c));
  TEST_ASSERT_TRUE(c.ok);
  TEST_ASSERT_EQUAL_UINT8(3, c.count);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 140.2f, c.close[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 142.31f, c.close[2]);
}

void test_too_few_prices_is_no_chart() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, R"({"data":{"attributes":{"ohlcv_list":[[1,1,1,1,2,0]]}}})"));
  static Chart c;
  TEST_ASSERT_FALSE(chartFromOhlcv(doc.as<JsonVariantConst>(), &c));
  TEST_ASSERT_FALSE(c.ok);
  TEST_ASSERT_FALSE(deserializeJson(doc, R"({"errors":[{"status":"404","title":"Not Found"}]})"));
  TEST_ASSERT_FALSE(chartFromOhlcv(doc.as<JsonVariantConst>(), &c));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_token_path_uses_wrapped_sol_for_sol);
  RUN_TEST(test_pool_from_token_answer);
  RUN_TEST(test_ohlcv_paths_per_range);
  RUN_TEST(test_prices_come_out_oldest_first);
  RUN_TEST(test_too_few_prices_is_no_chart);
  return UNITY_END();
}
