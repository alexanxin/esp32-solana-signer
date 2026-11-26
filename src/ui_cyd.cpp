// CYD screen and touch: LVGL 9 draws, TFT_eSPI talks to the ILI9341 screen, XPT2046 reads touch.
// Only compiled for the `cyd` blend (see platformio.ini).
#ifdef BLEND_CYD

#include "ui_cyd.h"

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "ble_transport.h"
#include "keys.h"
#include "log.h"
#include "tx_decode.h"

// Inter, made with scripts/make_fonts.sh (the design's font; LVGL ships only Montserrat).
LV_FONT_DECLARE(inter_12);
LV_FONT_DECLARE(inter_12_bold);
LV_FONT_DECLARE(inter_16_semibold);
LV_FONT_DECLARE(inter_28_bold);

namespace {

// ---- Hardware ----------------------------------------------------------------------------

constexpr int kScreenW = 240, kScreenH = 320;  // portrait, the CYD's real pixels

// Board: the CYD variant from our store (same as the "CYD tidal remote" project, which works on it).
// Unlike the common ESP32-2432S028R, the touch chip SHARES the screen's SPI bus (TOUCH_CS = 33,
// set in platformio.ini) and the backlight is on GPIO 27. TFT_eSPI reads touch for us.

// Touch calibration, measured on THIS board on 2026-11-26 (corner taps in the serial log).
// The touch panel is mounted turned 90 degrees to the screen: raw x follows the screen's
// height (top -> bottom) and raw y follows its width, reversed (left -> right = big -> small).
// map() is linear, so a reversed axis just has MIN > MAX.
constexpr int kRawYTop = 268, kRawYBottom = 3650;  // raw x at screen y = 0 / y = 320
constexpr int kRawXLeft = 3765, kRawXRight = 310;  // raw y at screen x = 0 / x = 240
constexpr int kPressMin = 250, kPressMax = 4000;  // pressure window: outside = no finger
constexpr int kRawSaneMin = 80, kRawSaneMax = 4050;  // raw values outside this are noise

constexpr int kBootButton = 0;  // BOOT = lock / reject. It can NEVER approve.

TFT_eSPI gTft;  // screen + touch, one SPI bus
// alignas(4): LVGL 9 refuses a buffer whose address is not a multiple of 4 (LV_DRAW_BUF_ALIGN).
alignas(4) uint8_t gDrawBuf[kScreenW * 10 * 2];  // LVGL draws 10 lines at a time (16-bit colour)

// ---- Look (from the design canvas) -------------------------------------------------------

const lv_color_t kGround = lv_color_hex(0x0E1116);
// The design's card colour is #181D25, but this TN panel lifts dark tones (its gamma curve
// makes near-black greys look lighter than on a laptop). One step darker looks right on it.
const lv_color_t kCard = lv_color_hex(0x13171E);
const lv_color_t kText = lv_color_hex(0xEEF1F5);
const lv_color_t kMuted = lv_color_hex(0xA3ADBA);
const lv_color_t kLime = lv_color_hex(0xC7F284);
const lv_color_t kAmber = lv_color_hex(0xFFC857);
const lv_color_t kOrange = lv_color_hex(0xFF9F5A);
const lv_color_t kTrack = lv_color_hex(0x262D38);

// ---- Approval state ----------------------------------------------------------------------

constexpr uint32_t kTimeoutMs = 30000;
constexpr uint32_t kHoldKnownMs = 1500;    // plain SOL transfer
constexpr uint32_t kHoldUnknownMs = 3000;  // anything we can't read: more effort on purpose

enum Decision { kPending, kApproved, kRejected };
Decision gDecision = kPending;
bool gHolding = false;
uint32_t gHoldStart = 0;
uint32_t gHoldMs = kHoldKnownMs;

lv_obj_t* gArc = nullptr;       // the ring inside the hold button
lv_obj_t* gHint = nullptr;      // "Let go too early" etc.
lv_obj_t* gCountdown = nullptr;
lv_obj_t* gTopBar = nullptr;
lv_obj_t* gSigLabel = nullptr;  // receipt row that gets the signature after signing
TxSummary gLastTx;              // what was just signed, for the receipt
bool gResultOpen = false;       // receipt on screen
bool gDoneTapped = false;
uint32_t gResultShownAt = 0;
constexpr uint32_t kReceiptMs = 60000;  // receipt goes away by itself after a minute

// Hold overlay (design 16): big ring over the sign screen while you hold, "let go too early",
// "Signing...". It never takes touches, so Reject and Hold underneath keep working.
lv_obj_t* gOverlay = nullptr;
lv_obj_t* gBigArc = nullptr;
lv_obj_t* gGlow = nullptr;    // soft circle behind the hand
lv_obj_t* gHand = nullptr;
lv_obj_t* gArrow = nullptr;
lv_obj_t* gCheck = nullptr;   // filled circle with a tick
lv_obj_t* gOvTitle = nullptr;
lv_obj_t* gOvBody = nullptr;
lv_color_t gAccent;           // lime for a transfer, amber for "can't read"
uint32_t gEarlyAt = 0;        // when "let go too early" appeared (0 = not showing)
bool gAwaitingSig = false;    // approved, "Signing..." up, receipt follows with the signature
uint32_t gApprovedAt = 0;

// Signer idle screen and its little sub-screens.
enum Nav { kNavNone, kNavIdle, kNavReceive, kNavSettings };
Nav gNav = kNavNone;          // set by buttons, acted on in uiService (never delete inside an event)
bool gIdleOpen = false;
int gShownConnected = -1;     // what the pill shows now: -1 unknown, 0 waiting, 1 connected
lv_obj_t* gPillDot = nullptr;
lv_obj_t* gPillLabel = nullptr;
const char* gDeviceName = "Sigly";

uint32_t tick() { return millis(); }

// LVGL hands us a finished strip of pixels; we push it to the screen over SPI.
void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  gTft.startWrite();
  gTft.setAddrWindow(area->x1, area->y1, w, h);
  gTft.pushColors((uint16_t*)px, w * h, true);  // true = swap bytes (LVGL is little-endian)
  gTft.endWrite();
  lv_display_flush_ready(disp);
}

