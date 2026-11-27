#pragma once
// Sigly has two modes and never runs both at once (CYD blend only):
//
//   Dashboard: Wi-Fi on, Bluetooth off. Shows your portfolio. Never asks for the PIN, so the
//              seed stays locked in flash the whole time. This is what Sigly starts in.
//   Signer:    Bluetooth on, Wi-Fi off. Asks for the PIN, then signs what you approve.
//
// Switching always restarts the board, so nothing from one mode is left in memory for the
// other. The Send button writes a one-time "start in Signer" ticket and restarts; the next
// start tears the ticket up straight away. So a crash, a power cut or a lock in Signer mode
// always lands back in the Dashboard, with the seed locked.

enum class SiglyMode { kDashboard, kSigner };

// Which mode this start is in. Reads the ticket and tears it up (call once, early).
SiglyMode modeAtBoot();

// Restarts the board into `mode`. Does not return.
void restartInto(SiglyMode mode);
