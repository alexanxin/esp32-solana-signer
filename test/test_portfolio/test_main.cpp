// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "portfolio.h"

// Trimmed public-RPC answers (the real ones repeat owner, program, rent... per account).
static const char* kBalance = R"({"jsonrpc":"2.0","result":{"context":{"slot":1},"value":6120000000},"id":1})";
static const char* kTokens = R"({"jsonrpc":"2.0","id":2,"result":{"context":{"slot":1},"value":[
  {"pubkey":"Acc1","account":{"lamports":2039280,"owner":"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
   "data":{"program":"spl-token","parsed":{"type":"account","info":{
     "isNative":false,"mint":"JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN","owner":"me","state":"initialized",
     "tokenAmount":{"amount":"210000000","decimals":6,"uiAmount":210.0,"uiAmountString":"210"}}},"space":165}}},
  {"pubkey":"Acc2","account":{"data":{"parsed":{"info":{
     "mint":"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
     "tokenAmount":{"amount":"250000000","decimals":6}}}}}},
  {"pubkey":"Acc3","account":{"data":{"parsed":{"info":{
     "mint":"Spam1111111111111111111111111111111111111111",
     "tokenAmount":{"amount":"1000000000000","decimals":6}}}}}},
  {"pubkey":"Acc4","account":{"data":{"parsed":{"info":{
     "mint":"Empty111111111111111111111111111111111111111",
     "tokenAmount":{"amount":"0","decimals":6}}}}}}
]}})";
// Jupiter Price API v3 answers, trimmed (the real ones add liquidity, blockId, decimals...).
// SOL is priced by wrapped SOL's mint.
static const char* kSolPrice = R"({
  "So11111111111111111111111111111111111111112":{"usdPrice":142.31,"priceChange24h":1.8}})";
static const char* kTokenPrices = R"({
  "So11111111111111111111111111111111111111112":{"usdPrice":142.31,"priceChange24h":1.8},
  "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN":{"usdPrice":0.66,"priceChange24h":-0.9},
  "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v":{"usdPrice":1.0,"priceChange24h":0.0}})";

static void build(bool mainnet, Portfolio* p) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, kBalance));
  TEST_ASSERT_TRUE(portfolioStart(doc["result"], p));

  JsonDocument filter;
  tokenAccountsFilter(filter);
  TEST_ASSERT_FALSE(deserializeJson(doc, kTokens, DeserializationOption::Filter(filter)));
  TEST_ASSERT_TRUE(portfolioAddTokens(doc["result"], p));

  // On devnet Jupiter knows none of the token mints, so only SOL comes back.
  TEST_ASSERT_FALSE(deserializeJson(doc, mainnet ? kTokenPrices : kSolPrice));
  portfolioApplyPrices(doc.as<JsonVariantConst>(), p);
  portfolioFinish(p, mainnet);
}

void test_mainnet_hides_unpriced_and_sorts_by_value() {
  Portfolio p;
  build(true, &p);
  TEST_ASSERT_EQUAL(3, p.count);  // SOL, USDC, JUP; the spam token is hidden, the empty skipped
  TEST_ASSERT_EQUAL(1, p.hidden);
  TEST_ASSERT_EQUAL_STRING("SOL", p.items[0].symbol);
  TEST_ASSERT_EQUAL_STRING("USDC", p.items[1].symbol);
  TEST_ASSERT_EQUAL_STRING("JUP", p.items[2].symbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 6.12, p.items[0].amount);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 210.0, p.items[2].amount);
  TEST_ASSERT_TRUE(p.anyPriced);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 6.12 * 142.31 + 250 + 138.6, p.totalUsd);
  // yesterday: SOL was 1.8% cheaper, JUP 0.9% dearer, USDC the same
  double then = 6.12 * 142.31 / 1.018 + 250 + 138.6 / 0.991;
  TEST_ASSERT_DOUBLE_WITHIN(0.01, then, p.totalUsd24hAgo);
  TEST_ASSERT_TRUE(p.items[2].hasChange);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, -0.9f, p.items[2].change24h);
}

void test_devnet_keeps_unpriced_tokens_after_sol() {
  Portfolio p;
  build(false, &p);
  TEST_ASSERT_EQUAL(4, p.count);
  TEST_ASSERT_EQUAL(0, p.hidden);
  TEST_ASSERT_EQUAL_STRING("SOL", p.items[0].symbol);  // the only priced one
  TEST_ASSERT_EQUAL_STRING("Spam...", p.items[1].symbol);  // biggest amount next
  TEST_ASSERT_FALSE(p.items[1].priced);
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 6.12 * 142.31, p.totalUsd);
}

void test_same_mint_in_two_accounts_adds_up() {
  Portfolio p;
  JsonDocument doc;
  deserializeJson(doc, R"({"value":0})");
  portfolioStart(doc.as<JsonVariantConst>(), &p);
  deserializeJson(doc, R"({"value":[
    {"account":{"data":{"parsed":{"info":{"mint":"M1","tokenAmount":{"amount":"5","decimals":0}}}}}},
    {"account":{"data":{"parsed":{"info":{"mint":"M1","tokenAmount":{"amount":"7","decimals":0}}}}}}]})");
  portfolioAddTokens(doc.as<JsonVariantConst>(), &p);
  TEST_ASSERT_EQUAL(2, p.count);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12, p.items[1].amount);
}

