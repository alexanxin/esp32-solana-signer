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
void settingsRow(lv_obj_t* scr, int y, const char* label, const char* value);
void brightnessRow(lv_obj_t* scr, int y);  // "Brightness 70%" + slider, saved on release
void addressBlock(lv_obj_t* scr, const char* addr);  // QR + grouped address + "trust this screen"

// Font Awesome icons that LVGL has no LV_SYMBOL_ for (scripts/make_fonts.sh puts them in
// inter_16_semibold). The \x.. are the UTF-8 bytes of the icon's code point.
#define SIG_SYMBOL_QRCODE "\xEF\x80\xA9"       // U+F029
#define SIG_SYMBOL_PAPER_PLANE "\xEF\x87\x98"  // U+F1D8
#define SIG_SYMBOL_COINS "\xEF\x94\x9E"        // U+F51E
#define SIG_SYMBOL_ARROW_UP "\xEF\x81\xA2"     // U+F062
#define SIG_SYMBOL_ARROW_DOWN "\xEF\x81\xA3"   // U+F063
#define SIG_SYMBOL_CUBE "\xEF\x86\xB2"         // U+F1B2
#define SIG_SYMBOL_EXCHANGE "\xEF\x8D\xA2"     // U+F362
#define SIG_SYMBOL_CHART_LINE "\xEF\x88\x81"   // U+F201
