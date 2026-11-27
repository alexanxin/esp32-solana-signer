// Runs on your computer, no board needed:  pio test -e native
#include <string.h>
#include <unity.h>

#include "heatmap.h"

static Holding H(const char* sym, double amount, double price, float change) {
  Holding h = {};
  strcpy(h.symbol, sym);
  h.amount = amount;
  h.usdPrice = price;
  h.priced = price > 0;
  h.change24h = change;
  h.hasChange = price > 0;
  return h;
}

// Every pixel of the box belongs to exactly one tile: no gaps, no overlaps.
static void assertCovers(const Tile* t, int n, int w, int h) {
  static unsigned char seen[300][300];
  memset(seen, 0, sizeof seen);
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE(t[i].w > 0 && t[i].h > 0);
    for (int y = t[i].y; y < t[i].y + t[i].h; y++)
      for (int x = t[i].x; x < t[i].x + t[i].w; x++) {
        TEST_ASSERT_TRUE(x >= 0 && x < w && y >= 0 && y < h);
        TEST_ASSERT_EQUAL(0, seen[y][x]);
        seen[y][x] = 1;
      }
  }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) TEST_ASSERT_EQUAL(1, seen[y][x]);
}

void test_design_example_sol_big_on_the_left() {
  Portfolio p = {};
  p.items[p.count++] = H("SOL", 6.12, 142.31, 1.8f);  // ~$871, 68%
  p.items[p.count++] = H("USDC", 250, 1.0, 0.0f);     // 19%
  p.items[p.count++] = H("JUP", 210, 0.66, -0.9f);    // 11%
  p.items[p.count++] = H("BONK", 1e6, 0.00001, 6.0f);
  p.items[p.count++] = H("PYTH", 40, 0.3, -2.0f);
  p.items[p.count++] = H("WIF", 10, 1.0, 2.0f);
  Tile t[kMaxTiles];
  int n = heatmapLayout(p, 224, 220, t);
  TEST_ASSERT_EQUAL(0, t[0].item);
  TEST_ASSERT_EQUAL(0, t[0].x);
  TEST_ASSERT_EQUAL(0, t[0].y);
  TEST_ASSERT_EQUAL(220, t[0].h);  // a full-height column
  TEST_ASSERT_TRUE(t[0].w > 140);
  TEST_ASSERT_EQUAL(4, n);  // SOL, USDC, JUP, "+3 more": BONK, PYTH and WIF are under 5%
  TEST_ASSERT_EQUAL(-1, t[3].item);
  TEST_ASSERT_EQUAL(3, t[3].moreCount);
  assertCovers(t, n, 224, 220);
}

void test_more_tile_change_is_value_weighted() {
  Portfolio p = {};
  p.items[p.count++] = H("A", 100, 1, 0);
  p.items[p.count++] = H("B", 50, 1, 0);
  p.items[p.count++] = H("C", 40, 1, 0);
  p.items[p.count++] = H("D", 30, 1, 0);
  p.items[p.count++] = H("E", 30, 1, 10.0f);
  p.items[p.count++] = H("F", 10, 1, -10.0f);
  Tile t[kMaxTiles];
  TEST_ASSERT_EQUAL(5, heatmapLayout(p, 200, 200, t));
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 5.0f, t[4].change24h);  // (30*10 - 10*10) / 40
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 40.0f / 260, t[4].share);
}

void test_one_extra_holding_gets_its_own_tile() {
  Portfolio p = {};
  for (int i = 0; i < 5; i++) {
    char s[4] = {(char)('A' + i), 0};
    p.items[p.count++] = H(s, 50 - i * 5, 1, 0);
  }
  Tile t[kMaxTiles];
  TEST_ASSERT_EQUAL(5, heatmapLayout(p, 224, 220, t));
  TEST_ASSERT_EQUAL(4, t[4].item);  // E itself, not "+1 more"
  TEST_ASSERT_EQUAL(0, t[4].moreCount);
  assertCovers(t, 5, 224, 220);
}

void test_unpriced_and_empty() {
  Portfolio p = {};
  Tile t[kMaxTiles];
  TEST_ASSERT_EQUAL(0, heatmapLayout(p, 224, 220, t));
  p.items[p.count++] = H("SOL", 0, 142, 1);  // nothing held
  p.items[p.count++] = H("Ab3x...", 5000, 0, 0);  // no price
  TEST_ASSERT_EQUAL(0, heatmapLayout(p, 224, 220, t));
  p.items[p.count++] = H("USDC", 10, 1, 0);
  TEST_ASSERT_EQUAL(1, heatmapLayout(p, 224, 220, t));
  TEST_ASSERT_EQUAL(2, t[0].item);
  assertCovers(t, 1, 224, 220);
}

void test_many_equal_holdings_stay_close_to_square() {
  Portfolio p = {};
  for (int i = 0; i < 4; i++) p.items[p.count++] = H("X", 10, 1, 0);
  Tile t[kMaxTiles];
  int n = heatmapLayout(p, 220, 220, t);
  TEST_ASSERT_EQUAL(4, n);
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_INT_WITHIN(2, 110, t[i].w);  // a 2 x 2 grid, not 4 slivers
    TEST_ASSERT_INT_WITHIN(2, 110, t[i].h);
  }
  assertCovers(t, n, 220, 220);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_design_example_sol_big_on_the_left);
  RUN_TEST(test_more_tile_change_is_value_weighted);
  RUN_TEST(test_one_extra_holding_gets_its_own_tile);
  RUN_TEST(test_unpriced_and_empty);
  RUN_TEST(test_many_equal_holdings_stay_close_to_square);
  return UNITY_END();
}