void test_rejects_an_error_answer() {
  JsonDocument doc;
  deserializeJson(doc, R"({"error":{"code":-32602,"message":"bad request"}})");
  Portfolio p;
  TEST_ASSERT_FALSE(portfolioStart(doc["result"], &p));
  TEST_ASSERT_FALSE(portfolioAddTokens(doc["result"], &p));
}

void test_price_path() {
  Portfolio p;
  build(false, &p);
  char path[512];
  pricePath(p, path, sizeof path);
  TEST_ASSERT_EQUAL_STRING(
      "/price/v3?ids=So11111111111111111111111111111111111111112,"
      "cbbtcf3aa214zXHbiAZQwf4122FBYbraNdFqgw4iMij,JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN,"
      "Spam1111111111111111111111111111111111111111,EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
      path);  // watchlist first; JUP (held and watched) only once
  pricePath(p, path, 60);  // too small for a second mint: SOL only
  TEST_ASSERT_EQUAL_STRING("/price/v3?ids=So11111111111111111111111111111111111111112", path);
}

void test_watchlist_is_priced_without_holding_it() {
  Portfolio p;
  build(true, &p);  // holds SOL, USDC, JUP; no BTC
  TEST_ASSERT_EQUAL_STRING("BTC", p.watch[0].symbol);
  TEST_ASSERT_FALSE(p.watch[0].priced);  // not in kTokenPrices
  TEST_ASSERT_EQUAL_STRING("SOL", p.watch[1].symbol);
  TEST_ASSERT_DOUBLE_WITHIN(1e-4, 142.31, p.watch[1].usdPrice);
  TEST_ASSERT_EQUAL_STRING("JUP", p.watch[2].symbol);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, -0.9f, p.watch[2].change24h);
  // Watching doesn't count as owning: the total is still only the holdings.
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 6.12 * 142.31 + 250 + 138.6, p.totalUsd);
}

void test_devnet_usdc_is_one_dollar() {
  Portfolio p;
  JsonDocument doc;
  deserializeJson(doc, R"({"value":0})");
  portfolioStart(doc.as<JsonVariantConst>(), &p);
  deserializeJson(doc, R"({"value":[{"account":{"data":{"parsed":{"info":{
    "mint":"4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU",
    "tokenAmount":{"amount":"25000000","decimals":6}}}}}}]})");
  portfolioAddTokens(doc.as<JsonVariantConst>(), &p);
  portfolioFinish(&p, false);  // no price answer at all: USDC still counts
  TEST_ASSERT_EQUAL_STRING("USDC", p.items[0].symbol);
  TEST_ASSERT_TRUE(p.anyPriced);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 25.0, p.totalUsd);
  char path[256];
  pricePath(p, path, sizeof path);  // and isn't asked about
  TEST_ASSERT_NULL(strstr(path, "4zMMC"));
}

void test_format_usd() {
  char s[24];
  formatUsd(1284.5, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$1,284.50", s);
  formatUsd(12840.4, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$12,840", s);
  formatUsd(1234567.0, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$1,234,567", s);
  formatUsd(0.42, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$0.42", s);
  formatUsd(0.0000192271, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$0.000019", s);
  formatUsd(0, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("$0.00", s);
}

void test_format_amount() {
  char s[24];
  formatAmount(6.12, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("6.12", s);
  formatAmount(6.1, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("6.1", s);
  formatAmount(210, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("210", s);
  formatAmount(1200000, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("1.2M", s);
  formatAmount(1000000, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("1M", s);
  formatAmount(0.0042, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("0.0042", s);
  formatAmount(250, s, sizeof s);
  TEST_ASSERT_EQUAL_STRING("250", s);
}

void test_format_change() {
  char s[40];
  bool up = false;
  formatChange(1284.50, 1252.40, s, sizeof s, &up);
  TEST_ASSERT_TRUE(up);
  TEST_ASSERT_EQUAL_STRING("\xE2\x96\xB2 $32.10 (2.6%)", s);
  formatChange(100, 110, s, sizeof s, &up);
  TEST_ASSERT_FALSE(up);
  TEST_ASSERT_EQUAL_STRING("\xE2\x96\xBC $10.00 (9.1%)", s);
}

void test_format_percent() {
  char s[16];
  bool up = false;
  formatPercent(1.84f, s, sizeof s, &up);
  TEST_ASSERT_TRUE(up);
  TEST_ASSERT_EQUAL_STRING("\xE2\x96\xB2 1.8%", s);
  formatPercent(-0.9f, s, sizeof s, &up);
  TEST_ASSERT_FALSE(up);
  TEST_ASSERT_EQUAL_STRING("\xE2\x96\xBC 0.9%", s);
  formatPercent(0.01f, s, sizeof s, &up);
  TEST_ASSERT_EQUAL_STRING("0.0%", s);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_mainnet_hides_unpriced_and_sorts_by_value);
  RUN_TEST(test_devnet_keeps_unpriced_tokens_after_sol);
  RUN_TEST(test_same_mint_in_two_accounts_adds_up);
  RUN_TEST(test_rejects_an_error_answer);
  RUN_TEST(test_price_path);
  RUN_TEST(test_watchlist_is_priced_without_holding_it);
  RUN_TEST(test_devnet_usdc_is_one_dollar);
  RUN_TEST(test_format_usd);
  RUN_TEST(test_format_amount);
  RUN_TEST(test_format_change);
  RUN_TEST(test_format_percent);
  return UNITY_END();
}
