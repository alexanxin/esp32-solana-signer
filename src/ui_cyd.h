#pragma once
#include <stddef.h>
#include <stdint.h>

// The CYD touchscreen (only built for the `cyd` blend). Screens follow the
// "Sigly CYD screens" design: 240x320 portrait, dark ground, lime accent.

void uiBegin(const char* deviceName);  // screen + touch on, shows the device name

// Call after Bluetooth is on (the PIN keypad shuffle and a new seed need real randomness).
// Shows the PIN screen: "Enter PIN" if the seed is locked, "Choose a PIN" on the first boot
// or when an older firmware left the seed unlocked. Once unlocked it calls setSeed() and
// shows "Waiting for your wallet". Until then every key request is refused.
void uiStartLocked();
void uiService();                      // call often from loop(): draws and reads touch

// The approval callback for apdu.cpp on the CYD. Shows what is being signed and waits for
// press-and-hold (approve), Reject, or 30 s (refuse). Returns true = sign.
// Afterwards a receipt stays up until Done, BOOT, 60 s, or the next request.
bool uiApprove(const uint8_t* message, size_t len);

// Called right after signing: puts the signature (the transaction id) on the receipt.
void uiShowSignature(const uint8_t signature[64]);
