#include "activity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portfolio.h"  // knownTokenSymbol, kWrappedSolMint

namespace {

void copyText(char* dst, size_t cap, const char* src) {
  snprintf(dst, cap, "%s", src != nullptr ? src : "");
}

const TxItem* findSig(const Activity& a, const char* sig) {
  for (int i = 0; i < a.count; i++) {
    if (strcmp(a.items[i].sig, sig) == 0) return &a.items[i];
  }
  return nullptr;
}

void symbolFor(const char* mint, char* out, size_t cap) {
  if (mint == nullptr || mint[0] == '\0' || strcmp(mint, kWrappedSolMint) == 0) {
    copyText(out, cap, "SOL");
    return;
  }
  const char* known = knownTokenSymbol(mint);
  if (known != nullptr) copyText(out, cap, known);
  else snprintf(out, cap, "%.4s...", mint);  // an honest address, not a name a scammer picked
}

// One token account's change in this transaction (before -> after).
struct TokenMove {
  int index;  // the account's place in accountKeys
  const char* mint;
  const char* owner;
  double delta;  // in whole tokens
};
constexpr int kMaxMoves = 16;

void addMoves(JsonArrayConst balances, double sign, TokenMove* moves, int* n) {
  for (JsonVariantConst b : balances) {
    int index = b["accountIndex"] | -1;
    // Read as text, like the RPC sends it: big token amounts don't fit a JSON number exactly.
    double v = atof(b["uiTokenAmount"]["uiAmountString"] | "0");
    TokenMove* m = nullptr;
    for (int i = 0; i < *n; i++) {
      if (moves[i].index == index) m = &moves[i];
    }
    if (m == nullptr) {
      if (*n >= kMaxMoves) continue;
      m = &moves[(*n)++];
      m->index = index;
      m->mint = b["mint"] | "";
      m->owner = b["owner"] | "";
      m->delta = 0;
    }
    m->delta += sign * v;
  }
}

// Your change of one asset: "" = SOL, else the token's mint.
struct Change {
  const char* mint;
  double delta;
};

void addChange(Change* list, int* n, const char* mint, double delta) {
  for (int i = 0; i < *n; i++) {
    if (strcmp(list[i].mint, mint) == 0) {
      list[i].delta += delta;
      return;
    }
  }
  if (*n < kMaxMoves) list[(*n)++] = {mint, delta};
}

// Picks the change to show for one direction (+1 in, -1 out): a token before SOL, because a
// SOL change next to a token one is usually just the fee for a new token account ("rent").
const Change* pick(const Change* list, int n, int direction) {
  const Change* sol = nullptr;
  for (int i = 0; i < n; i++) {
    if (list[i].delta * direction <= 0) continue;
    if (list[i].mint[0] != '\0') return &list[i];
    sol = &list[i];
  }
  return sol;
}

// The other wallet in a token transfer: an account of the same token, not yours, that moved
// the other way.
const char* tokenCounterparty(const TokenMove* moves, int n, const char* mint, double direction,
                              const char* owner) {
  for (int i = 0; i < n; i++) {
    const TokenMove& m = moves[i];
    if (strcmp(m.mint, mint) == 0 && strcmp(m.owner, owner) != 0 && m.delta * direction < 0) {
      return m.owner;
    }
  }
  return "";
}

// The other wallet in a plain SOL transfer (System Program "transfer").
const char* solCounterparty(JsonArrayConst instructions, const char* owner, bool sent) {
  for (JsonVariantConst ix : instructions) {
    const char* type = ix["parsed"]["type"] | "";
    if (strcmp(ix["program"] | "", "system") != 0 || strncmp(type, "transfer", 8) != 0) continue;
    const char* from = ix["parsed"]["info"]["source"] | "";
    const char* to = ix["parsed"]["info"]["destination"] | "";
    if (sent && strcmp(from, owner) == 0) return to;
    if (!sent && strcmp(to, owner) == 0) return from;
  }
  return "";
}

void applyOne(JsonVariantConst result, const char* owner, TxItem* t) {
  JsonVariantConst meta = result["meta"];
  JsonArrayConst keys = result["transaction"]["message"]["accountKeys"];
  if (meta.isNull() || keys.isNull()) return;  // stays kPending, asked again next time
  if (t->blockTime == 0) t->blockTime = result["blockTime"] | 0u;

  int me = -1, i = 0;
  for (JsonVariantConst k : keys) {
    if (strcmp(k["pubkey"] | "", owner) == 0) me = i;
    i++;
  }
  double fee = (meta["fee"] | 0.0) / 1e9;
  bool paidFee = me == 0;  // the first account pays the fee
  t->feeSol = paidFee ? fee : 0;
  if (!meta["err"].isNull()) {
    t->kind = TxKind::kFailed;
    return;
  }

  Change changes[kMaxMoves];
  int nChanges = 0;
  if (me >= 0) {
    double pre = meta["preBalances"][me] | 0.0;
    double post = meta["postBalances"][me] | 0.0;
    // Leave the fee out: "Sent 0.5 SOL" should say 0.5, not 0.500005.
    double sol = (post - pre) / 1e9 + (paidFee ? fee : 0);
    if (fabs(sol) >= 1e-9) addChange(changes, &nChanges, "", sol);
  }
  TokenMove moves[kMaxMoves];
  int nMoves = 0;
  addMoves(meta["preTokenBalances"], -1, moves, &nMoves);
  addMoves(meta["postTokenBalances"], +1, moves, &nMoves);
  for (int m = 0; m < nMoves; m++) {
    if (strcmp(moves[m].owner, owner) != 0 || fabs(moves[m].delta) < 1e-12) continue;
    // Wrapped SOL is SOL: a swap from SOL wraps and unwraps it inside the same transaction.
    const char* mint = strcmp(moves[m].mint, kWrappedSolMint) == 0 ? "" : moves[m].mint;
    addChange(changes, &nChanges, mint, moves[m].delta);
  }

  const Change* in = pick(changes, nChanges, +1);
  const Change* out = pick(changes, nChanges, -1);
  // A SOL change of less than 0.01 next to a token is rent for a token account, not a swap.
  if (in != nullptr && out != nullptr && in->mint[0] != '\0' && out->mint[0] == '\0' &&
      -out->delta < 0.01) {
    out = nullptr;
  }
  if (in != nullptr && out != nullptr && out->mint[0] != '\0' && in->mint[0] == '\0' &&
      in->delta < 0.01) {
    in = nullptr;
  }
  t->other[0] = '\0';
  if (in != nullptr && out != nullptr) {
    t->kind = TxKind::kSwap;
    symbolFor(in->mint, t->symbol, sizeof t->symbol);
    t->amount = in->delta;
    symbolFor(out->mint, t->outSymbol, sizeof t->outSymbol);
    t->outAmount = -out->delta;
    return;
  }
  const Change* c = in != nullptr ? in : out;
  if (c == nullptr) {
    t->kind = TxKind::kApp;
    return;
  }
  bool sent = c == out;
  t->kind = sent ? TxKind::kSent : TxKind::kReceived;
  symbolFor(c->mint, t->symbol, sizeof t->symbol);
  t->amount = fabs(c->delta);
  const char* who = c->mint[0] == '\0'
                        ? solCounterparty(result["transaction"]["message"]["instructions"], owner, sent)
                        : tokenCounterparty(moves, nMoves, c->mint, sent ? -1 : +1, owner);
  copyText(t->other, sizeof t->other, who);
}

}  // namespace