void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  data->state = LV_INDEV_STATE_RELEASED;
  uint16_t z = gTft.getTouchRawZ();  // pressure: how hard (or whether) a finger presses
  if (z < kPressMin || z > kPressMax) return;
  uint16_t rx = 0, ry = 0;
  if (!gTft.getTouchRaw(&rx, &ry)) return;
  if (rx < kRawSaneMin || rx > kRawSaneMax || ry < kRawSaneMin || ry > kRawSaneMax) return;

  int x = map(ry, kRawXLeft, kRawXRight, 0, kScreenW);  // swapped: raw y gives screen x
  int y = map(rx, kRawYTop, kRawYBottom, 0, kScreenH);  // and raw x gives screen y
  data->point.x = constrain(x, 0, kScreenW - 1);
  data->point.y = constrain(y, 0, kScreenH - 1);
  data->state = LV_INDEV_STATE_PRESSED;

  static uint32_t lastLog = 0;  // log a touch at most 4 times a second
  if (millis() - lastLog > 250) {
    lastLog = millis();
    Serial.printf("[%6lu ms] touch raw %u,%u z %u -> pixel %d,%d\n", millis(), rx, ry, z,
                  (int)data->point.x, (int)data->point.y);
  }
}

// ---- Small builders, so each screen reads like the design ---------------------------------

lv_obj_t* newScreen() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, kGround, 0);
  lv_obj_set_style_text_color(scr, kText, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

lv_obj_t* text(lv_obj_t* parent, const char* s, const lv_font_t* font, lv_color_t color, int x,
               int y, int width = 0) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, s);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }
  lv_obj_set_pos(label, x, y);
  return label;
}

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg, int radius) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, radius, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  return b;
}

