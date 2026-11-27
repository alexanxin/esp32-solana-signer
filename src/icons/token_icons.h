#pragma once
// Round 24 x 24 token logos built into the firmware (made by scripts/make_token_icons.py
// from Jupiter's token list). Plain C so the generated .c file stays simple.
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// The logo for a symbol as the board shows it ("SOL", "USDC", "BTC"...), or NULL if there
// is none (the row then shows the first-letter badge).
const lv_image_dsc_t* tokenIcon(const char* symbol);

#ifdef __cplusplus
}
#endif