void activitySignaturesBody(const char* address, char* out, size_t cap) {
  snprintf(out, cap,
           R"({"jsonrpc":"2.0","id":3,"method":"getSignaturesForAddress","params":["%s",)"
           R"({"limit":%d}]})",
           address, kMaxActivity);
}

int activityFromSignatures(JsonVariantConst result, const Activity& old, Activity* out) {
  JsonArrayConst list = result.as<JsonArrayConst>();
  if (list.isNull()) return -1;
  memset(out, 0, sizeof *out);
  int pending = 0;
  for (JsonVariantConst s : list) {
    if (out->count >= kMaxActivity) break;
    const char* sig = s["signature"] | "";
    if (sig[0] == '\0') continue;
    TxItem& t = out->items[out->count++];
    const TxItem* seen = findSig(old, sig);
    if (seen != nullptr && seen->kind != TxKind::kPending) {
      t = *seen;
      continue;
    }
    copyText(t.sig, sizeof t.sig, sig);
    t.blockTime = s["blockTime"] | 0u;
    // A failed one only cost its fee: nothing more to ask about it.
    t.kind = s["err"].isNull() ? TxKind::kPending : TxKind::kFailed;
    if (t.kind == TxKind::kPending) pending++;
  }
  return pending;
}

void activityDetailsBody(const Activity& a, char* out, size_t cap) {
  size_t n = 0;
  out[0] = '\0';
  for (int i = 0; i < a.count; i++) {
    if (a.items[i].kind != TxKind::kPending) continue;
    int w = snprintf(out + n, cap - n,
                     R"(%c{"jsonrpc":"2.0","id":%d,"method":"getTransaction","params":["%s",)"
                     R"({"encoding":"jsonParsed","maxSupportedTransactionVersion":0}]})",
                     n == 0 ? '[' : ',', i, a.items[i].sig);
    if (w < 0 || n + w + 2 >= cap) break;  // full: the rest are asked next time
    n += w;
  }
  if (n > 0) snprintf(out + n, cap - n, "]");
}

