#pragma once
// The CYD's RGB status light (on the back of the board). CYD only.
//   Dashboard: lime = your portfolio is up over 24 h, orange = down; a few white flashes
//              when a new transaction shows up in Activity.
//   Signer:    blue breathing while a phone is connected, amber breathing while a
//              transaction waits for your approval; green flashes = signed, red = rejected.
// Settings > Status light: Off / Dim / Normal (saved in flash).
#include "led_pattern.h"

void ledBegin();  // call once at start; restores the saved level
void ledSetLook(LedMode mode, Rgb color, uint16_t periodMs = 0);  // the steady look
void ledBlink(Rgb color, uint8_t times);  // quick flashes on top, then back to the look
void ledService();  // call often (every loop): updates the light

LedLevel ledLevel();
void ledSetLevel(LedLevel level);  // takes effect at once and is saved
