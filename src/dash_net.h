#pragma once
// The Dashboard's internet side (CYD blend, Dashboard mode only; Signer mode never calls any
// of this, so Wi-Fi is never switched on while the seed can be unlocked).
//
// Two jobs:
//   1. Setup: Sigly opens its own small Wi-Fi network "Sigly-setup". You join it from your
//      phone and fill in a form (your Wi-Fi, time zone, optionally your own RPC).
//   2. Refresh: joins your Wi-Fi and every 2 minutes asks the Solana RPC for your balances
//      and Jupiter for prices. This runs in a background task, so the screen stays smooth
//      while it waits for answers (an HTTPS request takes a second or two on this chip).
#include <stdint.h>

#include "portfolio.h"

enum class NetState : uint8_t {
  kOff,         // dashNetBegin() not called
  kConnecting,  // joining your Wi-Fi
  kFetching,    // online, asking for balances and prices
  kOk,          // the last refresh worked
  kNoWifi,      // can't join the Wi-Fi (wrong password, out of range...)
  kRpcError,    // the Solana RPC didn't answer (or answered an error)
  kPriceError,  // balances fine, but the price API didn't answer
};

// Joins the saved Wi-Fi and starts refreshing `address`. Call once.
void dashNetBegin(const char* address);
NetState dashNetState();
bool dashNetConnected();

// Copies the newest portfolio into *out if there is a newer one than `seenVersion`.
// Returns the version you now have (0 = none yet). fetchedAt: unix time of that refresh.
uint32_t dashNetTakePortfolio(uint32_t seenVersion, Portfolio* out, uint32_t* fetchedAt);

// Ask for a refresh now (e.g. the "Try again" button).
void dashNetRefreshNow();

// ---- Setup network -----------------------------------------------------------------------

// Opens "Sigly-setup" with a fresh random password and starts the setup page.
// ssid / pass get the network's name and password (for the screen and its QR code).
void dashPortalBegin(char ssid[16], char pass[12]);
// Call often while the setup screen is up. Returns true once the form was saved (then
// restart: the Dashboard comes back up with the new settings).
bool dashPortalService();