void activityDetailsFilter(JsonDocument& filter) {
  filter.clear();
  JsonObject one = filter.add<JsonObject>();  // applies to every answer in the array
  one["id"] = true;
  one["error"] = true;
  JsonObject r = one["result"].to<JsonObject>();
  r["blockTime"] = true;
  JsonObject meta = r["meta"].to<JsonObject>();
  meta["err"] = true;
  meta["fee"] = true;
  meta["preBalances"] = true;
  meta["postBalances"] = true;
  for (const char* key : {"preTokenBalances", "postTokenBalances"}) {
    JsonObject b = meta[key].add<JsonObject>();
    b["accountIndex"] = true;
    b["mint"] = true;
    b["owner"] = true;
    b["uiTokenAmount"]["uiAmountString"] = true;
  }
  JsonObject msg = r["transaction"]["message"].to<JsonObject>();
  msg["accountKeys"].add<JsonObject>()["pubkey"] = true;
  JsonObject ix = msg["instructions"].add<JsonObject>();
  ix["program"] = true;
  ix["parsed"]["type"] = true;
  ix["parsed"]["info"]["source"] = true;
  ix["parsed"]["info"]["destination"] = true;
}

void activityApplyDetails(JsonVariantConst answer, const char* owner, Activity* a) {
  for (JsonVariantConst one : answer.as<JsonArrayConst>()) {
    int id = one["id"] | -1;
    if (id < 0 || id >= a->count || a->items[id].kind != TxKind::kPending) continue;
    if (one["result"].isNull()) continue;  // not found yet (too new) or an error: ask again
    applyOne(one["result"], owner, &a->items[id]);
  }
}

void formatAgo(uint32_t now, uint32_t t, char* out, size_t cap) {
  if (t == 0 || now == 0) {
    copyText(out, cap, "");
    return;
  }
  uint32_t s = now > t ? now - t : 0;
  if (s < 60) copyText(out, cap, "now");
  else if (s < 3600) snprintf(out, cap, "%um ago", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, cap, "%uh ago", (unsigned)(s / 3600));
  else snprintf(out, cap, "%ud ago", (unsigned)(s / 86400));
}

void shortAddress(const char* addr, char* out, size_t cap) {
  size_t n = strlen(addr);
  if (n <= 11) copyText(out, cap, addr);
  else snprintf(out, cap, "%.4s...%s", addr, addr + n - 4);
}
