#include "portfolio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void copyText(char* dst, size_t cap, const char* src) {
  snprintf(dst, cap, "%s", src != nullptr ? src : "");
}

// Most valuable first; tokens without a price go last, biggest amount first.
bool before(const Holding& a, const Holding& b) {
  if (a.priced != b.priced) return a.priced;
  if (a.priced) return a.amount * a.usdPrice > b.amount * b.usdPrice;
  return a.amount > b.amount;
}

void sortHoldings(Holding* h, int n) {  // insertion sort: n is at most kMaxHoldings
  for (int i = 1; i < n; i++) {
    Holding x = h[i];
    int j = i - 1;
    while (j >= 0 && before(x, h[j])) {
      h[j + 1] = h[j];
      j--;
    }
    h[j + 1] = x;
  }
}

Holding* findMint(Portfolio* p, const char* mint) {
  for (int i = 0; i < p->count; i++) {
    if (strcmp(p->items[i].mint, mint) == 0) return &p->items[i];
  }
  return nullptr;
}

// Adds a thousands comma: 1284 -> "1,284".
void withCommas(long long whole, char* out, size_t cap) {
  char digits[24];
  snprintf(digits, sizeof digits, "%lld", whole);
  size_t n = strlen(digits), o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) {
    if (i > 0 && (n - i) % 3 == 0 && o + 2 < cap) out[o++] = ',';
    out[o++] = digits[i];
  }
  out[o] = '\0';
}

// How many decimals show the first two digits that matter: 0.0042 -> 4, 0.000019 -> 6.
int twoDigits(double a) {
  int d = (int)-floor(log10(a)) + 1;
  return d < 2 ? 2 : (d > 12 ? 12 : d);
}

struct KnownToken {
  const char* mint;
  const char* symbol;
};
// Well-known mainnet tokens, so the list says "USDC" instead of "EPjF...". Anything else
// shows its short mint: we'd rather show an honest address than a name a scammer picked.
const KnownToken kKnown[] = {
    {"So11111111111111111111111111111111111111112", "wSOL"},
    {"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v", "USDC"},
    {"Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB", "USDT"},
    {"JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN", "JUP"},
    {"DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263", "BONK"},
    {"HZ1JovNiVvGrGNiiYvEozEVgZ58xaU3RKwX8eACQBCt3", "PYTH"},
    {"J1toso1uCk3RLmjorhTtrVwY9HJ7X8V9yYac6Y7kGCPn", "JitoSOL"},
    {"mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So", "mSOL"},
    {"bSo13r4TkiE4KumL71LsHTPpL2euBYLFx6h9HP3piy1", "bSOL"},
    {"EKpQGSJtjMFqKZ9KQanSqYXRcF8fBopzLHYxdM65zcjm", "WIF"},
    {"4k3Dyjzvzp8eMZWUXbBCjEvwSkkk59S5iCNLY3QrkX6R", "RAY"},
    {"jtojtomepa8beP8AuQc6eXt5FriJwfFMwQx2v2f9mCL", "JTO"},
    {"7vfCXTUXx5WJV5JADk17DUJ4ksgau7utNKj4b963voxs", "ETH"},
    {"cbbtcf3aa214zXHbiAZQwf4122FBYbraNdFqgw4iMij", "BTC"},
    {"4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU", "USDC"},  // Circle's devnet USDC
};

// Circle's devnet USDC: no market knows it (devnet mints aren't the real ones), so it is
// valued at $1, like the Tally app does. Everything else on devnet just shows its amount.
const char* kDevnetUsdc = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU";

// The watchlist. Solana has no "real" BTC: the most traded stand-in is Coinbase's cbBTC
// (1 cbBTC = 1 BTC, held by Coinbase), so its price is Bitcoin's price.
struct Watch {
  const char* symbol;
  const char* mint;
};
const Watch kWatch[kWatchCount] = {
    {"BTC", "cbbtcf3aa214zXHbiAZQwf4122FBYbraNdFqgw4iMij"},
    {"ETH", "7vfCXTUXx5WJV5JADk17DUJ4ksgau7utNKj4b963voxs"},  // Wormhole ETH: 1 = 1 ETH
    {"SOL", "So11111111111111111111111111111111111111112"},
    {"JUP", "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN"},
    {"BONK", "DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263"},
    {"WIF", "EKpQGSJtjMFqKZ9KQanSqYXRcF8fBopzLHYxdM65zcjm"},
};

}  // namespace

const char* knownTokenSymbol(const char* mint) {
  for (const KnownToken& k : kKnown) {
    if (strcmp(k.mint, mint) == 0) return k.symbol;
  }
  return nullptr;
}

