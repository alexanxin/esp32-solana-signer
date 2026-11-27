#pragma once

// The Sigly logo animation played once at power-on (CYD blend only).
//
// Call it after LVGL and the screen are set up. It takes about 3.6 seconds and blocks while it
// plays (nothing else needs the board that early). When it returns, the finished boot screen
// (logo, name, full bar, DEVNET badge) is still showing; the next screen you load replaces it.
void bootAnimationPlay(const char* deviceName);
