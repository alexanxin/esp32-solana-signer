#pragma once
// What the Dashboard keeps in flash (NVS, folder "dash"). Nothing here is secret enough to
// need the PIN: the address is public, and the Wi-Fi password is the same one your phone
// keeps. The seed is never in here; it stays in the PIN vault (seed_store.cpp).
#include <stddef.h>
#include <stdint.h>

#include "portfolio.h"

// Account 1's public address, saved by the Signer each time you unlock it. The Dashboard
// shows this account. Returns false if no Signer unlock has saved one yet.
bool dashAddress(char out[45]);
void dashSetAddress(const char* addr);  // writes only if it changed (flash wears out)

struct DashSetup {
  char ssid[33];     // Wi-Fi name (32 bytes max in the Wi-Fi standard)
  char pass[65];     // Wi-Fi password (63 max)
  char tz[48];       // time zone as a POSIX rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
  char rpcUrl[128];  // optional own RPC (e.g. Helius); "" = the free public Solana RPC
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
