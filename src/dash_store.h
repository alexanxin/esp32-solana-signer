#pragma once
// What the Dashboard keeps in flash (NVS, folder "dash"). Nothing here is secret enough to
// need the PIN: the address is public, and the Wi-Fi password is the same one your phone
// keeps. The seed is never in here; it stays in the PIN vault (seed_store.cpp).
#include <stddef.h>
#include <stdint.h>

#include "activity.h"
#include "portfolio.h"

// The public addresses of Accounts 1-3, saved by the Signer each time you unlock it. The
// Dashboard shows the one chosen on the setup page. Returns false if no Signer unlock has
// saved that account yet.
constexpr uint8_t kDashAccounts = 3;
bool dashAddress(char out[45], uint8_t account = 1);
void dashSetAddress(const char* addr, uint8_t account = 1);  // writes only if it changed
// Forgets the cached portfolio and Activity (they belong to another account now).
void dashForgetCache();

struct DashSetup {
  char ssid[33];     // Wi-Fi name (32 bytes max in the Wi-Fi standard)
  char pass[65];     // Wi-Fi password (63 max)
  char tz[48];       // time zone as a POSIX rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
  char rpcUrl[128];  // optional own RPC (e.g. Helius); "" = the free public Solana RPC
  // The Dashboard's own settings (all on the setup page too):
  char watch[80];      // Market grid, in order: "BTC,ETH,SOL,JUP,BONK,WIF"
  bool euro;           // show money in euros instead of dollars
  uint8_t hideUnder;   // hide tokens worth less than this many dollars (0 = show all)
  uint8_t refreshMin;  // refresh every 1, 2 or 5 minutes
  uint8_t account;     // which account to show: 1, 2 or 3
};
// Returns false if Wi-Fi was never set up (ssid empty).
bool dashLoadSetup(DashSetup* out);
void dashSaveSetup(const DashSetup& s);
// Settings > "Set up again": asks the next start to open the setup network. Taking the
// request clears it, so it happens once.
void dashRequestSetup();
bool dashTakeSetupRequest();

// The last portfolio, so the Dashboard shows something at once after a restart (switching
// modes restarts) and while offline. `fetchedAt` is unix time (0 = clock not set then).
bool dashLoadPortfolio(Portfolio* p, uint32_t* fetchedAt);
void dashSavePortfolio(const Portfolio& p, uint32_t fetchedAt);

// The last Activity list, for the same reason.
bool dashLoadActivity(Activity* a, uint32_t* fetchedAt);
void dashSaveActivity(const Activity& a, uint32_t fetchedAt);
