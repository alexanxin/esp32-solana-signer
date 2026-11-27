// CYD screen and touch: LVGL 9 draws, TFT_eSPI talks to the ILI9341 screen, XPT2046 reads touch.
// Only compiled for the `cyd` blend (see platformio.ini).
#ifdef BLEND_CYD
#include "ui_kit.h"

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "backlight.h"

namespace {


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


TFT_eSPI gTft;  // screen + touch, one SPI bus
// alignas(4): LVGL 9 refuses a buffer whose address is not a multiple of 4 (LV_DRAW_BUF_ALIGN).
alignas(4) uint8_t gDrawBuf[kScreenW * 10 * 2];  // LVGL draws 10 lines at a time (16-bit colour)


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


}  // namespace

void kitBegin() {
  lv_init();
  lv_tick_set_cb(tick);
  gTft.init();  // also switches the backlight on (TFT_BL in platformio.ini)
  gTft.setRotation(0);  // portrait 240x320
  gTft.fillScreen(TFT_BLACK);
  backlightBegin();  // dims the backlight to the saved level (PWM on GPIO 27)
  lv_display_t* disp = lv_display_create(kScreenW, kScreenH);
  lv_display_set_flush_cb(disp, flush);
  lv_display_set_buffers(disp, gDrawBuf, nullptr, sizeof gDrawBuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readTouch);
  pinMode(kBootButton, INPUT_PULLUP);
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
               int y, int width) {
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
  lv_obj_center(text(back, LV_SYMBOL_LEFT, &inter_16_semibold, kText, 0, 0));
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


void loadAndDropOld(lv_obj_t* scr) {
  lv_obj_t* old = lv_screen_active();
  lv_screen_load(scr);
  if (old != nullptr && old != scr) lv_obj_delete(old);
}

#endif  // BLEND_CYD