void show(lv_obj_t* o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* pillButton(lv_obj_t* parent, int x, int y, int w, int h, const char* label,
                     lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_bg_color(b, kCard, 0);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_center(text(b, label, &inter_12_bold, kText, 0, 0));
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// "<" back button + title, like the Receive and Settings designs.
void backHeader(lv_obj_t* scr, const char* title, lv_event_cb_t onBack) {
  lv_obj_t* back = lv_button_create(scr);
  lv_obj_set_pos(back, 4, 0);
  lv_obj_set_size(back, 44, 44);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_center(text(back, LV_SYMBOL_LEFT, &lv_font_montserrat_16, kText, 0, 0));
  lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* t = text(scr, title, &inter_16_semibold, kText, 52, 0);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 52, 12);
}

void devnetBadge(lv_obj_t* scr) {
  lv_obj_t* badge = box(scr, 10, 4, 58, 16, kLime, 4);
  lv_obj_t* label = text(badge, "DEVNET", &inter_12_bold, kGround, 0, 0);
  lv_obj_center(label);
}

// "C77JeUzT4mWd..." -> "C77J eUzT 4mWd ..." so a human can compare group by group.
void groupAddress(const char* addr, char* out, size_t outCap) {
  size_t n = 0;
  for (size_t i = 0; addr[i] != '\0' && n + 2 < outCap; i++) {
    if (i > 0 && i % 4 == 0) out[n++] = ' ';
    out[n++] = addr[i];
  }
  out[n] = '\0';
}

// One coloured piece of text inside a span group (LVGL's way to mix colours in a paragraph).
void addSpan(lv_obj_t* group, const char* s, const lv_font_t* font, lv_color_t color) {
  lv_span_t* span = lv_spangroup_new_span(group);
  lv_span_set_text(span, s);
  lv_style_set_text_font(lv_span_get_style(span), font);
  lv_style_set_text_color(lv_span_get_style(span), color);
}

// ---- Screens -----------------------------------------------------------------------------

// Hand and arrow outlines from the design's SVG (132x132 box), drawn as lines.
const lv_point_precise_t kHandPts[] = {{58, 76}, {58, 54}, {59, 51}, {62, 50}, {65, 51}, {66, 54},
                                       {66, 68}, {76, 70}, {79, 72}, {80, 76}, {78, 86}, {62, 86},
                                       {58, 76}};
const lv_point_precise_t kArrowShaft[] = {{50, 66}, {82, 66}};
const lv_point_precise_t kArrowHead[] = {{62, 54}, {50, 66}, {62, 78}};

lv_obj_t* drawLine(lv_obj_t* parent, const lv_point_precise_t* pts, uint32_t n, lv_color_t c, int w) {
  lv_obj_t* l = lv_line_create(parent);
  lv_line_set_points(l, pts, n);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_width(l, w, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_pos(l, 0, 0);
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

void buildOverlay(lv_obj_t* scr, const char* summary) {
  gOverlay = box(scr, 0, 0, 240, 320, kGround, 0);
  lv_obj_remove_flag(gOverlay, LV_OBJ_FLAG_CLICKABLE);  // touches fall through to the buttons
  lv_obj_t* sum = text(gOverlay, summary, &inter_12, kMuted, 0, 0);
  lv_obj_align(sum, LV_ALIGN_TOP_MID, 0, 26);

  lv_obj_t* art = box(gOverlay, 54, 50, 132, 132, kGround, 0);  // the design's 132x132 drawing
  lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
  gGlow = box(art, 28, 28, 76, 76, gAccent, 38);
  lv_obj_set_style_bg_opa(gGlow, LV_OPA_10, 0);
  lv_obj_remove_flag(gGlow, LV_OBJ_FLAG_CLICKABLE);
  gCheck = box(art, 28, 28, 76, 76, gAccent, 38);
  lv_obj_remove_flag(gCheck, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(text(gCheck, LV_SYMBOL_OK, &lv_font_montserrat_28, kGround, 0, 0));

  gBigArc = lv_arc_create(art);
  lv_obj_set_size(gBigArc, 122, 122);
  lv_obj_center(gBigArc);
  lv_arc_set_rotation(gBigArc, 270);
  lv_arc_set_bg_angles(gBigArc, 0, 360);
  lv_arc_set_range(gBigArc, 0, 100);
  lv_obj_remove_style(gBigArc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(gBigArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(gBigArc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(gBigArc, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(gBigArc, kTrack, LV_PART_MAIN);

  gHand = lv_obj_create(art);  // a transparent holder, so the line points are in SVG coordinates
  lv_obj_remove_style_all(gHand);
  lv_obj_set_size(gHand, 132, 132);
  lv_obj_remove_flag(gHand, LV_OBJ_FLAG_CLICKABLE);
  drawLine(gHand, kHandPts, sizeof kHandPts / sizeof kHandPts[0], gAccent, 3);
  gArrow = lv_obj_create(art);
  lv_obj_remove_style_all(gArrow);
  lv_obj_set_size(gArrow, 132, 132);
  lv_obj_remove_flag(gArrow, LV_OBJ_FLAG_CLICKABLE);
  drawLine(gArrow, kArrowShaft, 2, kMuted, 3);
  drawLine(gArrow, kArrowHead, 3, kMuted, 3);

  gOvTitle = text(gOverlay, "", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(gOvTitle, LV_ALIGN_TOP_MID, 0, 194);
  gOvBody = text(gOverlay, "", &inter_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(gOvBody, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(gOvBody, LV_ALIGN_TOP_MID, 0, 220);
  show(gOverlay, false);
}

void overlayHolding() {
  gEarlyAt = 0;
  show(gOverlay, true);
  show(gGlow, true);
  show(gHand, true);
  show(gArrow, false);
  show(gCheck, false);
  lv_obj_set_style_arc_color(gBigArc, gAccent, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(gBigArc, kTrack, LV_PART_MAIN);
  lv_arc_set_value(gBigArc, 0);
  lv_label_set_text(gOvTitle, "Keep holding...");
  lv_label_set_text(gOvBody, gHoldMs == kHoldKnownMs ? "The ring fills in 1.5 s."
                                                     : "The ring fills in 3 s. Only if you started this.");
  lv_obj_align(gOvTitle, LV_ALIGN_TOP_MID, 0, 194);
}

void overlayTooEarly() {
  gEarlyAt = millis();
  show(gGlow, false);
  show(gHand, false);
  show(gArrow, true);
  lv_obj_set_style_arc_color(gBigArc, kMuted, LV_PART_INDICATOR);  // the ring stays where you let go
  lv_label_set_text(gOvTitle, "Let go too early");
  lv_label_set_text(gOvBody, "Nothing was signed. Hold again, or tap Reject.");
  lv_obj_align(gOvTitle, LV_ALIGN_TOP_MID, 0, 194);
}

void overlaySigning() {
  gEarlyAt = 0;
  show(gOverlay, true);
  show(gGlow, false);
  show(gHand, false);
  show(gArrow, false);
  show(gCheck, true);
  lv_obj_set_style_arc_color(gBigArc, gAccent, LV_PART_MAIN);
  lv_obj_set_style_arc_color(gBigArc, gAccent, LV_PART_INDICATOR);
  lv_arc_set_value(gBigArc, 100);
  lv_label_set_text(gOvTitle, "Signing...");
  lv_label_set_text(gOvBody, "Sending the signature to your phone.");
  lv_obj_align(gOvTitle, LV_ALIGN_TOP_MID, 0, 194);
}

void onHoldEvent(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    gHolding = true;
    gHoldStart = millis();
    overlayHolding();
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (gHolding && gDecision == kPending) {
      overlayTooEarly();
      lv_arc_set_value(gArc, 0);
      lv_obj_set_width(gTopBar, 0);
    }
    gHolding = false;
  }
}

void onRejectEvent(lv_event_t*) { gDecision = kRejected; }

void showSignScreen(const TxSummary& tx) {
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  gCountdown = text(scr, "0:30", &inter_12, kMuted, 196, 6);

  char buf[96];
  bool known = tx.valid && tx.kind == TxSummary::kSolTransfer;

  int feeY = 214;  // where the fee line goes; a transfer's card is short, so it moves up
  if (known) {
    char amount[24], addr[45], grouped[64];
    formatSol(tx.lamports, amount);
    text(scr, "Send", &inter_12, kMuted, 16, 30);
    snprintf(buf, sizeof buf, "%s SOL", amount);
    text(scr, buf, &inter_28_bold, kText, 16, 46, 208);

    // Full address in groups of 4, first and last 4 letters in bold lime: those are what
    // wallets show (C77J...No8s), the groups in between are for a careful check.
    lv_obj_t* card = box(scr, 16, 86, 208, 0, kCard, 8);
    lv_obj_set_height(card, LV_SIZE_CONTENT);  // grow to fit the address (2 lines, sometimes 3)
    lv_obj_set_style_pad_bottom(card, 10, 0);
    text(card, "To \xC2\xB7 check every group", &inter_12, kMuted, 10, 8);
    base58Encode(tx.to, addr);
    groupAddress(addr, grouped, sizeof grouped);
    size_t len = strlen(addr);
    size_t lastStart = (len - 4) + (len - 4) / 4;  // where address letter len-4 sits in `grouped`
    char head[5], mid[64];
    snprintf(head, sizeof head, "%.4s", grouped);
    snprintf(mid, sizeof mid, "%.*s", (int)(lastStart - 4), grouped + 4);

    lv_obj_t* spans = lv_spangroup_create(card);
    lv_obj_set_pos(spans, 10, 26);
    lv_obj_set_width(spans, 188);
    lv_spangroup_set_mode(spans, LV_SPAN_MODE_BREAK);  // wrap lines, grow downwards
    lv_obj_set_style_text_line_space(spans, 3, 0);
    addSpan(spans, head, &inter_12_bold, kLime);
    addSpan(spans, mid, &inter_12, kText);
    addSpan(spans, grouped + lastStart, &inter_12_bold, kLime);
    lv_spangroup_refr_mode(spans);
    lv_obj_update_layout(card);
    feeY = lv_obj_get_y(card) + lv_obj_get_height(card) + 10;
    if (feeY < 150 || feeY > 214) feeY = 190;  // layout not measured for some reason: safe spot
    gHoldMs = kHoldKnownMs;
  } else {
    // Amber warning box: icon + bold first sentence, like the design.
    lv_obj_t* warn = box(scr, 16, 30, 208, 0, lv_color_hex(0x2B2414), 8);
    lv_obj_set_height(warn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(warn, 8, 0);
    lv_obj_set_style_border_color(warn, kAmber, 0);
    lv_obj_set_style_border_width(warn, 1, 0);
    text(warn, LV_SYMBOL_WARNING, &lv_font_montserrat_16, kAmber, 8, 8);
    lv_obj_t* msg = lv_spangroup_create(warn);
    lv_obj_set_pos(msg, 32, 8);
    lv_obj_set_width(msg, 166);
    lv_spangroup_set_mode(msg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_line_space(msg, 2, 0);
    if (tx.valid) {
      addSpan(msg, "Can't read this one. ", &inter_12_bold, kAmber);
      addSpan(msg, "Only sign if you just started it in your wallet.", &inter_12, kText);
    } else {
      addSpan(msg, "Not a Solana transaction. ", &inter_12_bold, kAmber);
      addSpan(msg, "Reject it.", &inter_12, kText);
    }
    lv_spangroup_refr_mode(msg);
    lv_obj_update_layout(warn);
    int y = lv_obj_get_y(warn) + lv_obj_get_height(warn) + 8;
    if (y < 80 || y > 120) y = 100;

    snprintf(buf, sizeof buf, "It talks to %u program%s", (unsigned)tx.programCount,
             tx.programCount == 1 ? "" : "s");
    text(scr, buf, &inter_12, kMuted, 16, y);
    y += 18;
    for (size_t i = 0; i < tx.programCount && i < 3; i++) {
      lv_obj_t* row = box(scr, 16, y, 208, 30, kCard, 8);
      y += 34;
      const char* name = knownProgramName(tx.programs[i]);
      char addr[45], shortName[24];
      if (name == nullptr) {
        base58Encode(tx.programs[i], addr);
        size_t len = strlen(addr);
        snprintf(shortName, sizeof shortName, "%.4s...%s", addr, addr + (len > 4 ? len - 4 : 0));
      }
      lv_obj_t* n = text(row, name ? name : shortName, &inter_12_bold, kText, 10, 0);
      lv_obj_align(n, LV_ALIGN_LEFT_MID, 10, 0);
      lv_obj_t* tag = text(row, name ? "known" : "unknown", &inter_12, name ? kMuted : kAmber, 0, 0);
      lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    feeY = y + 2;  // no fee line here (the design leaves it out); the hint goes under the list
    gHoldMs = kHoldUnknownMs;
  }

  if (known) {
    char fee[24];
    formatSol(tx.feeLamports, fee);
    snprintf(buf, sizeof buf, "Network fee  %s SOL", fee);
    text(scr, buf, &inter_12, kMuted, 16, feeY);
  }

  gHint = text(scr, "", &inter_12, kMuted, 16, known ? feeY + 22 : feeY, 208);

  // Reject: outlined pill, bottom left.
  lv_obj_t* reject = lv_button_create(scr);
  lv_obj_set_pos(reject, 12, 256);
  lv_obj_set_size(reject, 60, 52);
  lv_obj_set_style_bg_opa(reject, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(reject, lv_color_hex(0x3A4250), 0);
  lv_obj_set_style_border_width(reject, 2, 0);
  lv_obj_set_style_radius(reject, 26, 0);
  lv_obj_set_style_shadow_width(reject, 0, 0);
  lv_obj_center(text(reject, "Reject", &inter_12, kText, 0, 0));
  lv_obj_add_event_cb(reject, onRejectEvent, LV_EVENT_CLICKED, nullptr);

  // Hold to sign: big pill with a ring that fills while you hold.
  lv_obj_t* hold = lv_button_create(scr);
  lv_obj_set_pos(hold, 80, 256);
  lv_obj_set_size(hold, 148, 52);
  lv_obj_set_style_pad_all(hold, 0, 0);  // LVGL buttons have inner padding by default; it pushed the text off the edge
  lv_obj_set_style_bg_color(hold, known ? kLime : kAmber, 0);
  lv_obj_set_style_radius(hold, 26, 0);
  lv_obj_set_style_shadow_width(hold, 0, 0);
  lv_obj_add_event_cb(hold, onHoldEvent, LV_EVENT_ALL, nullptr);

  gArc = lv_arc_create(hold);
  lv_obj_set_size(gArc, 26, 26);
  lv_obj_align(gArc, LV_ALIGN_LEFT_MID, 10, 0);
  lv_arc_set_rotation(gArc, 270);
  lv_arc_set_bg_angles(gArc, 0, 360);
  lv_arc_set_range(gArc, 0, 100);
  lv_arc_set_value(gArc, 0);
  lv_obj_remove_style(gArc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(gArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(gArc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(gArc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(gArc, kGround, LV_PART_MAIN);  // empty track: the dark colour, see-through
  lv_obj_set_style_arc_opa(gArc, LV_OPA_30, LV_PART_MAIN);  // so the filling ring (100% dark) stands out
  lv_obj_set_style_arc_color(gArc, kGround, LV_PART_INDICATOR);

  lv_obj_t* label = text(hold, known ? "Hold to sign" : "Hold 3 s", &inter_16_semibold, kGround, 0, 0);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 44, 0);  // just right of the ring, vertically centred

  gAccent = known ? kLime : kAmber;
  char summary[64];
  if (known) {
    char amount[24], addr[45];
    formatSol(tx.lamports, amount);
    base58Encode(tx.to, addr);
    size_t len = strlen(addr);
    snprintf(summary, sizeof summary, "%s SOL to %.4s...%s", amount, addr, addr + len - 4);
  } else {
    snprintf(summary, sizeof summary, "Transaction Sigly can't read");
  }
  buildOverlay(scr, summary);

  // Progress bar along the very top: starts 0 px wide, grows to the full 240 px while you hold.
  gTopBar = box(scr, 0, 0, 0, 3, gAccent, 0);

  lv_screen_load(scr);
}

void onMyAddress(lv_event_t*) { gNav = kNavReceive; }
void onSettings(lv_event_t*) { gNav = kNavSettings; }
void onBack(lv_event_t*) { gNav = kNavIdle; }

// Screens other than the idle one call this first, so stale pointers are never used.
void leaveIdle() {
  gIdleOpen = false;
  gPillDot = nullptr;
  gPillLabel = nullptr;
}

void loadAndDropOld(lv_obj_t* scr) {
  lv_obj_t* old = lv_screen_active();
  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
}

void showIdle(const char* deviceName) {
  gSigLabel = nullptr;  // that label is about to be deleted with its screen
  gResultOpen = false;
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  lv_obj_t* bt = text(scr, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_12, kMuted, 0, 0);
  lv_obj_align(bt, LV_ALIGN_TOP_RIGHT, -52, 6);
  lv_obj_t* who = text(scr, "Signer", &inter_12, kMuted, 0, 0);
  lv_obj_align(who, LV_ALIGN_TOP_RIGHT, -10, 6);

  // Three rings, like the design: faint, medium, solid with a dark centre.
  const int sizes[] = {88, 64, 40};
  const lv_opa_t opas[] = {LV_OPA_20, LV_OPA_40, LV_OPA_COVER};
  for (int i = 0; i < 3; i++) {
    lv_obj_t* r = box(scr, 0, 0, sizes[i], sizes[i], i == 2 ? kCard : kGround, sizes[i] / 2);
    lv_obj_set_style_bg_opa(r, i == 2 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, kLime, 0);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_opa(r, opas[i], 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 48 + (88 - sizes[i]) / 2);
  }
  lv_obj_t* mid = text(scr, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_16, kLime, 0, 0);
  lv_obj_align(mid, LV_ALIGN_TOP_MID, 0, 48 + 44 - 8);

  lv_obj_t* title = text(scr, "Send from your wallet app", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 146);
  char buf[96];
  snprintf(buf, sizeof buf, "Open Phantom or Jupiter and send. %s shows up as a Ledger.",
           deviceName);
  lv_obj_t* sub = text(scr, buf, &inter_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 172);

  // "Phone connected" pill; uiService() keeps it up to date.
  lv_obj_t* pill = box(scr, 0, 0, 0, 24, kCard, 12);
  lv_obj_set_width(pill, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(pill, 10, 0);
  lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(pill, 6, 0);
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 218);
  gPillDot = box(pill, 0, 0, 8, 8, kMuted, 4);
  gPillLabel = text(pill, "", &inter_12, kText, 0, 0);
  gShownConnected = -1;
  gIdleOpen = true;

  pillButton(scr, 12, 264, 104, 44, "My address", onMyAddress);
  pillButton(scr, 124, 264, 104, 44, "Settings", onSettings);

  loadAndDropOld(scr);
}

void updateConnectionPill() {
  if (!gIdleOpen || gPillDot == nullptr) return;
  int now = bleTransportConnected() ? 1 : 0;
  if (now == gShownConnected) return;
  gShownConnected = now;
  lv_obj_set_style_bg_color(gPillDot, now ? kLime : kMuted, 0);
  lv_label_set_text(gPillLabel, now ? "Phone connected" : "Waiting for your phone");
}

// Account 1 = 44'/501'/0'/0', the first account Phantom and Jupiter show.
const uint32_t kAccount1[] = {0x8000002C, 0x800001F5, 0x80000000, 0x80000000};

void showReceive() {
  leaveIdle();
  lv_obj_t* scr = newScreen();
  backHeader(scr, "Account 1 address", onBack);

  char addr[45] = "";
  if (hasSeed()) {
    uint8_t priv[32], pub[32];
    deriveKey(kAccount1, 4, priv);
    publicKeyFromPrivate(priv, pub);
    wipe(priv, sizeof priv);
    base58Encode(pub, addr);
  }
  if (addr[0] == '\0') {
    lv_obj_t* none = text(scr, "No key yet. Restart the device.", &inter_12, kMuted, 0, 0);
    lv_obj_align(none, LV_ALIGN_CENTER, 0, 0);
    loadAndDropOld(scr);
    return;
  }

  lv_obj_t* frame = box(scr, 0, 48, 140, 140, kText, 8);  // light frame = the QR's quiet zone
  lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_t* qr = lv_qrcode_create(frame);
  lv_qrcode_set_size(qr, 124);
  lv_qrcode_set_dark_color(qr, kGround);
  lv_qrcode_set_light_color(qr, kText);
  lv_qrcode_update(qr, addr, strlen(addr));
  lv_obj_center(qr);

  char grouped[64];
  groupAddress(addr, grouped, sizeof grouped);
  size_t len = strlen(addr);
  size_t lastStart = (len - 4) + (len - 4) / 4;
  char head[5], mid[64];
  snprintf(head, sizeof head, "%.4s", grouped);
  snprintf(mid, sizeof mid, "%.*s", (int)(lastStart - 4), grouped + 4);
  lv_obj_t* spans = lv_spangroup_create(scr);
  lv_obj_set_width(spans, 224);  // wide enough for 2 lines of groups
  lv_spangroup_set_mode(spans, LV_SPAN_MODE_BREAK);
  lv_spangroup_set_align(spans, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_line_space(spans, 3, 0);
  addSpan(spans, head, &inter_12_bold, kLime);
  addSpan(spans, mid, &inter_12, kText);
  addSpan(spans, grouped + lastStart, &inter_12_bold, kLime);
  lv_spangroup_refr_mode(spans);
  lv_obj_align(spans, LV_ALIGN_TOP_MID, 0, 198);

  lv_obj_t* hint = text(scr, "Check this against your wallet app. Trust this screen, not the phone.",
                        &inter_12, kMuted, 0, 0, 200);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 258);
  loadAndDropOld(scr);
}

void settingsRow(lv_obj_t* scr, int y, const char* label, const char* value) {
  lv_obj_t* row = box(scr, 8, y, 224, 44, kCard, 8);
  lv_obj_t* l = text(row, label, &inter_12_bold, kText, 0, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t* v = text(row, value, &inter_12, kMuted, 0, 0);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
}

void showSettings() {
  leaveIdle();
  lv_obj_t* scr = newScreen();
  backHeader(scr, "Settings", onBack);
  settingsRow(scr, 48, "Device name", gDeviceName);
  settingsRow(scr, 94, "Network", "Devnet");
  settingsRow(scr, 140, "Account", "Account 1");
  settingsRow(scr, 186, "Brightness", "100%");
  lv_obj_t* note = text(scr, "Read-only for now. Changing these comes with the Dashboard.",
                        &inter_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 244);
  loadAndDropOld(scr);
}

void onDoneEvent(lv_event_t*) { gDoneTapped = true; }  // handled in uiService, not inside the event

// One "label ........ value" line on the receipt card.
lv_obj_t* receiptRow(lv_obj_t* card, int y, const char* label, const char* value, lv_color_t color) {
  text(card, label, &inter_12, kMuted, 10, y);
  lv_obj_t* v = text(card, value, &inter_12, color, 0, y);
  lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -10, y);
  return v;
}

// Shorten a base58 string to "ABCD...WXYZ" so it fits one line.
void shortB58(const char* full, char* out, size_t outSize) {
  size_t len = strlen(full);
  snprintf(out, outSize, "%.4s...%s", full, full + (len > 4 ? len - 4 : 0));
}

void showResult(Decision d, bool timedOut) {
  leaveIdle();
  gAwaitingSig = false;
  lv_obj_t* old = lv_screen_active();
  lv_obj_t* scr = newScreen();
  gSigLabel = nullptr;
  bool ok = d == kApproved;
  const char* title = ok ? "Signed" : timedOut ? "Timed out" : "Rejected";
  const char* body = ok        ? "Your wallet is sending it now. Watch it confirm in Phantom."
                     : timedOut ? "No answer in 30 s, so nothing was signed."
                                : "Nothing was signed. Your wallet shows it as cancelled.";
  lv_color_t color = ok ? kLime : timedOut ? kMuted : kOrange;

  lv_obj_t* dot = box(scr, 92, 24, 56, 56, color, 28);
  lv_obj_center(text(dot, ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, &lv_font_montserrat_28, kGround, 0, 0));
  lv_obj_t* t = text(scr, title, &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 88);
  lv_obj_t* b = text(scr, body, &inter_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 112);

  if (ok && gLastTx.valid) {  // the receipt: what you just signed
    char buf[48], val[48], addr[45];
    lv_obj_t* card = box(scr, 16, 150, 208, 100, kCard, 8);
    if (gLastTx.kind == TxSummary::kSolTransfer) {
      formatSol(gLastTx.lamports, buf);
      snprintf(val, sizeof val, "%s SOL", buf);
      receiptRow(card, 8, "Sent", val, kText);
      base58Encode(gLastTx.to, addr);
      shortB58(addr, val, sizeof val);
      receiptRow(card, 30, "To", val, kLime);
    } else {
      receiptRow(card, 8, "Signed", "unreadable transaction", kAmber);
      snprintf(val, sizeof val, "%u", (unsigned)gLastTx.programCount);
      receiptRow(card, 30, "Programs", val, kText);
    }
    formatSol(gLastTx.feeLamports, buf);
    snprintf(val, sizeof val, "%s SOL", buf);
    receiptRow(card, 52, "Fee", val, kText);
    gSigLabel = receiptRow(card, 74, "Signature", "...", kText);  // filled in by uiShowSignature
  }

  lv_obj_t* done = lv_button_create(scr);
  lv_obj_set_pos(done, 16, 260);
  lv_obj_set_size(done, 208, 48);
  lv_obj_set_style_bg_color(done, kCard, 0);
  lv_obj_set_style_radius(done, 24, 0);
  lv_obj_set_style_shadow_width(done, 0, 0);
  lv_obj_center(text(done, "Done", &inter_16_semibold, kText, 0, 0));
  lv_obj_add_event_cb(done, onDoneEvent, LV_EVENT_CLICKED, nullptr);

  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
}

void lvglLog(lv_log_level_t, const char* msg) { Serial.print(msg); }

}  // namespace

void uiBegin(const char* deviceName) {
  gDeviceName = deviceName;
  Serial.println("ui: lv_init");
  lv_init();
  lv_log_register_print_cb(lvglLog);  // LVGL warnings and errors go to the serial monitor
  lv_tick_set_cb(tick);
  Serial.println("ui: screen");
  gTft.init();  // also switches the backlight on (TFT_BL in platformio.ini)
  gTft.setRotation(0);  // portrait 240x320
  gTft.fillScreen(TFT_BLACK);
  lv_display_t* disp = lv_display_create(kScreenW, kScreenH);
  lv_display_set_flush_cb(disp, flush);
  lv_display_set_buffers(disp, gDrawBuf, nullptr, sizeof gDrawBuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  Serial.println("ui: touch");
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readTouch);

  pinMode(kBootButton, INPUT_PULLUP);
  showIdle(gDeviceName);
  Serial.println("ui: ready");
}

void uiService() {
  if (gNav != kNavNone) {  // a button asked for another screen
    Nav go = gNav;
    gNav = kNavNone;
    if (go == kNavIdle) showIdle(gDeviceName);
    if (go == kNavReceive) showReceive();
    if (go == kNavSettings) showSettings();
  }
  updateConnectionPill();
  if (gAwaitingSig && millis() - gApprovedAt > 3000) {  // no signature came (no key?): still close
    showResult(kApproved, false);
    gResultOpen = true;
    gResultShownAt = millis();
  }
  if (gResultOpen) {  // receipt up: Done, BOOT or a minute takes you back to the idle screen
    // BOOT is ignored for half a second, in case it is still held from rejecting.
    bool boot = digitalRead(kBootButton) == LOW && millis() - gResultShownAt > 500;
    if (gDoneTapped || boot || millis() - gResultShownAt >= kReceiptMs) {
      gDoneTapped = false;
      showIdle(gDeviceName);
      if (boot) delay(300);  // don't let the same press count twice
    }
  }
  lv_timer_handler();
}

void uiShowSignature(const uint8_t signature[64]) {
  char full[90], shortSig[24];
  base58EncodeBytes(signature, 64, full, sizeof full);
  Serial.printf("signature (transaction id): %s\n", full);  // public: it goes on-chain anyway
  if (gAwaitingSig) {  // "Signing..." was up: now the receipt
    showResult(kApproved, false);
    gResultOpen = true;
    gDoneTapped = false;
    gResultShownAt = millis();
  }
  if (gSigLabel == nullptr) return;
  shortB58(full, shortSig, sizeof shortSig);
  lv_label_set_text(gSigLabel, shortSig);
  lv_timer_handler();
}

bool uiApprove(const uint8_t* message, size_t len) {
  logHex(">>> SIGN REQUEST, message", message, len);

  TxSummary tx;
  decodeTransaction(message, len, &tx);  // tx.valid says whether it made sense
  gLastTx = tx;
  leaveIdle();
  gAwaitingSig = false;
  gEarlyAt = 0;
  gSigLabel = nullptr;  // a new request replaces any receipt still on screen
  gResultOpen = false;
  gDecision = kPending;
  gHolding = false;
  lv_obj_t* old = lv_screen_active();
  showSignScreen(tx);
  if (old != nullptr) lv_obj_delete(old);

  uint32_t start = millis();
  bool timedOut = false;
  while (gDecision == kPending) {
    uint32_t now = millis();
    if (now - start >= kTimeoutMs) {
      timedOut = true;
      gDecision = kRejected;
      break;
    }

    if (gHolding) {
      uint32_t elapsed = now - gHoldStart;  // how long the finger has been down, in ms
      int percent = 0;

      // TODO(human): turn `elapsed` into a percentage of gHoldMs (0 to 100).
      //
         percent = elapsed * 100 / gHoldMs;     // e.g. 750 ms of 1500 ms -> 50
         if (percent >= 100) {                  // held long enough?
           percent = 100;                       // never show more than a full ring
           gDecision = kApproved;
         }
      //
      // Until this is written, percent stays 0: the ring never fills and nothing can be
      // approved. That's the safe way to be unfinished.

      lv_arc_set_value(gArc, percent);
      lv_obj_set_width(gTopBar, lv_pct(percent));
      lv_arc_set_value(gBigArc, percent);  // the big ring on the overlay

    }

    if (gEarlyAt != 0 && now - gEarlyAt > 1500) {  // "let go too early" shown long enough
      gEarlyAt = 0;
      show(gOverlay, false);
    }

    if (digitalRead(kBootButton) == LOW) {  // BOOT means "no" (later: also lock + Dashboard)
      Serial.println("rejected with BOOT button");
      gDecision = kRejected;
    }

    static uint32_t lastSecond = 0;  // update the countdown once a second
    if (now - lastSecond >= 1000) {
      lastSecond = now;
      uint32_t left = (kTimeoutMs - (now - start) + 999) / 1000;
      char buf[8];
      snprintf(buf, sizeof buf, "0:%02lu", (unsigned long)left);
      lv_label_set_text(gCountdown, buf);
    }

    lv_timer_handler();
    delay(5);
  }

  bool approved = gDecision == kApproved;
  Serial.println(approved ? "approved" : timedOut ? "timed out" : "rejected");
  if (approved) {  // "Signing..." until the signature exists; uiShowSignature() opens the receipt
    overlaySigning();
    gAwaitingSig = true;
    gApprovedAt = millis();
    lv_timer_handler();
    return true;
  }
  showResult(gDecision, timedOut);  // stays up as a receipt; uiService() takes it down
  gResultOpen = true;
  gDoneTapped = false;
  gResultShownAt = millis();
  lv_timer_handler();
  return approved;
}

#endif  // BLEND_CYD
