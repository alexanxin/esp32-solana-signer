#ifdef BLEND_CYD
#include "dash_store.h"

#include <Preferences.h>  // key-value storage in flash ("NVS")
#include <string.h>

namespace {
const char* kFolder = "dash";

// A saved Portfolio is a copy of the struct's bytes. If a later firmware changes the struct,
// old bytes would be read wrongly, so we save its size too and ignore a size that differs.
struct SavedPortfolio {
  uint32_t size;
  uint32_t fetchedAt;
  Portfolio p;
};
struct SavedActivity {
  uint32_t size;
  uint32_t fetchedAt;
  Activity a;
};
}  // namespace

bool dashAddress(char out[45]) {
  Preferences prefs;
  prefs.begin(kFolder, true);  // true = read only
  size_t n = prefs.getString("addr", out, 45);
  prefs.end();
  return n > 1 && out[0] != '\0';
}

void dashSetAddress(const char* addr) {
  char now[45] = "";
  if (dashAddress(now) && strcmp(now, addr) == 0) return;
  Preferences prefs;
  prefs.begin(kFolder, false);
  prefs.putString("addr", addr);
  prefs.remove("pf");  // a portfolio of another address is no use any more
  prefs.remove("act");
  prefs.end();
}

bool dashLoadSetup(DashSetup* out) {
  memset(out, 0, sizeof *out);
  Preferences prefs;
  prefs.begin(kFolder, true);
  prefs.getString("ssid", out->ssid, sizeof out->ssid);
  prefs.getString("pass", out->pass, sizeof out->pass);
  prefs.getString("tz", out->tz, sizeof out->tz);
  prefs.getString("rpc", out->rpcUrl, sizeof out->rpcUrl);
  prefs.end();
  if (out->tz[0] == '\0') strcpy(out->tz, "CET-1CEST,M3.5.0,M10.5.0/3");  // Central Europe
  return out->ssid[0] != '\0';
}

void dashSaveSetup(const DashSetup& s) {
  Preferences prefs;
  prefs.begin(kFolder, false);
  prefs.putString("ssid", s.ssid);
  prefs.putString("pass", s.pass);
  prefs.putString("tz", s.tz);
  prefs.putString("rpc", s.rpcUrl);
  prefs.end();
}

void dashRequestSetup() {
  Preferences prefs;
  prefs.begin(kFolder, false);
  prefs.putBool("setupReq", true);
  prefs.end();
}

bool dashTakeSetupRequest() {
  Preferences prefs;
  prefs.begin(kFolder, false);
  bool req = prefs.getBool("setupReq", false);
  if (req) prefs.remove("setupReq");
  prefs.end();
  return req;
}

bool dashLoadPortfolio(Portfolio* p, uint32_t* fetchedAt) {
  static SavedPortfolio saved;  // static: 2 KB is too much for the stack
  Preferences prefs;
  prefs.begin(kFolder, true);
  size_t n = prefs.getBytes("pf", &saved, sizeof saved);
  prefs.end();
  if (n != sizeof saved || saved.size != sizeof(Portfolio)) return false;
  *p = saved.p;
  *fetchedAt = saved.fetchedAt;
  return true;
}

void dashSavePortfolio(const Portfolio& p, uint32_t fetchedAt) {
  static SavedPortfolio saved;
  saved.size = sizeof(Portfolio);
  saved.fetchedAt = fetchedAt;
  saved.p = p;
  Preferences prefs;
  prefs.begin(kFolder, false);
  prefs.putBytes("pf", &saved, sizeof saved);
  prefs.end();
}
// Made on the heap for the moment it's needed: ~1.2 KB that would otherwise sit in DRAM.
bool dashLoadActivity(Activity* a, uint32_t* fetchedAt) {
  SavedActivity* saved = new SavedActivity();
  Preferences prefs;
  prefs.begin(kFolder, true);
  size_t n = prefs.getBytes("act", saved, sizeof *saved);
  prefs.end();
  bool ok = n == sizeof *saved && saved->size == sizeof(Activity);
  if (ok) {
    *a = saved->a;
    *fetchedAt = saved->fetchedAt;
  }
  delete saved;
  return ok;
}

void dashSaveActivity(const Activity& a, uint32_t fetchedAt) {
  SavedActivity* saved = new SavedActivity();
  saved->size = sizeof(Activity);
  saved->fetchedAt = fetchedAt;
  saved->a = a;
  Preferences prefs;
  prefs.begin(kFolder, false);
  prefs.putBytes("act", saved, sizeof *saved);
  prefs.end();
  delete saved;
}
#endif  // BLEND_CYD
