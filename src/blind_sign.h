// The "Unknown transactions" switch in Settings.
//
// Off (the default): Sigly only signs what it can show you, a plain SOL transfer for now.
// Anything else (swaps, NFTs, token approvals, "sign in" messages) is shown and blocked.
// On: you may sign those too, after a 3 second hold, trusting the wallet app. That is
// "blind signing", the same switch a Ledger has.
//
// Why off by default: a hacked phone can't change what a SOL transfer screen shows, but it
// CAN dress a drain-your-wallet transaction up as "just a swap". Blocked by default means you
// turn it on for the minutes you need it, then off again.
#pragma once

bool blindSigningAllowed();             // read from flash once, then cached
void setBlindSigningAllowed(bool allowed);  // saves to flash at once