void portfolioInitWatch(Portfolio* p) {
  for (int i = 0; i < kWatchCount; i++) {
    memset(&p->watch[i], 0, sizeof p->watch[i]);
    copyText(p->watch[i].symbol, sizeof p->watch[i].symbol, kWatch[i].symbol);
    copyText(p->watch[i].mint, sizeof p->watch[i].mint, kWatch[i].mint);
  }
}

bool portfolioStart(JsonVariantConst getBalanceResult, Portfolio* out) {
  memset(out, 0, sizeof *out);
  JsonVariantConst lamports = getBalanceResult["value"];
  if (!lamports.is<double>()) return false;
  Holding& sol = out->items[0];
  copyText(sol.symbol, sizeof sol.symbol, "SOL");
  sol.amount = lamports.as<double>() / 1e9;
  out->count = 1;  // SOL is always listed, even at 0
  portfolioInitWatch(out);
  return true;
}

void tokenAccountsFilter(JsonDocument& filter) {
  filter.clear();
  filter["error"] = true;  // keep an RPC error so it can be logged
  JsonObject info = filter["result"]["value"].add<JsonObject>()["account"]["data"]["parsed"]
                        ["info"].to<JsonObject>();
  info["mint"] = true;
  info["tokenAmount"]["amount"] = true;
  info["tokenAmount"]["decimals"] = true;
}

bool portfolioAddTokens(JsonVariantConst result, Portfolio* p) {
  JsonArrayConst accounts = result["value"];
  if (accounts.isNull()) return false;
  for (JsonVariantConst acc : accounts) {
    JsonVariantConst info = acc["account"]["data"]["parsed"]["info"];
    const char* mint = info["mint"] | "";
    // "amount" is a string: token amounts can be bigger than a double counts exactly.
    double raw = atof(info["tokenAmount"]["amount"] | "0");
    int decimals = info["tokenAmount"]["decimals"] | 0;
    if (mint[0] == '\0' || raw <= 0) continue;
    double amount = raw / pow(10.0, decimals);
    Holding* same = findMint(p, mint);
    if (same != nullptr) {
      same->amount += amount;
      continue;
    }
    if (p->count >= kMaxHoldings) {
      p->hidden++;
      continue;
    }
    Holding& h = p->items[p->count++];
    memset(&h, 0, sizeof h);
    copyText(h.mint, sizeof h.mint, mint);
    const char* known = knownTokenSymbol(mint);
    if (known != nullptr) copyText(h.symbol, sizeof h.symbol, known);
    else snprintf(h.symbol, sizeof h.symbol, "%.4s...", mint);
    h.amount = amount;
    if (strcmp(mint, kDevnetUsdc) == 0) {
      h.usdPrice = 1.0;
      h.priced = true;
      h.hasChange = true;  // 0.0%
    }
  }
  return true;
}

const char* const kWrappedSolMint = "So11111111111111111111111111111111111111112";

void pricePath(const Portfolio& p, char* out, size_t cap) {
  int n = snprintf(out, cap, "/price/v3?ids=%s", kWrappedSolMint);
  // Watchlist first, then holdings; each mint only once (strstr: is it in the URL already?).
  for (int i = 0; i < kWatchCount + p.count; i++) {
    const char* mint = i < kWatchCount ? p.watch[i].mint : p.items[i - kWatchCount].mint;
    if (mint[0] == '\0' || strcmp(mint, kDevnetUsdc) == 0 || strstr(out, mint) != nullptr) {
      continue;
    }
    if (n + strlen(mint) + 2 >= cap) break;  // URL full: the rest stay unpriced this time
    n += snprintf(out + n, cap - n, ",%s", mint);
  }
}

void portfolioApplyPrices(JsonVariantConst answer, Portfolio* p) {
  JsonObjectConst obj = answer.as<JsonObjectConst>();
  for (JsonPairConst kv : obj) {
    const char* key = kv.key().c_str();
    double usd = kv.value()["usdPrice"] | 0.0;
    JsonVariantConst change = kv.value()["priceChange24h"];
    if (usd <= 0) continue;
    for (int i = 0; i < p->count + kWatchCount; i++) {
      Holding& h = i < p->count ? p->items[i] : p->watch[i - p->count];
      const char* mint = h.mint[0] == '\0' ? kWrappedSolMint : h.mint;
      if (strcmp(key, mint) != 0) continue;
      h.usdPrice = usd;
      h.priced = true;
      h.hasChange = change.is<double>();
      h.change24h = h.hasChange ? change.as<float>() : 0;
    }
  }
}

namespace {
const Holding* findIn(const Holding* list, int n, const char* mint) {
  for (int i = 0; i < n; i++) {
    if (strcmp(list[i].mint, mint) == 0) return &list[i];
  }
  return nullptr;
}
}  // namespace

