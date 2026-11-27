// Signer screens on the CYD (PIN, recovery words, sign, receipt, settings). The screen
// hardware and the shared builders are in ui_kit.cpp.
// Only compiled for the `cyd` blend (see platformio.ini).
#ifdef BLEND_CYD

#include "ui_cyd.h"

#include <Arduino.h>
#include <esp_random.h>  // esp_random(): shuffles the PIN keys
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "backlight.h"
#include "bip39.h"
#include "blind_sign.h"
#include "boot_anim.h"
#include "ble_transport.h"
#include "dash_store.h"
#include "keys.h"
#include "log.h"
#include "mode.h"
#include "pin_vault.h"
#include "seed_store.h"
#include "tx_decode.h"
#include "ui_kit.h"


namespace {

// ---- Approval state ----------------------------------------------------------------------

constexpr uint32_t kTimeoutMs = 30000;
constexpr uint32_t kHoldKnownMs = 1500;    // plain SOL transfer
constexpr uint32_t kHoldUnknownMs = 3000;  // anything we can't read: more effort on purpose

enum Decision { kPending, kApproved, kRejected };
Decision gDecision = kPending;
bool gHolding = false;
uint32_t gHoldStart = 0;
uint32_t gHoldMs = kHoldKnownMs;

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
bool gHasVault = false;    // a PIN vault exists (set at start and after setup)

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
  lv_obj_center(text(gCheck, LV_SYMBOL_OK, &inter_28_bold, kGround, 0, 0));

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
      lv_obj_set_width(gTopBar, 0);
    }
    gHolding = false;
  }
}

void onRejectEvent(lv_event_t*) { gDecision = kRejected; }

// A red "(!) text" line. The icon comes from the Montserrat font (Inter has no symbols).
// Long text wraps; returns the y just below it.
int redFlagLine(lv_obj_t* scr, int y, const char* s) {
  text(scr, LV_SYMBOL_WARNING, &inter_12, kRed, 16, y + 1);
  lv_obj_t* l = text(scr, s, &inter_12, kRed, 34, y, 190);
  lv_obj_update_layout(l);
  int h = lv_obj_get_height(l);
  return y + (h > 0 && h < 60 ? h : 16) + 4;
}

