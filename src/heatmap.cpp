#include "heatmap.h"

#include <math.h>
#include <string.h>

namespace {

constexpr double kMinShare = 0.05;

// How far from a square the worst tile of a row is (1 = all squares), for a row of tiles
// with these areas laid along a side of length `side`.
double worst(const double* area, int n, double side) {
  double sum = 0, mx = 0, mn = 1e300;
  for (int i = 0; i < n; i++) {
    sum += area[i];
    if (area[i] > mx) mx = area[i];
    if (area[i] < mn) mn = area[i];
  }
  double s2 = side * side, sum2 = sum * sum;
  double a = s2 * mx / sum2, b = sum2 / (s2 * mn);
  return a > b ? a : b;
}

}  // namespace

int heatmapLayout(const Portfolio& p, int w, int h, Tile out[kMaxTiles]) {
  // The values: the biggest holdings, then the rest added up into one "more" tile.
  double value[kMaxTiles] = {0};
  double weightedChange[kMaxTiles] = {0};
  int n = 0, more = 0;
  double total = 0;
  memset(out, 0, sizeof(Tile) * kMaxTiles);
  for (int i = 0; i < p.count; i++) {
    const Holding& hd = p.items[i];
    if (hd.priced && hd.amount * hd.usdPrice > 0) total += hd.amount * hd.usdPrice;
  }
  for (int i = 0; i < p.count; i++) {
    const Holding& hd = p.items[i];
    double v = hd.priced ? hd.amount * hd.usdPrice : 0;
    if (v <= 0) continue;
    int slot;
    // Its own tile if it's among the biggest AND at least 5%: smaller ones would be slivers
    // too thin to read or tap, so they join the "more" tile.
    if (n < kMaxTiles - 1 && more == 0 && v >= kMinShare * total) {
      slot = n++;
      out[slot].item = (int8_t)i;
      out[slot].hasChange = hd.hasChange;
    } else {
      if (more == 0) {
        slot = n++;
        out[slot].item = -1;
      }
      slot = kMaxTiles - 1;
      more++;
      out[slot].hasChange = out[slot].hasChange || hd.hasChange;
    }
    value[slot] += v;
    weightedChange[slot] += hd.hasChange ? v * hd.change24h : 0;
  }
  if (n == 0 || total <= 0) return 0;
  if (more == 1) {  // a "+1 more" tile is silly: show that holding itself (always the last)
    for (int i = p.count - 1; i >= 0; i--) {
      if (p.items[i].priced && p.items[i].amount * p.items[i].usdPrice > 0) {
        out[n - 1].item = (int8_t)i;
        break;
      }
    }
    more = 0;
  }
  for (int i = 0; i < n; i++) {
    out[i].share = (float)(value[i] / total);
    out[i].change24h = (float)(value[i] > 0 ? weightedChange[i] / value[i] : 0);
  }
  out[n - 1].moreCount = (uint8_t)more;

  // The tiles are already biggest first (the "more" tile last, it can be bigger than the
  // ones before it, which only makes the layout a little less square). Areas in pixels:
  double area[kMaxTiles];
  for (int i = 0; i < n; i++) area[i] = value[i] / total * w * h;

  double x = 0, y = 0, bw = w, bh = h;  // the box still to fill
  int i = 0;
  while (i < n) {
    double side = bw < bh ? bw : bh;
    int len = 1;  // tiles in this row
    while (i + len < n && worst(area + i, len + 1, side) <= worst(area + i, len, side)) len++;
    double sum = 0;
    for (int k = 0; k < len; k++) sum += area[i + k];
    bool last = i + len == n;
    if (bw >= bh) {  // a column on the left, as wide as it needs to be
      double cw = last ? bw : sum / bh;
      double ty = y;
      for (int k = 0; k < len; k++) {
        double th = (k == len - 1) ? y + bh - ty : area[i + k] / cw;
        Tile& t = out[i + k];
        t.x = (int16_t)lround(x);
        t.y = (int16_t)lround(ty);
        t.w = (int16_t)(lround(x + cw) - t.x);
        t.h = (int16_t)(lround(ty + th) - t.y);
        ty += th;
      }
      x += cw;
      bw -= cw;
    } else {  // a row along the top
      double rh = last ? bh : sum / bw;
      double tx = x;
      for (int k = 0; k < len; k++) {
        double tw = (k == len - 1) ? x + bw - tx : area[i + k] / rh;
        Tile& t = out[i + k];
        t.x = (int16_t)lround(tx);
        t.y = (int16_t)lround(y);
        t.w = (int16_t)(lround(tx + tw) - t.x);
        t.h = (int16_t)(lround(y + rh) - t.y);
        tx += tw;
      }
      y += rh;
      bh -= rh;
    }
    i += len;
  }
  return n;
}
