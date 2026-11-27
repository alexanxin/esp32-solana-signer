#pragma once
// The portfolio heatmap: your holdings as tiles, tile size = share of your money, colour =
// 24 h move (drawn by dash_ui.cpp). Plain C++ so the layout is tested on the computer.
//
// The layout is a "squarified treemap" (Bruls, Huizing, van Wijk, 2000): fill the box one
// row at a time along its short side, adding the next tile to the row only while that
// makes the row's tiles closer to squares. Long thin slivers are hard to read and to tap.
#include <stdint.h>

#include "portfolio.h"

constexpr int kMaxTiles = 5;  // the 4 biggest holdings + one "+N more" tile

struct Tile {
  int16_t x, y, w, h;  // pixels, inside the box given to heatmapLayout
  int8_t item;         // index into Portfolio::items, or -1 for the "+N more" tile
  uint8_t moreCount;   // how many holdings the "+N more" tile stands for
  float share;         // 0..1 of the portfolio's value
  float change24h;     // value-weighted for the "more" tile
  bool hasChange;
};

// Lays out the priced holdings of p (portfolioFinish() sorted them, most valuable first)
// in a w x h box. Returns how many tiles it wrote into out (0 = nothing priced yet).
int heatmapLayout(const Portfolio& p, int w, int h, Tile out[kMaxTiles]);
