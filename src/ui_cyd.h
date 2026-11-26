#pragma once
#include <stddef.h>
#include <stdint.h>

// The CYD touchscreen (only built for the `cyd` blend). Screens follow the
// "Sigly CYD screens" design: 240x320 portrait, dark ground, lime accent.

void uiBegin(const char* deviceName);  // screen + touch on, shows "Waiting for your wallet"
void uiService();                      // call often from loop(): draws and reads touch

// The approval callback for apdu.cpp on the CYD. Shows what is being signed and waits for
// press-and-hold (approve), Reject, or 30 s (refuse). Returns true = sign.
bool uiApprove(const uint8_t* message, size_t len);