void showSignScreen(const TxSummary& tx) {
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  gCountdown = text(scr, "0:30", &inter_12, kMuted, 196, 6);

  char buf[96];
  bool known = !needsBlindSigning(tx);
  // Can't read it and the "Unknown transactions" switch is off: show it, but only Reject.
  bool blocked = !known && !blindSigningAllowed();

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
    // Red instead of amber when we spotted a red flag (see TxRisk in tx_decode.h).
    lv_color_t tone = tx.risks ? kRed : kAmber;
    lv_obj_t* warn = box(scr, 16, 30, 208, 0, tx.risks ? lv_color_hex(0x2B1616) : lv_color_hex(0x2B2414), 8);
    lv_obj_set_height(warn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(warn, 8, 0);
    lv_obj_set_style_border_color(warn, tone, 0);
    lv_obj_set_style_border_width(warn, 1, 0);
    text(warn, LV_SYMBOL_WARNING, &inter_16_semibold, tone, 8, 8);
    lv_obj_t* msg = lv_spangroup_create(warn);
    lv_obj_set_pos(msg, 32, 8);
    lv_obj_set_width(msg, 166);
    lv_spangroup_set_mode(msg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_line_space(msg, 2, 0);
    if (!tx.valid) {
      addSpan(msg, "Not a Solana transaction. ", &inter_12_bold, tone);
      addSpan(msg, "Reject it.", &inter_12, kText);
    } else if (blocked) {
      addSpan(msg, "Blocked: can't read this. ", &inter_12_bold, tone);
      addSpan(msg, "Allow Unknown transactions in Settings to sign.", &inter_12, kText);
    } else if (tx.risks) {
      addSpan(msg, "Red flags. ", &inter_12_bold, tone);
      addSpan(msg, "Reject unless you know exactly why.", &inter_12, kText);
    } else {
      addSpan(msg, "Can't read this one. ", &inter_12_bold, tone);
      addSpan(msg, "Only sign if you just started it in your wallet.", &inter_12, kText);
    }
    lv_spangroup_refr_mode(msg);
    lv_obj_update_layout(warn);
    int y = lv_obj_get_y(warn) + lv_obj_get_height(warn) + 8;
    if (y < 80 || y > 140) y = 100;  // layout not measured for some reason: safe spot

    // One red line per red flag, before the program list: they matter more.
    for (uint8_t bit = 1; bit != 0 && bit <= kRiskHighFee; bit <<= 1) {
      if (!(tx.risks & bit)) continue;
      if (bit == kRiskHighFee) {
        char fee[24];
        formatSol(tx.feeLamports, fee);
        snprintf(buf, sizeof buf, "Fee %s SOL: very high", fee);
      } else {
        snprintf(buf, sizeof buf, "%s", riskText((TxRisk)bit));
      }
      y = redFlagLine(scr, y, buf);
    }
    if (tx.risks) y += 4;

    snprintf(buf, sizeof buf, "It talks to %u program%s", (unsigned)tx.programCount,
             tx.programCount == 1 ? "" : "s");
    text(scr, buf, &inter_12, kMuted, 16, y);
    y += 18;
    // As many program rows as fit above the hint line (fewer when there are red flags).
    for (size_t i = 0; i < tx.programCount && i < 3 && y + 30 <= 226; i++) {
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
    if (tx.risks & kRiskHighFee) {
      snprintf(buf, sizeof buf, "Network fee %s SOL: very high", fee);
      feeY = redFlagLine(scr, feeY, buf) - 22;  // the hint goes 22 px under the last line
    } else {
      snprintf(buf, sizeof buf, "Network fee  %s SOL", fee);
      text(scr, buf, &inter_12, kMuted, 16, feeY);
    }
  }

  gHint = text(scr, "", &inter_12, kMuted, 16, known ? feeY + 22 : feeY, 208);

  // Reject: outlined pill, bottom left.
  // Blocked: no hold button, so Reject takes the whole row.
  lv_obj_t* reject = lv_button_create(scr);
  lv_obj_set_pos(reject, 12, 256);
  lv_obj_set_size(reject, blocked ? 216 : 60, 52);
  lv_obj_set_style_bg_opa(reject, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(reject, lv_color_hex(0x3A4250), 0);
  lv_obj_set_style_border_width(reject, 2, 0);
  lv_obj_set_style_radius(reject, 26, 0);
  lv_obj_set_style_shadow_width(reject, 0, 0);
  lv_obj_center(text(reject, "Reject", &inter_12, kText, 0, 0));
  lv_obj_add_event_cb(reject, onRejectEvent, LV_EVENT_CLICKED, nullptr);

  // Hold to sign: big pill. Progress shows on the full-screen overlay and the top bar.
  // Not there at all when blocked: with no button to hold, nothing can be approved.
  if (!blocked) {
    lv_obj_t* hold = lv_button_create(scr);
    lv_obj_set_pos(hold, 80, 256);
    lv_obj_set_size(hold, 148, 52);
    lv_obj_set_style_pad_all(hold, 0, 0);  // LVGL buttons have inner padding by default; it pushed the text off the edge
    lv_obj_set_style_bg_color(hold, known ? kLime : tx.risks ? kRed : kAmber, 0);
    lv_obj_set_style_radius(hold, 26, 0);
    lv_obj_set_style_shadow_width(hold, 0, 0);
    lv_obj_add_event_cb(hold, onHoldEvent, LV_EVENT_ALL, nullptr);

    lv_obj_t* label = text(hold, known ? "Hold to sign" : "Hold 3 s", &inter_16_semibold, kGround, 0, 0);
    lv_obj_center(label);  // the progress shows on the full-screen overlay, so the pill is just text
  }

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


void showIdle(const char* deviceName) {
  gSigLabel = nullptr;  // that label is about to be deleted with its screen
  gResultOpen = false;
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  lv_obj_t* bt = text(scr, LV_SYMBOL_BLUETOOTH, &inter_12, kMuted, 0, 0);
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
  lv_obj_t* mid = text(scr, LV_SYMBOL_BLUETOOTH, &inter_16_semibold, kLime, 0, 0);
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

  addressBlock(scr, addr);
  loadAndDropOld(scr);
}

void onBlindSwitch(lv_event_t* e) {
  lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
  setBlindSigningAllowed(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

// "Unknown transactions" on/off (see blind_sign.h). Amber when on: it's the risky setting.
void blindSigningRow(lv_obj_t* scr, int y) {
  lv_obj_t* row = box(scr, 8, y, 224, 44, kCard, 8);
  lv_obj_t* l = text(row, "Unknown transactions", &inter_12_bold, kText, 0, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_set_size(sw, 44, 24);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_ext_click_area(sw, 10);
  lv_obj_set_style_bg_color(sw, kTrack, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, kAmber, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (blindSigningAllowed()) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, onBlindSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
}

void showSettings() {
  leaveIdle();
  lv_obj_t* scr = newScreen();
  backHeader(scr, "Settings", onBack);
  settingsRow(scr, 48, "Device name", gDeviceName);
  settingsRow(scr, 94, "Network", "Devnet");
  settingsRow(scr, 140, "Backup", hasRecoveryWords() ? "24 words" : "None yet");
  blindSigningRow(scr, 186);
  brightnessRow(scr, 236);
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
  lv_obj_center(text(dot, ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, &inter_28_bold, kGround, 0, 0));
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

// ---- PIN screen (design "PIN") ------------------------------------------------------------
//
// One screen, three jobs:
//   kPinEnter   unlock the vault at boot
//   kPinChoose  first boot, or an old plain seed: pick a PIN...
//   kPinRepeat  ...and type it again, so a typo can't lock you out
//
// The digits are shuffled every time the screen opens ("keys move every time"), so finger
// smudges and someone watching your hand don't give the PIN away.

enum PinMode { kPinOff, kPinEnter, kPinChoose, kPinRepeat };
enum PinKey { kKeyBack = 10, kKeyOk = 11 };

PinMode gPinMode = kPinOff;
char gPin[kPinMax + 1];        // digits typed so far
size_t gPinLen = 0;
char gFirstPin[kPinMax + 1];   // the PIN from kPinChoose, to compare in kPinRepeat
bool gPinSubmit = false;       // OK tapped; handled in uiService (Argon2 takes ~1 s)
lv_obj_t* gPinDots[kPinMax];
lv_obj_t* gPinHint = nullptr;
uint8_t gNewSeed[64];          // seed waiting for its PIN (new words, restored words, old key)
bool gNewSeedHasWords = false; // gNewSeed came from 24 recovery words
uint8_t gOldSeed[64];          // a key without words, kept while "No backup yet" is up

void forgetTypedPin() {
  wipe(gPin, sizeof gPin);
  gPinLen = 0;
}

void updatePinDots() {
  for (size_t i = 0; i < kPinMax; i++) show(gPinDots[i], i < gPinLen);
}

void setPinHint(const char* s, lv_color_t color) {
  if (gPinHint == nullptr) return;
  lv_label_set_text(gPinHint, s);
  lv_obj_set_style_text_color(gPinHint, color, 0);
}

// Show a message right now, before something slow (Argon2) blocks the loop.
void pinBusy(const char* s) {
  setPinHint(s, kText);
  lv_refr_now(nullptr);
}

void onPinKey(lv_event_t* e) {
  int key = (int)(intptr_t)lv_event_get_user_data(e);
  if (key == kKeyOk) {
    gPinSubmit = true;
  } else if (key == kKeyBack) {
    if (gPinLen > 0) gPin[--gPinLen] = '\0';
  } else if (gPinLen < kPinMax) {
    gPin[gPinLen++] = (char)('0' + key);
    gPin[gPinLen] = '\0';
  }
  updatePinDots();
}

lv_obj_t* pinKey(lv_obj_t* scr, int x, int y, const char* label, const lv_font_t* font,
                 lv_color_t bg, lv_color_t fg, int key) {
  lv_obj_t* b = lv_button_create(scr);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, 70, 46);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_center(text(b, label, font, fg, 0, 0));
  lv_obj_add_event_cb(b, onPinKey, LV_EVENT_CLICKED, (void*)(intptr_t)key);
  return b;
}

// The normal hint under the dots, for each mode.
void defaultPinHint(char* buf, size_t cap) {
  if (gPinMode == kPinRepeat) {
    snprintf(buf, cap, "Type the same PIN again");
  } else if (gPinMode == kPinChoose) {
    snprintf(buf, cap, "4 to 8 digits \xC2\xB7 keys move each time");
  } else {
    int left = triesLeft();
    if (left <= 1) snprintf(buf, cap, "Last try: a wrong PIN erases Sigly");
    else snprintf(buf, cap, "%d tries, then Sigly erases itself", left);
  }
}

void showPin(PinMode mode, const char* hint = nullptr, lv_color_t hintColor = kMuted) {
  leaveIdle();
  gSigLabel = nullptr;
  gResultOpen = false;
  gPinMode = mode;
  gPinSubmit = false;
  forgetTypedPin();

  // Drop the old screen BEFORE building this one: the keypad is 12 buttons, and two
  // keypads at once would be a big bite of LVGL's 40 KB.
  lv_obj_t* old = lv_screen_active();
  lv_obj_t* scr = newScreen();
  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
  devnetBadge(scr);
  const char* title = mode == kPinEnter ? "Enter PIN" : mode == kPinChoose ? "Choose a PIN"
                                                                           : "Repeat PIN";
  lv_obj_t* t = text(scr, title, &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 28);

  // Dots: one per digit typed. A flex row skips hidden children, so it stays centred.
  lv_obj_t* row = box(scr, 0, 0, 200, 14, kGround, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  for (size_t i = 0; i < kPinMax; i++) gPinDots[i] = box(row, 0, 0, 12, 12, kLime, 6);
  updatePinDots();

  char buf[80];
  if (hint == nullptr) {
    defaultPinHint(buf, sizeof buf);
    hint = buf;
    if (mode == kPinEnter && triesLeft() <= 1) hintColor = kOrange;
  }
  gPinHint = text(scr, hint, &inter_12, hintColor, 0, 0, 216);
  lv_obj_set_style_text_align(gPinHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(gPinHint, LV_ALIGN_TOP_MID, 0, 82);

  // Shuffle 0-9 (Fisher-Yates). esp_random() is truly random because Bluetooth is on.
  int digits[10];
  for (int i = 0; i < 10; i++) digits[i] = i;
  for (int i = 9; i > 0; i--) {
    int j = esp_random() % (i + 1);
    int tmp = digits[i];
    digits[i] = digits[j];
    digits[j] = tmp;
  }

  // 3 x 4 keys: nine digits, then  [back] [last digit] [OK].
  const int xs[] = {12, 85, 158};
  const int ys[] = {112, 164, 216, 268};
  for (int i = 0; i < 9; i++) {
    char d[2] = {(char)('0' + digits[i]), '\0'};
    pinKey(scr, xs[i % 3], ys[i / 3], d, &inter_28_bold, kCard, kText, digits[i]);
  }
  pinKey(scr, xs[0], ys[3], LV_SYMBOL_BACKSPACE, &inter_16_semibold, kGround, kMuted, kKeyBack);
  char last[2] = {(char)('0' + digits[9]), '\0'};
  pinKey(scr, xs[1], ys[3], last, &inter_28_bold, kCard, kText, digits[9]);
  pinKey(scr, xs[2], ys[3], "OK", &inter_16_semibold, kLime, kGround, kKeyOk);
}

// The Dashboard shows Account 1 but never unlocks the seed, so the Signer leaves it the
// public address (public: it's on-chain anyway) each time you unlock.
void saveAddressForDashboard() {
  uint8_t priv[32], pub[32];
  char addr[45];
  deriveKey(kAccount1, 4, priv);
  publicKeyFromPrivate(priv, pub);
  wipe(priv, sizeof priv);
  base58Encode(pub, addr);
  dashSetAddress(addr);
}

void finishUnlock() {
  gPinMode = kPinOff;
  gPinHint = nullptr;
  forgetTypedPin();
  wipe(gFirstPin, sizeof gFirstPin);
  wipe(gNewSeed, sizeof gNewSeed);
  wipe(gOldSeed, sizeof gOldSeed);
  gNewSeedHasWords = false;
  gHasVault = true;
  saveAddressForDashboard();
  showIdle(gDeviceName);
}

enum SetupFrom { kFromNothing, kFromWiped, kFromVault, kFromPlain };
void showWelcome(SetupFrom from);  // further down, in "Recovery words"

void handlePinSubmit() {
  gPinSubmit = false;
  gPin[gPinLen] = '\0';
  if (!pinAllowed(gPin)) {
    setPinHint("Use 4 to 8 digits", kOrange);
    return;
  }

  if (gPinMode == kPinChoose) {
    memcpy(gFirstPin, gPin, sizeof gFirstPin);
    showPin(kPinRepeat);
    return;
  }

  if (gPinMode == kPinRepeat) {
    if (strcmp(gPin, gFirstPin) != 0) {
      wipe(gFirstPin, sizeof gFirstPin);
      showPin(kPinChoose, "They didn't match. Try again.", kOrange);
      return;
    }
    pinBusy("Locking your key...");
    if (saveVault(gNewSeed, gPin)) {
      // Remember whether this seed has words on paper. Settings shows it, and a seed
      // without words gets the "No backup yet" screen after every unlock.
      setHasRecoveryWords(gNewSeedHasWords);
      setSeed(gNewSeed);
      finishUnlock();
    } else {
      wipe(gFirstPin, sizeof gFirstPin);
      showPin(kPinChoose, "Couldn't save it. Try again.", kOrange);
    }
    return;
  }

  // kPinEnter
  pinBusy("Checking...");
  uint8_t seed[64];
  UnlockResult r = unlockVault(gPin, seed);
  if (r == UnlockResult::kOk) {
    if (hasRecoveryWords()) {
      setSeed(seed);
      wipe(seed, sizeof seed);
      finishUnlock();
    } else {
      // A seed from older firmware: random bytes, no words exist for it. Keep it in
      // gOldSeed (not in keys.cpp, so nothing can sign) while Sigly offers a backup.
      memcpy(gOldSeed, seed, 64);
      wipe(seed, sizeof seed);
      forgetTypedPin();
      gPinMode = kPinOff;
      gPinHint = nullptr;
      showWelcome(kFromVault);
    }
    return;
  }
  wipe(seed, sizeof seed);
  if (r == UnlockResult::kWrongPin) {
    char buf[80];
    if (triesLeft() <= 1) snprintf(buf, sizeof buf, "Wrong PIN \xC2\xB7 last try before erase");
    else snprintf(buf, sizeof buf, "Wrong PIN \xC2\xB7 %d tries left", triesLeft());
    showPin(kPinEnter, buf, kOrange);  // fresh shuffle too
  } else if (r == UnlockResult::kWiped) {
    forgetTypedPin();
    gPinMode = kPinOff;
    gPinHint = nullptr;
    showWelcome(kFromWiped);
  } else if (r == UnlockResult::kNoMemory) {
    showPin(kPinEnter, "Low memory. Restart Sigly.", kOrange);
  } else {  // kNoVault
    showPin(kPinEnter, "No saved key found.", kOrange);
  }
}

// ---- Recovery words (setup, backup check, restore) ---------------------------------------
//
// First start, after a wipe, or an old wallet without words:
//
//   Welcome --Create--> 24 words (2 pages) --> check 3 of them --> choose PIN --> idle
//           --Restore-> type 24 words --> checksum --> "is this your address?" --> PIN
//
// Nothing is written to flash until the PIN is chosen (saveVault). Pull the plug halfway
// and the board is exactly as it was before. While these screens are up, keys.cpp holds
// no seed, so nothing can be signed.
//
// Buttons only set gSetupAction; uiService() acts on it. Building a new screen deletes the
// old one, and deleting the button whose click is still being handled would crash LVGL.

enum SetupAction {
  kActNone,
  kActCreate,     // Welcome: new wallet
  kActRestore,    // Welcome: restore from words
  kActLater,      // Welcome: keep the old key without words
  kActWordsNext,  // words page 1 -> page 2
  kActWordsBack,  // words page 2 -> page 1
  kActWordsDone,  // "I wrote them down" -> check
  kActChoice,     // a word was tapped on the check screen (gChoiceTapped)
  kActSeeWords,   // check screen: show the words again
  kActKey,        // restore keyboard: a letter or backspace (gKeyTapped)
  kActSuggest,    // restore: a suggested word was tapped (gSuggestTapped)
  kActRestoreBack,
  kActFix,        // review screen: retype word gFixIndex
  kActAddrYes,    // "is this your address?" yes
  kActAddrNo,
};

SetupAction gSetupAction = kActNone;
bool gSetupOpen = false;     // one of these screens is up (auto-lock leaves it alone)
SetupFrom gSetupFrom = kFromNothing;
uint16_t gWords[kWordCount];  // word numbers (0..2047): shown, checked, or typed in
int gWordsPage = 0;
bool gReviewMode = false;     // the words grid is the after-restore review (words tappable)

void onSetupButton(lv_event_t* e) {
  gSetupAction = (SetupAction)(intptr_t)lv_event_get_user_data(e);
}

// A big rounded button. Lime = the main action, card colour = the other one.
lv_obj_t* setupButton(lv_obj_t* scr, int x, int y, int w, int h, const char* label, bool main,
                      SetupAction act) {
  lv_obj_t* b = lv_button_create(scr);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_bg_color(b, main ? kLime : kCard, 0);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_center(text(b, label, &inter_16_semibold, main ? kGround : kText, 0, 0));
  lv_obj_add_event_cb(b, onSetupButton, LV_EVENT_CLICKED, (void*)(intptr_t)act);
  return b;
}

lv_obj_t* centeredText(lv_obj_t* scr, const char* s, const lv_font_t* font, lv_color_t c, int y,
                       int width = 208) {
  lv_obj_t* t = text(scr, s, font, c, 0, 0, width);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, y);
  return t;
}

lv_obj_t* setupScreen() {
  leaveIdle();
  gSigLabel = nullptr;
  gResultOpen = false;
  gSetupOpen = true;
  // Show the new (still empty) screen and delete the old one FIRST, then build: two full
  // screens at once (24 words + keyboard...) would be too big a bite of LVGL's 40 KB.
  lv_obj_t* old = lv_screen_active();
  lv_obj_t* scr = newScreen();
  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
  devnetBadge(scr);
  return scr;
}

// Wipes everything the setup screens kept in RAM.
void forgetSetup() {
  wipe(gWords, sizeof gWords);
  gSetupOpen = false;
}

// ---- Welcome ----

void showWelcome(SetupFrom from) {
  gSetupFrom = from;
  clearSeed();  // nothing can sign while setting up
  lv_obj_t* scr = setupScreen();
  bool old = from == kFromVault || from == kFromPlain;

  // A small "words" icon: three lines on a card, like a page of a notebook.
  lv_obj_t* icon = box(scr, 0, 0, 56, 56, kCard, 14);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 34);
  for (int i = 0; i < 3; i++) {
    lv_obj_t* line = box(icon, 14, 16 + i * 10, i == 2 ? 18 : 28, 4, old ? kAmber : kLime, 2);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  }

  if (old) {
    centeredText(scr, "No backup yet", &inter_16_semibold, kText, 102);
    centeredText(scr,
                 "This wallet has no recovery words. If the board breaks, its funds are gone. "
                 "A new wallet with 24 words gets a new address.",
                 &inter_12, kMuted, 128);
    setupButton(scr, 16, 196, 208, 44, "New wallet with words", true, kActCreate);
    setupButton(scr, 16, 246, 100, 40, "Restore", false, kActRestore);
    setupButton(scr, 124, 246, 100, 40, "Later", false, kActLater);
  } else {
    centeredText(scr, from == kFromWiped ? "Sigly was erased" : "Set up Sigly",
                 &inter_16_semibold, kText, 102);
    centeredText(scr,
                 from == kFromWiped
                     ? "3 wrong PINs, so the key was deleted. Your 24 words bring it back."
                     : "Make a new wallet, or bring one back with its 24 recovery words.",
                 &inter_12, from == kFromWiped ? kOrange : kMuted, 128);
    setupButton(scr, 16, 196, 208, 48, "Create new wallet", true, kActCreate);
    setupButton(scr, 16, 252, 208, 48, "Restore from words", false, kActRestore);
  }
  loadAndDropOld(scr);
}

// ---- Show the 24 words ----

// 12 words in two columns of 6: "1 abandon   7 ability". If `tappable`, each word is a
// button that asks to retype it (the review screen after a failed checksum).
void wordGrid(lv_obj_t* scr, int firstIndex, int y0, bool tappable);

void showWords(int page) {
  gWordsPage = page;
  gReviewMode = false;
  lv_obj_t* scr = setupScreen();
  char title[40];
  snprintf(title, sizeof title, "Words %d to %d", page * 12 + 1, page * 12 + 12);
  lv_obj_t* t = text(scr, title, &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
  centeredText(scr, page == 0 ? "Write them on paper, in order. Never a photo."
                              : "Check each one. Nobody else should see them.",
               &inter_12, kMuted, 50, 216);
  wordGrid(scr, page * 12, 80, false);
  if (page == 0) {
    setupButton(scr, 16, 268, 208, 44, "Next", true, kActWordsNext);
  } else {
    setupButton(scr, 16, 268, 72, 44, "Back", false, kActWordsBack);
    setupButton(scr, 96, 268, 128, 44, "I wrote them", true, kActWordsDone);
  }
  loadAndDropOld(scr);
}

int gFixIndex = -1;  // review screen: which word was tapped

void onFixWord(lv_event_t* e) {
  gFixIndex = (int)(intptr_t)lv_event_get_user_data(e);
  gSetupAction = kActFix;
}

void wordGrid(lv_obj_t* scr, int firstIndex, int y0, bool tappable) {
  for (int i = 0; i < 12; i++) {
    int index = firstIndex + i;
    int col = i / 6, row = i % 6;
    int x = 12 + col * 112, y = y0 + row * 30;
    // Only the review screen needs a box around each word (something to tap). Without it
    // the labels sit straight on the screen: 12 fewer objects, less of LVGL's memory.
    lv_obj_t* cell = scr;
    if (tappable) {
      cell = box(scr, x, y, 104, 26, kCard, 6);
      x = y = 0;
    }
    char num[8];
    snprintf(num, sizeof num, "%d", index + 1);
    lv_obj_t* n = text(cell, num, &inter_12, kMuted, 0, 0);
    lv_obj_set_width(n, 20);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(n, x, y + 6);
    text(cell, bip39Word(gWords[index]), &inter_16_semibold, kText, x + 26, y + 3);
    if (tappable) {
      lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(cell, onFixWord, LV_EVENT_CLICKED, (void*)(intptr_t)index);
    }
  }
}

// ---- Check the backup: "Which is word #7?" x3 ----
//
// Typing 3 words on this small screen would be slow, so you pick the right one out of 4.
// The 3 wrong choices are random words from the list. Positions are random too, so you
// can't pass by remembering only the first few.

constexpr int kChecks = 3;
int gCheckPos[kChecks];   // which word positions get asked (0..23)
int gCheckRound = 0;      // 0..2
uint16_t gChoices[4];     // the 4 words on the buttons
int gChoiceTapped = -1;

int randomBelow(int n) { return (int)(esp_random() % (uint32_t)n); }

void pickCheckPositions() {
  for (int i = 0; i < kChecks; i++) {
    bool again;
    do {
      gCheckPos[i] = randomBelow(kWordCount);
      again = false;
      for (int j = 0; j < i; j++) again |= gCheckPos[j] == gCheckPos[i];
    } while (again);
  }
  // Ask them in order (#4, #11, #20), the way you read your paper.
  for (int i = 0; i < kChecks; i++)
    for (int j = i + 1; j < kChecks; j++)
      if (gCheckPos[j] < gCheckPos[i]) {
        int t = gCheckPos[i];
        gCheckPos[i] = gCheckPos[j];
        gCheckPos[j] = t;
      }
}

void onChoice(lv_event_t* e) {
  gChoiceTapped = (int)(intptr_t)lv_event_get_user_data(e);
  gSetupAction = kActChoice;
}

void showCheck(const char* hint = nullptr) {
  int pos = gCheckPos[gCheckRound];
  uint16_t right = gWords[pos];

  // The right word goes to a random button; the others get random different words.
  int rightSlot = randomBelow(4);
  for (int i = 0; i < 4; i++) {
    if (i == rightSlot) {
      gChoices[i] = right;
      continue;
    }
    bool clash;
    do {
      gChoices[i] = (uint16_t)randomBelow(2048);
      clash = gChoices[i] == right;
      for (int j = 0; j < i; j++) clash |= gChoices[j] == gChoices[i];
    } while (clash);
  }

  lv_obj_t* scr = setupScreen();
  char buf[48];
  snprintf(buf, sizeof buf, "Check %d of %d", gCheckRound + 1, kChecks);
  lv_obj_t* step = text(scr, buf, &inter_12, kMuted, 0, 0);
  lv_obj_align(step, LV_ALIGN_TOP_RIGHT, -10, 6);

  snprintf(buf, sizeof buf, "Which is word #%d?", pos + 1);
  lv_obj_t* t = text(scr, buf, &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
  centeredText(scr, hint != nullptr ? hint : "Look at your paper, not your memory.", &inter_12,
               hint != nullptr ? kOrange : kMuted, 68, 216);

  for (int i = 0; i < 4; i++) {
    lv_obj_t* b = lv_button_create(scr);
    lv_obj_set_pos(b, 12 + (i % 2) * 112, 108 + (i / 2) * 64);
    lv_obj_set_size(b, 104, 56);
    lv_obj_set_style_bg_color(b, kCard, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_center(text(b, bip39Word(gChoices[i]), &inter_16_semibold, kText, 0, 0));
    lv_obj_add_event_cb(b, onChoice, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  setupButton(scr, 16, 260, 208, 44, "See the words again", false, kActSeeWords);
  loadAndDropOld(scr);
}

// Shows `s` at once, before something slow (PBKDF2 or Argon2) blocks the loop.
void setupBusy(const char* s) {
  lv_obj_t* scr = setupScreen();
  lv_obj_t* t = text(scr, s, &inter_16_semibold, kText, 0, 0);
  lv_obj_center(t);
  loadAndDropOld(scr);
  lv_refr_now(nullptr);
}

// Words checked (or restored): turn them into the seed, then on to the PIN.
void wordsToSeedThenPin(const char* pinHint) {
  setupBusy("Making your keys...");
  uint32_t t0 = millis();
  bip39ToSeed(gWords, "", gNewSeed);  // about a second: 2048 rounds of HMAC-SHA512
  Serial.printf("[%6lu ms] words -> seed took %lu ms\n", millis(),
                (unsigned long)(millis() - t0));
  gNewSeedHasWords = true;
  forgetSetup();
  showPin(kPinChoose, pinHint);
}

// ---- Restore: type the 24 words ----
//
// A to Z keyboard where letters that can't lead to a word are greyed out, so a typo is
// impossible. BIP39 words are unique after 4 letters, so you rarely type more than that:
// as soon as 4 or fewer words fit, they show up as buttons to tap.

int gRestoreIndex = 0;   // which word is being typed (0..23)
char gTyped[9];          // letters typed so far for this word
size_t gTypedLen = 0;
bool gFixing = false;    // retyping one word from the review screen
int gKeyTapped = -1;     // 0..25 = a..z, 26 = backspace
int gSuggestTapped = -1;
lv_obj_t* gTypedLabel = nullptr;
lv_obj_t* gRestoreTitle = nullptr;
lv_obj_t* gKeyboard = nullptr;
lv_obj_t* gSuggestBox = nullptr;
int gSuggestFirst = 0;   // number of the first suggested word
constexpr int kBackspace = 26;

// The keyboard's buttons, row by row ("\n" starts a new row, "" ends the list).
// LVGL keeps a pointer to this list, so it must live forever: static, not on the stack.
const char* const kKeyMap[] = {"a", "b", "c", "d", "e", "f", "g", "\n",
                               "h", "i", "j", "k", "l", "m", "n", "\n",
                               "o", "p", "q", "r", "s", "t", "u", "\n",
                               "v", "w", "x", "y", "z", LV_SYMBOL_BACKSPACE, ""};

void onKey(lv_event_t* e) {
  lv_obj_t* km = (lv_obj_t*)lv_event_get_target(e);
  gKeyTapped = (int)lv_buttonmatrix_get_selected_button(km);
  gSetupAction = kActKey;
}

void onSuggest(lv_event_t* e) {
  gSuggestTapped = (int)(intptr_t)lv_event_get_user_data(e);
  gSetupAction = kActSuggest;
}

// Redraws what changes while typing: the title, the letters, the greyed keys, the suggestions.
void updateRestore() {
  char buf[40];
  snprintf(buf, sizeof buf, gFixing ? "Fix word %d" : "Word %d of 24", gRestoreIndex + 1);
  lv_label_set_text(gRestoreTitle, buf);
  snprintf(buf, sizeof buf, "%s_", gTyped);
  lv_label_set_text(gTypedLabel, buf);

  uint32_t next = bip39NextLetters(gTyped);
  for (int k = 0; k < 26; k++) {
    if (next & (1u << k)) lv_buttonmatrix_clear_button_ctrl(gKeyboard, k, LV_BUTTONMATRIX_CTRL_DISABLED);
    else lv_buttonmatrix_set_button_ctrl(gKeyboard, k, LV_BUTTONMATRIX_CTRL_DISABLED);
  }

  lv_obj_clean(gSuggestBox);  // remove the old suggestions
  int count = bip39Matches(gTyped, &gSuggestFirst);
  if (gTypedLen == 0 || count > 4) {
    snprintf(buf, sizeof buf, gTypedLen == 0 ? "Type the first letters" : "%d words fit", count);
    lv_obj_t* t = text(gSuggestBox, buf, &inter_12, kMuted, 0, 0);
    lv_obj_center(t);
    return;
  }
  for (int i = 0; i < count; i++) {
    lv_obj_t* b = lv_button_create(gSuggestBox);
    lv_obj_set_pos(b, (i % 2) * 112, (i / 2) * 36);
    lv_obj_set_size(b, 104, 32);
    lv_obj_set_style_bg_color(b, i == 0 && count == 1 ? kLime : kCard, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_center(text(b, bip39Word(gSuggestFirst + i), &inter_16_semibold,
                       i == 0 && count == 1 ? kGround : kText, 0, 0));
    lv_obj_add_event_cb(b, onSuggest, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

void showRestore() {
  lv_obj_t* scr = setupScreen();

  // "<" goes back one word (or to Welcome from word 1).
  lv_obj_t* back = lv_button_create(scr);
  lv_obj_set_pos(back, 196, 0);
  lv_obj_set_size(back, 44, 28);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_center(text(back, LV_SYMBOL_LEFT, &inter_16_semibold, kMuted, 0, 0));
  lv_obj_add_event_cb(back, onSetupButton, LV_EVENT_CLICKED, (void*)(intptr_t)kActRestoreBack);

  gRestoreTitle = text(scr, "", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(gRestoreTitle, LV_ALIGN_TOP_MID, 0, 26);

  lv_obj_t* field = box(scr, 16, 52, 208, 36, kCard, 8);
  gTypedLabel = text(field, "", &inter_16_semibold, kLime, 0, 0);
  lv_obj_align(gTypedLabel, LV_ALIGN_LEFT_MID, 12, 0);

  gSuggestBox = box(scr, 12, 96, 216, 68, kGround, 0);

  gKeyboard = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(gKeyboard, kKeyMap);
  lv_obj_set_pos(gKeyboard, 0, 170);
  lv_obj_set_size(gKeyboard, 240, 150);
  lv_obj_set_style_bg_opa(gKeyboard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(gKeyboard, 0, 0);
  lv_obj_set_style_pad_all(gKeyboard, 4, 0);
  lv_obj_set_style_pad_gap(gKeyboard, 4, 0);
  lv_obj_set_style_bg_color(gKeyboard, kCard, LV_PART_ITEMS);
  lv_obj_set_style_text_color(gKeyboard, kText, LV_PART_ITEMS);
  lv_obj_set_style_radius(gKeyboard, 6, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(gKeyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(gKeyboard, 0, LV_PART_ITEMS);
  // Greyed out = no word continues with this letter. LVGL's theme would lay a light grey
  // "filter" over disabled keys; switch that off so they go dark instead.
  lv_obj_set_style_color_filter_opa(gKeyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(gKeyboard, kGround, LV_PART_ITEMS | LV_STATE_DISABLED);
  lv_obj_set_style_text_color(gKeyboard, kTrack, LV_PART_ITEMS | LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(gKeyboard, kLime, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(gKeyboard, kGround, LV_PART_ITEMS | LV_STATE_PRESSED);
  // Inter 16 carries the backspace icon too (scripts/make_fonts.sh adds it).
  lv_obj_set_style_text_font(gKeyboard, &inter_16_semibold, LV_PART_ITEMS);
  lv_obj_add_event_cb(gKeyboard, onKey, LV_EVENT_VALUE_CHANGED, nullptr);
  updateRestore();
}

void startWord(int index) {
  gRestoreIndex = index;
  wipe(gTyped, sizeof gTyped);
  gTypedLen = 0;
}

void showReview(const char* hint);
void showAddressCheck();

// A word is complete: store it, then the next word, the review, or the checksum.
void acceptWord(uint16_t word) {
  gWords[gRestoreIndex] = word;
  if (!gFixing && gRestoreIndex + 1 < kWordCount) {
    startWord(gRestoreIndex + 1);
    updateRestore();
    return;
  }
  gFixing = false;
  startWord(0);
  gTypedLabel = gRestoreTitle = gKeyboard = gSuggestBox = nullptr;
  if (bip39ChecksumOk(gWords)) {
    showAddressCheck();
  } else {
    showReview("These words don't fit together. Tap a word to fix it.");
  }
}

// ---- After restore: review and "is this your address?" ----

int gReviewPage = 0;

void showReview(const char* hint) {
  gReviewMode = true;
  lv_obj_t* scr = setupScreen();
  lv_obj_t* t = text(scr, gReviewPage == 0 ? "Words 1 to 12" : "Words 13 to 24",
                     &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
  centeredText(scr, hint, &inter_12, kOrange, 50, 216);
  wordGrid(scr, gReviewPage * 12, 80, true);
  setupButton(scr, 16, 268, 96, 44, "Start over", false, kActRestore);
  setupButton(scr, 120, 268, 104, 44, gReviewPage == 0 ? "13 to 24" : "1 to 12", true,
              gReviewPage == 0 ? kActWordsNext : kActWordsBack);
  loadAndDropOld(scr);
}

// The checksum catches almost every typo, but not all (1 in 256 slips through). Showing
// the address lets you compare it with the one your wallet app showed before.
void showAddressCheck() {
  setupBusy("Making your keys...");
  uint8_t seed[64], priv[32], pub[32];
  bip39ToSeed(gWords, "", seed);
  setSeed(seed);  // only for one moment, to work out the address
  deriveKey(kAccount1, 4, priv);
  publicKeyFromPrivate(priv, pub);
  clearSeed();
  memcpy(gNewSeed, seed, 64);  // kept for the PIN step: no need to run PBKDF2 twice
  wipe(seed, sizeof seed);
  wipe(priv, sizeof priv);
  char addr[45], grouped[64];
  base58Encode(pub, addr);
  groupAddress(addr, grouped, sizeof grouped);

  lv_obj_t* scr = setupScreen();
  lv_obj_t* t = text(scr, "Is this your wallet?", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 34);
  centeredText(scr, "Account 1 from your words:", &inter_12, kMuted, 64);
  lv_obj_t* card = box(scr, 16, 88, 208, 72, kCard, 8);
  lv_obj_t* a = text(card, grouped, &inter_12_bold, kLime, 0, 0, 188);
  lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(a);
  centeredText(scr, "Compare it with the address in your wallet app.", &inter_12, kMuted, 170);
  setupButton(scr, 16, 216, 208, 44, "Yes, that's it", true, kActAddrYes);
  setupButton(scr, 16, 266, 208, 40, "No, check the words", false, kActAddrNo);
  loadAndDropOld(scr);
}

// ---- What the buttons do ----

void handleSetupAction() {
  SetupAction act = gSetupAction;
  gSetupAction = kActNone;
  switch (act) {
    case kActCreate: {
      uint8_t entropy[kEntropySize];
      newEntropy(entropy);  // 32 random bytes (Bluetooth is on, so truly random)
      bip39FromEntropy(entropy, gWords);
      wipe(entropy, sizeof entropy);
      showWords(0);
      break;
    }
    case kActRestore:
      wipe(gWords, sizeof gWords);
      gFixing = false;
      startWord(0);
      showRestore();
      break;
    case kActLater:
      // Carry on with the old key (gOldSeed stays untouched until the new wallet is saved,
      // so "Later" still works after going into Create or Restore and back).
      memcpy(gNewSeed, gOldSeed, 64);
      wipe(gOldSeed, sizeof gOldSeed);
      forgetSetup();
      if (gSetupFrom == kFromVault) {  // unlocked already: carry on with the old key
        setSeed(gNewSeed);
        finishUnlock();
      } else {  // kFromPlain: old unlocked key, lock it with a PIN like before
        gNewSeedHasWords = false;
        showPin(kPinChoose, "Locks the key already on this board");
      }
      break;
    case kActWordsNext:
    case kActWordsBack:
      // The same two buttons turn the page on the words screen and on the review screen.
      if (gReviewMode) {
        gReviewPage = act == kActWordsNext ? 1 : 0;
        showReview("Tap any word that's wrong to retype it.");
      } else {
        showWords(act == kActWordsNext ? 1 : 0);
      }
      break;
    case kActWordsDone:
      pickCheckPositions();
      gCheckRound = 0;
      showCheck();
      break;
    case kActChoice:
      if (gChoices[gChoiceTapped] != gWords[gCheckPos[gCheckRound]]) {
        char hint[64];
        snprintf(hint, sizeof hint, "That's not word #%d. Check your paper.",
                 gCheckPos[gCheckRound] + 1);
        showCheck(hint);  // same question, new choices
      } else if (++gCheckRound < kChecks) {
        showCheck();
      } else {
        wordsToSeedThenPin("Backup checked. Now pick a PIN.");
      }
      break;
    case kActSeeWords:
      showWords(0);
      break;
    case kActKey:
      if (gKeyTapped == kBackspace) {
        if (gTypedLen > 0) gTyped[--gTypedLen] = '\0';
      } else if (gKeyTapped >= 0 && gKeyTapped < 26 && gTypedLen < 8 &&
                 (bip39NextLetters(gTyped) & (1u << gKeyTapped))) {
        gTyped[gTypedLen++] = (char)('a' + gKeyTapped);
        gTyped[gTypedLen] = '\0';
      }
      updateRestore();
      break;
    case kActSuggest:
      acceptWord((uint16_t)(gSuggestFirst + gSuggestTapped));
      break;
    case kActRestoreBack:
      if (gFixing || gRestoreIndex == 0) {
        bool wasFixing = gFixing;
        gFixing = false;
        gTypedLabel = gRestoreTitle = gKeyboard = gSuggestBox = nullptr;
        if (wasFixing) showReview("Tap a word to fix it.");
        else showWelcome(gSetupFrom);
      } else {
        startWord(gRestoreIndex - 1);
        updateRestore();
      }
      break;
    case kActFix:
      gFixing = true;
      startWord(gFixIndex);
      showRestore();
      break;
    case kActAddrYes:
      gNewSeedHasWords = true;
      forgetSetup();
      showPin(kPinChoose, "Wallet found. Now pick a PIN.");
      break;
    case kActAddrNo:
      wipe(gNewSeed, sizeof gNewSeed);
      gReviewPage = 0;
      showReview("Tap any word that's wrong to retype it.");
      break;
    case kActNone:
      break;
  }
}

// ---- Locking = back to the Dashboard ----------------------------------------------------
//
// Unlocked, the seed sits in RAM. Locking wipes it there (the vault in flash stays) and
// restarts the board into the Dashboard, so the Signer is only ever up while you use it.
// Two ways: 2 minutes without a touch, or BOOT on the idle screens. The PIN screen does
// the same, so a Signer nobody unlocks goes back too. Only touches count: a connected phone
// does NOT keep Sigly unlocked (like a Ledger).
//
// Not while the first wallet is being set up (no vault yet): the Dashboard has nothing to
// show then, and the words screens wait as long as you need.

constexpr uint32_t kAutoLockMs = 120000;
bool gBootWasDown = true;  // true at start: a BOOT held while powering on doesn't count

void lockNow(const char* why) {
  clearSeed();
  Serial.printf("[%6lu ms] locked: %s\n", millis(), why);
  lv_obj_t* scr = newScreen();
  lv_obj_t* t = text(scr, "Locked", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -10);
  lv_obj_t* sub = text(scr, "Back to the Dashboard...", &inter_12, kMuted, 0, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 16);
  loadAndDropOld(scr);
  lv_refr_now(nullptr);  // draw it now: the restart comes before the next frame
  restartInto(SiglyMode::kDashboard);
}

// Called from uiService() when nothing else is going on.
void checkLock() {
  bool bootDown = digitalRead(kBootButton) == LOW;
  bool pressed = bootDown && !gBootWasDown;  // only a NEW press, not one still held
  gBootWasDown = bootDown;

  if (!gHasVault || gSetupOpen || gAwaitingSig) return;  // first setup / busy signing
  // Choosing or repeating a new PIN (after restoring words) waits as long as you need.
  if (gPinMode == kPinChoose || gPinMode == kPinRepeat) return;
  // lv_display_get_inactive_time(): ms since the last touch (LVGL tracks it for us).
  if (lv_display_get_inactive_time(nullptr) >= kAutoLockMs) {
    lockNow("2 minutes without a touch");
  } else if (pressed && !gResultOpen) {  // on the receipt, BOOT means "close" instead
    lockNow("BOOT pressed");
  }
}

void lvglLog(lv_log_level_t, const char* msg) { Serial.print(msg); }

}  // namespace

void uiBegin(const char* deviceName) {
  gDeviceName = deviceName;
  Serial.println("ui: lv_init");
  kitBegin();  // screen, touch, backlight, LVGL
  lv_log_register_print_cb(lvglLog);  // LVGL warnings and errors go to the serial monitor
  // The logo signs itself in (~3.6 s). Its finished boot screen stays up until
  // uiStartLocked() loads the PIN screen, so there is no other screen in between.
  bootAnimationPlay(gDeviceName);
  Serial.println("ui: ready");
}

void uiStartLocked() {
  switch (seedState()) {
    case SeedState::kVault:
      gHasVault = true;
      if (failedTries() >= kMaxTries) {  // power was cut during the last allowed try
        wipeEverything();
        showWelcome(kFromWiped);
        break;
      }
      Serial.println("locked: enter your PIN on the screen");
      showPin(kPinEnter);
      break;
    case SeedState::kPlain:  // older firmware saved the seed unlocked
      Serial.println("found an unlocked seed with no words: offer a backup");
      if (loadPlainSeed(gOldSeed)) {
        showWelcome(kFromPlain);
      } else {
        showWelcome(kFromNothing);
      }
      break;
    case SeedState::kNone:
      Serial.println("first start: create a wallet or restore one from words");
      showWelcome(kFromNothing);
      break;
  }
}

void uiService() {
  if (gNav != kNavNone) {  // a button asked for another screen
    Nav go = gNav;
    gNav = kNavNone;
    if (go == kNavIdle) showIdle(gDeviceName);
    if (go == kNavReceive) showReceive();
    if (go == kNavSettings) showSettings();
  }
  if (gPinSubmit) handlePinSubmit();
  if (gSetupAction != kActNone) handleSetupAction();
  checkLock();
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
      if (boot) {
        delay(300);           // don't let the same press count twice
        gBootWasDown = true;  // and don't let it lock either: wait for the release
      }
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
