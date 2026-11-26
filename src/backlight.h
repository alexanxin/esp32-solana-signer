// Screen brightness for the CYD: the backlight (GPIO 27) is dimmed with PWM.
#pragma once
#include <stdint.h>

void backlightBegin();             // call after the screen is set up; restores the saved level
void backlightSet(uint8_t percent);  // 10..100, takes effect at once
uint8_t backlightPercent();
void backlightSave();              // write the current level to flash (call when the finger lifts)
