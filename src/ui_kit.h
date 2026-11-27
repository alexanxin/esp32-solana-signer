#pragma once
// Shared screen kit for the CYD: the screen + touch hardware, the design's colours and fonts,
// and the small builders every screen uses (so each screen reads like the design).
// Used by the Signer screens (ui_cyd.cpp) and the Dashboard screens (dash_ui.cpp).
#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

// Inter, made with scripts/make_fonts.sh (the design's font). The icons (LV_SYMBOL_...)
// live inside these fonts too.
LV_FONT_DECLARE(inter_12);
LV_FONT_DECLARE(inter_12_bold);
LV_FONT_DECLARE(inter_16_semibold);
LV_FONT_DECLARE(inter_28_bold);

constexpr int kScreenW = 240, kScreenH = 320;  // portrait, the CYD's real pixels
constexpr int kBootButton = 0;  // BOOT = lock / reject / back. It can NEVER approve.

// ---- Look (from the design canvas) -------------------------------------------------------
// "static": every file that includes this gets its own copy (they are just numbers).
static const lv_color_t kGround = lv_color_hex(0x0E1116);
// The design's card colour is #181D25, but this TN panel lifts dark tones (its gamma curve
// makes near-black greys look lighter than on a laptop). One step darker looks right on it.
static const lv_color_t kCard = lv_color_hex(0x13171E);
static const lv_color_t kText = lv_color_hex(0xEEF1F5);
static const lv_color_t kMuted = lv_color_hex(0xA3ADBA);
static const lv_color_t kLime = lv_color_hex(0xC7F284);
static const lv_color_t kAmber = lv_color_hex(0xFFC857);
static const lv_color_t kOrange = lv_color_hex(0xFF9F5A);
static const lv_color_t kRed = lv_color_hex(0xFF6B6B);  // red flags on the sign screen
static const lv_color_t kTrack = lv_color_hex(0x262D38);

// Screen, touch and backlight on; LVGL ready. Call once at power-on.
void kitBegin();

// ---- Small builders ----------------------------------------------------------------------
lv_obj_t* newScreen();
lv_obj_t* text(lv_obj_t* parent, const char* s, const lv_font_t* font, lv_color_t color, int x,
               int y, int width = 0);
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg, int radius);
void show(lv_obj_t* o, bool on);
lv_obj_t* pillButton(lv_obj_t* parent, int x, int y, int w, int h, const char* label,
                     lv_event_cb_t cb);
void backHeader(lv_obj_t* scr, const char* title, lv_event_cb_t onBack);
void devnetBadge(lv_obj_t* scr);
void groupAddress(const char* addr, char* out, size_t outCap);
void addSpan(lv_obj_t* group, const char* s, const lv_font_t* font, lv_color_t color);
void loadAndDropOld(lv_obj_t* scr);  // show scr, delete the screen that was up
