// CYD screen and touch: LVGL 9 draws, TFT_eSPI talks to the ILI9341 screen, XPT2046 reads touch.
// Only compiled for the `cyd` blend (see platformio.ini).
#ifdef BLEND_CYD

#include "ui_cyd.h"

#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "tx_decode.h"

namespace {

// ---- Hardware ----------------------------------------------------------------------------

constexpr int kScreenW = 240, kScreenH = 320;  // portrait, the CYD's real pixels

// Touch chip on its own SPI bus (the screen uses the other one).
constexpr int kTouchClk = 25, kTouchMosi = 32, kTouchMiso = 39, kTouchCs = 33, kTouchIrq = 36;

// Touch calibration: the chip reports raw numbers (about 200..3900), we turn them into
// pixels. Boards differ a little. If taps land in the wrong place, watch the "touch raw"
// lines in the serial log, tap the corners, and adjust these.
constexpr int kRawXMin = 200, kRawXMax = 3700;
constexpr int kRawYMin = 240, kRawYMax = 3800;
constexpr bool kSwapXY = false;  // raw x is screen y?
constexpr bool kFlipX = false;
constexpr bool kFlipY = false;

constexpr int kBootButton = 0;  // BOOT = lock / reject. It can NEVER approve.

SPIClass gTouchSpi(VSPI);
XPT2046_Touchscreen gTouch(kTouchCs, kTouchIrq);
// alignas(4): LVGL 9 refuses a buffer whose address is not a multiple of 4 (LV_DRAW_BUF_ALIGN).
alignas(4) uint8_t gDrawBuf[kScreenW * 10 * 2];  // LVGL draws 10 lines at a time (16-bit colour)

// ---- Look (from the design canvas) -------------------------------------------------------

const lv_color_t kGround = lv_color_hex(0x0E1116);
const lv_color_t kCard = lv_color_hex(0x181D25);
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

uint32_t tick() { return millis(); }

void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  if (!gTouch.touched()) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  TS_Point p = gTouch.getPoint();
  int rx = kSwapXY ? p.y : p.x;
  int ry = kSwapXY ? p.x : p.y;
  int x = map(rx, kRawXMin, kRawXMax, 0, kScreenW - 1);
  int y = map(ry, kRawYMin, kRawYMax, 0, kScreenH - 1);
  if (kFlipX) x = kScreenW - 1 - x;
  if (kFlipY) y = kScreenH - 1 - y;
  data->point.x = constrain(x, 0, kScreenW - 1);
  data->point.y = constrain(y, 0, kScreenH - 1);
  data->state = LV_INDEV_STATE_PRESSED;