void portfolioCarryPrices(const Portfolio& old, Portfolio* p) {
  for (int i = 0; i < p->count + kWatchCount; i++) {
    Holding& h = i < p->count ? p->items[i] : p->watch[i - p->count];
    if (h.priced) continue;
    const Holding* was = findIn(old.items, old.count, h.mint);  // SOL: mint "" matches SOL
    if (was == nullptr || !was->priced) was = findIn(old.watch, kWatchCount, h.mint);
    if (was == nullptr || !was->priced) continue;
    h.usdPrice = was->usdPrice;
    h.priced = true;
    h.change24h = was->change24h;
    h.hasChange = was->hasChange;
  }
}

void portfolioFinish(Portfolio* p, bool hideUnpriced) {
  if (hideUnpriced) {
    int kept = 0;
    for (int i = 0; i < p->count; i++) {
      Holding& h = p->items[i];
      if (h.priced || h.mint[0] == '\0') {
        p->items[kept++] = h;  // SOL stays even before its price arrives
      } else {
        p->hidden++;
      }
    }
    p->count = kept;
  }
  sortHoldings(p->items, p->count);
  p->totalUsd = 0;
  p->totalUsd24hAgo = 0;
  p->anyPriced = false;
  for (int i = 0; i < p->count; i++) {
    const Holding& h = p->items[i];
    if (!h.priced) continue;
    double value = h.amount * h.usdPrice;
    p->totalUsd += value;
    // price now = price then * (1 + change/100), so price then = now / (1 + change/100)
    p->totalUsd24hAgo += h.hasChange ? value / (1.0 + h.change24h / 100.0) : value;
    p->anyPriced = true;
  }
}

void formatUsd(double usd, char* out, size_t cap) {
  const char* sign = usd < 0 ? "-" : "";
  double a = fabs(usd);
  char whole[24];
  if (a >= 10000) {
    withCommas((long long)llround(a), whole, sizeof whole);
    snprintf(out, cap, "%s$%s", sign, whole);
  } else if (a >= 1 || a == 0) {
    long long cents = llround(a * 100);
    withCommas(cents / 100, whole, sizeof whole);
    snprintf(out, cap, "%s$%s.%02lld", sign, whole, cents % 100);
  } else if (a >= 0.01) {
    snprintf(out, cap, "%s$%.2f", sign, a);
  } else {
    snprintf(out, cap, "%s$%.*f", sign, twoDigits(a), a);  // $0.0042: two digits that matter
  }
}

void formatAmount(double v, char* out, size_t cap) {
  if (v >= 1e9) snprintf(out, cap, "%.1fB", v / 1e9);
  else if (v >= 1e6) snprintf(out, cap, "%.1fM", v / 1e6);
  else if (v >= 1e4) snprintf(out, cap, "%.1fK", v / 1e3);
  else if (v >= 100) snprintf(out, cap, "%.0f", v);
  else if (v >= 1) snprintf(out, cap, "%.2f", v);
  else if (v == 0) snprintf(out, cap, "0");
  else snprintf(out, cap, "%.*f", twoDigits(v), v);
  // "6.10" -> "6.1", "210.00" never happens (no decimals from 100), "1.0M" -> "1M"
  char* dot = strchr(out, '.');
  if (dot != nullptr) {
    char* end = out + strlen(out);
    char suffix = 0;
    if (end > out && (end[-1] == 'K' || end[-1] == 'M' || end[-1] == 'B')) suffix = *--end;
    while (end > dot + 1 && end[-1] == '0') end--;
    if (end == dot + 1) end = dot;
    if (suffix) *end++ = suffix;
    *end = '\0';
  }
}

void formatChange(double now, double ref, char* out, size_t cap, bool* up) {
  double diff = now - ref;
  *up = diff >= 0;
  char usd[24];
  formatUsd(fabs(diff), usd, sizeof usd);
  double pct = ref > 0 ? fabs(diff) / ref * 100.0 : 0;
  // ▲ / ▼ as UTF-8 (U+25B2 / U+25BC); the Inter fonts carry them.
  snprintf(out, cap, "%s %s (%.1f%%)", *up ? "\xE2\x96\xB2" : "\xE2\x96\xBC", usd, pct);
}

void formatPercent(float pct, char* out, size_t cap, bool* up) {
  *up = pct >= 0;
  float a = fabsf(pct);
  if (a < 0.05f) {
    snprintf(out, cap, "0.0%%");
    *up = true;
    return;
  }
  snprintf(out, cap, "%s %.1f%%", *up ? "\xE2\x96\xB2" : "\xE2\x96\xBC", a);
}