  static uint32_t lastLog = 0;  // log a touch at most 4 times a second
  if (millis() - lastLog > 250) {
    lastLog = millis();
    Serial.printf("[%6lu ms] touch raw %d,%d -> pixel %d,%d\n", millis(), p.x, p.y,
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

void devnetBadge(lv_obj_t* scr) {
  lv_obj_t* badge = box(scr, 10, 4, 58, 16, kLime, 4);
  lv_obj_t* label = text(badge, "DEVNET", &lv_font_montserrat_12, kGround, 0, 0);
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

// ---- Screens -----------------------------------------------------------------------------

void onHoldEvent(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    gHolding = true;
    gHoldStart = millis();
    lv_label_set_text(gHint, "Keep holding...");
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (gHolding && gDecision == kPending) {
      lv_label_set_text(gHint, "Let go too early. Nothing signed, hold again.");
      lv_arc_set_value(gArc, 0);
    }
    gHolding = false;
  }
}

void onRejectEvent(lv_event_t*) { gDecision = kRejected; }

void showSignScreen(const TxSummary& tx) {
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  gCountdown = text(scr, "0:30", &lv_font_montserrat_12, kMuted, 196, 6);

  char buf[96];
  bool known = tx.valid && tx.kind == TxSummary::kSolTransfer;

  if (known) {
    char amount[24], addr[45], grouped[64];
    formatSol(tx.lamports, amount);
    text(scr, "Send", &lv_font_montserrat_12, kMuted, 16, 30);
    snprintf(buf, sizeof buf, "%s SOL", amount);
    text(scr, buf, &lv_font_montserrat_28, kText, 16, 46, 208);

    lv_obj_t* card = box(scr, 16, 86, 208, 120, kCard, 8);
    text(card, "To  (check every group)", &lv_font_montserrat_12, kMuted, 10, 8);
    base58Encode(tx.to, addr);
    size_t len = strlen(addr);
    snprintf(buf, sizeof buf, "%.4s ... %s", addr, addr + (len > 4 ? len - 4 : 0));
    text(card, buf, &lv_font_montserrat_16, kLime, 10, 26);
    groupAddress(addr, grouped, sizeof grouped);
    text(card, grouped, &lv_font_montserrat_12, kText, 10, 50, 188);
    gHoldMs = kHoldKnownMs;
  } else {
    lv_obj_t* warn = box(scr, 16, 30, 208, 58, lv_color_hex(0x2B2414), 8);
    lv_obj_set_style_border_color(warn, kAmber, 0);
    lv_obj_set_style_border_width(warn, 1, 0);
    text(warn, tx.valid ? "Can't read this one. Only sign if you just started it in your wallet."
                        : "This doesn't look like a Solana transaction. Reject it.",
         &lv_font_montserrat_12, kAmber, 8, 6, 190);

    snprintf(buf, sizeof buf, "It talks to %u program%s", (unsigned)tx.programCount,
             tx.programCount == 1 ? "" : "s");
    text(scr, buf, &lv_font_montserrat_12, kMuted, 16, 96);
    for (size_t i = 0; i < tx.programCount && i < 3; i++) {
      lv_obj_t* row = box(scr, 16, 114 + (int)i * 32, 208, 28, kCard, 8);
      const char* name = knownProgramName(tx.programs[i]);
      char addr[45];
      if (name == nullptr) {
        base58Encode(tx.programs[i], addr);
        size_t len = strlen(addr);
        snprintf(buf, sizeof buf, "%.4s...%s  (unknown)", addr, addr + (len > 4 ? len - 4 : 0));
        name = buf;
      }
      text(row, name, &lv_font_montserrat_12, kText, 10, 7);
    }
    gHoldMs = kHoldUnknownMs;
  }

  if (tx.valid) {
    char fee[24];
    formatSol(tx.feeLamports, fee);
    snprintf(buf, sizeof buf, "Network fee  %s SOL", fee);
    text(scr, buf, &lv_font_montserrat_12, kMuted, 16, 214);
  }

  gHint = text(scr, "", &lv_font_montserrat_12, kMuted, 16, 236, 208);

  // Reject: outlined pill, bottom left.
  lv_obj_t* reject = lv_button_create(scr);
  lv_obj_set_pos(reject, 12, 256);
  lv_obj_set_size(reject, 72, 52);
  lv_obj_set_style_bg_opa(reject, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(reject, lv_color_hex(0x3A4250), 0);
  lv_obj_set_style_border_width(reject, 2, 0);
  lv_obj_set_style_radius(reject, 26, 0);
  lv_obj_set_style_shadow_width(reject, 0, 0);
  lv_obj_center(text(reject, "Reject", &lv_font_montserrat_12, kText, 0, 0));
  lv_obj_add_event_cb(reject, onRejectEvent, LV_EVENT_CLICKED, nullptr);

  // Hold to sign: big pill with a ring that fills while you hold.
  lv_obj_t* hold = lv_button_create(scr);
  lv_obj_set_pos(hold, 92, 256);
  lv_obj_set_size(hold, 136, 52);
  lv_obj_set_style_bg_color(hold, known ? kLime : kAmber, 0);
  lv_obj_set_style_radius(hold, 26, 0);
  lv_obj_set_style_shadow_width(hold, 0, 0);
  lv_obj_add_event_cb(hold, onHoldEvent, LV_EVENT_ALL, nullptr);

  gArc = lv_arc_create(hold);
  lv_obj_set_size(gArc, 30, 30);
  lv_obj_align(gArc, LV_ALIGN_LEFT_MID, 4, 0);
  lv_arc_set_rotation(gArc, 270);
  lv_arc_set_bg_angles(gArc, 0, 360);
  lv_arc_set_range(gArc, 0, 100);
  lv_arc_set_value(gArc, 0);
  lv_obj_remove_style(gArc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(gArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(gArc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(gArc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(gArc, lv_color_hex(0x8A9A6A), LV_PART_MAIN);
  lv_obj_set_style_arc_color(gArc, kGround, LV_PART_INDICATOR);

  text(hold, known ? "Hold to sign" : "Hold 3 s", &lv_font_montserrat_16, kGround, 40, 16);

  lv_screen_load(scr);
}

void showIdle(const char* deviceName) {
  lv_obj_t* old = lv_screen_active();
  lv_obj_t* scr = newScreen();
  devnetBadge(scr);
  text(scr, "Signer", &lv_font_montserrat_12, kMuted, 186, 6);

  lv_obj_t* ring = lv_arc_create(scr);
  lv_obj_set_size(ring, 96, 96);
  lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 56);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_value(ring, 0);
  lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(ring, kLime, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, 2, LV_PART_MAIN);

  lv_obj_t* title = text(scr, "Send from your wallet app", &lv_font_montserrat_16, kText, 0, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 168);
  char buf[96];
  snprintf(buf, sizeof buf, "Open Phantom or Jupiter and send. %s shows up as a Ledger.",
           deviceName);
  lv_obj_t* sub = text(scr, buf, &lv_font_montserrat_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 196);

  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
}

void showResult(Decision d, bool timedOut) {
  lv_obj_t* old = lv_screen_active();
  lv_obj_t* scr = newScreen();
  const char* title = d == kApproved ? "Signed" : timedOut ? "Timed out" : "Rejected";
  const char* body = d == kApproved  ? "Your wallet is sending it now."
                     : timedOut      ? "No answer in 30 s, so nothing was signed."
                                     : "Nothing was signed.";
  lv_color_t color = d == kApproved ? kLime : timedOut ? kMuted : kOrange;

  lv_obj_t* dot = box(scr, 84, 80, 72, 72, color, 36);
  lv_obj_t* mark = text(dot, d == kApproved ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, &lv_font_montserrat_28,
                        kGround, 0, 0);
  lv_obj_center(mark);
  lv_obj_t* t = text(scr, title, &lv_font_montserrat_16, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 168);
  lv_obj_t* b = text(scr, body, &lv_font_montserrat_12, kMuted, 0, 0, 208);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 194);

  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
}

const char* gDeviceName = "Sigly";

void lvglLog(lv_log_level_t, const char* msg) { Serial.print(msg); }

}  // namespace

void uiBegin(const char* deviceName) {
  gDeviceName = deviceName;
  Serial.println("ui: lv_init");
  lv_init();
  lv_log_register_print_cb(lvglLog);  // LVGL warnings and errors go to the serial monitor
  lv_tick_set_cb(tick);
  Serial.println("ui: screen");
  lv_tft_espi_create(kScreenW, kScreenH, gDrawBuf, sizeof gDrawBuf);
  Serial.println("ui: touch");

  gTouchSpi.begin(kTouchClk, kTouchMiso, kTouchMosi, kTouchCs);
  gTouch.begin(gTouchSpi);
  gTouch.setRotation(0);
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readTouch);

  pinMode(kBootButton, INPUT_PULLUP);
  showIdle(gDeviceName);
  Serial.println("ui: ready");
}

void uiService() { lv_timer_handler(); }

bool uiApprove(const uint8_t* message, size_t len) {
  logHex(">>> SIGN REQUEST, message", message, len);

  TxSummary tx;
  decodeTransaction(message, len, &tx);  // tx.valid says whether it made sense
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
      //   percent = elapsed * 100 / gHoldMs;     // e.g. 750 ms of 1500 ms -> 50
      //   if (percent >= 100) {                  // held long enough?
      //     percent = 100;                       // never show more than a full ring
      //     gDecision = kApproved;
      //   }
      //
      // Until this is written, percent stays 0: the ring never fills and nothing can be
      // approved. That's the safe way to be unfinished.

      lv_arc_set_value(gArc, percent);
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
  showResult(gDecision, timedOut);
  uint32_t shown = millis();
  while (millis() - shown < 1500) {  // let the human see the result
    lv_timer_handler();
    delay(5);
  }
  showIdle(gDeviceName);
  return approved;
}

#endif  // BLEND_CYD
