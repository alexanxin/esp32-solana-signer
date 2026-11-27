// Boot animation: the Sigly logo signs itself in (design "Boot").
// Only compiled for the `cyd` blend: the whole file sits inside #ifdef BLEND_CYD.
//
// How it works, in plain terms:
//   * There is ONE clock: `t`, the milliseconds since the animation started.
//   * Every piece (ring, dashes, the S, the name, the bar) has a time window. phase() turns
//     "where is t inside my window" into a number from 0.0 (not started) to 1.0 (finished).
//   * Each frame we work out every piece from t alone and redraw. No stored images: the logo
//     is drawn from circles and a curve, so it costs almost no memory (the CYD has no PSRAM).
//
// Timeline (ms):     0      500     1000     1500     2000     2500     3000     3600
//   outer ring       [=sweep=]
//   dashed ring          [==spin in==]
//   the S                     [====pen draws====]
//   pulse ring                               [=ripple=]
//   name                                 [=rise=]
//   "Solana signer"                            [=fade=]
//   bar                                         [=======fill=======]
//   DEVNET badge                                       [fade]
//   hold                                                          [........]

#ifdef BLEND_CYD

#include "boot_anim.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

LV_FONT_DECLARE(inter_12);
LV_FONT_DECLARE(inter_12_bold);
LV_FONT_DECLARE(inter_28_bold);

namespace {

// Same colours as ui_cyd.cpp (the design canvas).
const lv_color_t kGround = lv_color_hex(0x0E1116);
const lv_color_t kText = lv_color_hex(0xEEF1F5);
const lv_color_t kMuted = lv_color_hex(0xA3ADBA);
const lv_color_t kLime = lv_color_hex(0xC7F284);
const lv_color_t kTrack = lv_color_hex(0x262D38);

constexpr uint32_t kTotalMs = 3600;  // whole animation including the hold at the end

// Layout on the 240x320 screen: logo, name, subtitle and bar stacked in the middle.
constexpr int kCx = 120, kCy = 114;  // centre of the logo
constexpr int kLogoBox = 160;        // drawing area around it (room for the pulse ring)
constexpr int kNameY = 176, kSubY = 218, kBarY = 249;
constexpr int kBarW = 120, kBarH = 4;

// The design's SVG logo lives in a 72x72 box. We draw it 96 px wide, so everything x 4/3.
constexpr float kScale = 96.0f / 72.0f;

// ---- Little maths helpers -----------------------------------------------------------------

// 0.0 before `start`, 1.0 after `start + length`, a straight ramp in between.
float phase(uint32_t t, uint32_t start, uint32_t length) {
  if (t <= start) return 0.0f;
  if (t >= start + length) return 1.0f;
  return (float)(t - start) / (float)length;
}

// Easing: makes motion start fast and settle gently, like a real object slowing down.
// Try graphing 1 - (1 - x)^3 between 0 and 1: steep at the start, flat at the end.
float easeOut(float x) {
  float inv = 1.0f - x;
  return 1.0f - inv * inv * inv;
}

// Slow start, fast middle, slow end. Nice for a pen stroke.
float easeInOut(float x) {
  return x < 0.5f ? 4.0f * x * x * x : 1.0f - powf(-2.0f * x + 2.0f, 3.0f) / 2.0f;
}

// A point of the SVG (72x72 box) to a screen pixel around the logo centre.
lv_point_t toScreen(float sx, float sy) {
  lv_point_t p;
  p.x = kCx + (int32_t)lroundf((sx - 36.0f) * kScale);
  p.y = kCy + (int32_t)lroundf((sy - 36.0f) * kScale);
  return p;
}

// ---- The S curve --------------------------------------------------------------------------
//
// The design's S is the SVG path "M27 44 c3 3 15 3 18-2 s-18-8-15-13 14-4 17-1": three cubic
// Bezier curves. Written out with absolute points, each curve is (start, control1, control2, end):
const float kCurves[3][4][2] = {
    {{27, 44}, {30, 47}, {42, 47}, {45, 42}},
    {{45, 42}, {48, 37}, {27, 34}, {30, 29}},
    {{30, 29}, {33, 24}, {44, 25}, {47, 28}},
};
// We turn the curves into a chain of short straight lines (enough of them, the eye can't tell)
// once at the start, and remember how long the chain is up to each point.
constexpr int kStepsPerCurve = 14;
constexpr int kSPoints = 3 * kStepsPerCurve + 1;
lv_point_t gS[kSPoints];
float gSLen[kSPoints];  // distance along the S from its start to point i

void buildS() {
  int n = 0;
  for (int c = 0; c < 3; c++) {
    for (int i = (c == 0 ? 0 : 1); i <= kStepsPerCurve; i++) {
      float u = (float)i / kStepsPerCurve, v = 1.0f - u;
      // Cubic Bezier: a weighted mix of the 4 points; the weights slide as u goes 0 -> 1.
      float w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
      float x = w0 * kCurves[c][0][0] + w1 * kCurves[c][1][0] + w2 * kCurves[c][2][0] + w3 * kCurves[c][3][0];
      float y = w0 * kCurves[c][0][1] + w1 * kCurves[c][1][1] + w2 * kCurves[c][2][1] + w3 * kCurves[c][3][1];
      gS[n] = toScreen(x, y);
      gSLen[n] = n == 0 ? 0.0f : gSLen[n - 1] + hypotf(gS[n].x - gS[n - 1].x, gS[n].y - gS[n - 1].y);
      n++;
    }
  }
}

// ---- Drawing ------------------------------------------------------------------------------

uint32_t gT = 0;  // the clock, in ms since the animation started

// An arc (part of a ring). Angles in degrees: 0 = 3 o'clock, going clockwise; 270 = 12 o'clock.
void arc(lv_layer_t* layer, int r, int width, float startDeg, float sweepDeg, lv_color_t color,
         lv_opa_t opa, bool rounded) {
  if (sweepDeg < 0.5f || opa == 0) return;
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center.x = kCx;
  d.center.y = kCy;
  d.radius = r;
  d.width = width;
  d.color = color;
  d.opa = opa;
  d.rounded = rounded;
  if (sweepDeg >= 360.0f) {
    d.start_angle = 0;
    d.end_angle = 360;
  } else {
    float s = fmodf(startDeg, 360.0f);
    float e = s + sweepDeg;
    if (e >= 360.0f) e -= 360.0f;  // LVGL draws "from 350 to 20" across 12 o'clock fine
    d.start_angle = (lv_value_precise_t)s;
    d.end_angle = (lv_value_precise_t)e;
  }
  lv_draw_arc(layer, &d);
}

// A filled dot: an arc whose thickness equals its radius fills the whole circle.
void dot(lv_layer_t* layer, lv_point_t p, int r, lv_color_t color, lv_opa_t opa) {
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center = p;
  d.radius = r;
  d.width = r;
  d.start_angle = 0;
  d.end_angle = 360;
  d.color = color;
  d.opa = opa;
  lv_draw_arc(layer, &d);
}

void segment(lv_layer_t* layer, lv_point_t a, lv_point_t b, int width, lv_color_t color) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.p1.x = a.x;
  d.p1.y = a.y;
  d.p2.x = b.x;
  d.p2.y = b.y;
  d.width = width;
  d.color = color;
  d.round_start = 1;  // round ends, so the short lines join without gaps
  d.round_end = 1;
  lv_draw_line(layer, &d);
}

// A point where the ring is at `deg` degrees.
lv_point_t onRing(int r, float deg) {
  float rad = deg * (float)M_PI / 180.0f;
  lv_point_t p;
  p.x = kCx + (int32_t)lroundf(r * cosf(rad));
  p.y = kCy + (int32_t)lroundf(r * sinf(rad));
  return p;
}

// LVGL calls this whenever the logo needs repainting. Everything comes from gT.
void drawLogo(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  const int rOuter = lroundf(32 * kScale), rInner = lroundf(22 * kScale);
  const int wOuter = 4, wInner = 2, wS = 4;

  // 1. Outer ring sweeps clockwise from 12 o'clock, a bright pen dot leading the way.
  float ring = easeOut(phase(gT, 0, 700));
  arc(layer, rOuter, wOuter, 270, 360 * ring, kLime, LV_OPA_COVER, true);
  if (ring > 0.0f && ring < 1.0f) {
    lv_point_t head = onRing(rOuter, 270 + 360 * ring);
    dot(layer, head, 7, kLime, LV_OPA_30);  // soft glow
    dot(layer, head, 3, kText, LV_OPA_COVER);
  }

  // 2. Dashed inner ring: 20 dashes appear one after another while the ring spins into place.
  float dashes = phase(gT, 300, 700);
  float spin = 120.0f * (1.0f - easeOut(dashes));  // starts turned 120 degrees, settles at 0
  int shown = (int)(dashes * 20.0f + 0.999f);
  for (int i = 0; i < shown; i++) {
    arc(layer, rInner, wInner, 270 + spin + i * 18.0f, 8.0f, kLime, LV_OPA_COVER, false);
  }

  // 3. The S is drawn like a signature: the pen moves along the curve, slow-fast-slow.
  float pen = easeInOut(phase(gT, 800, 900));
  if (pen > 0.0f) {
    float want = pen * gSLen[kSPoints - 1];  // how far along the S the pen has got
    lv_point_t tip = gS[0];
    for (int i = 1; i < kSPoints; i++) {
      if (gSLen[i] <= want) {
        segment(layer, gS[i - 1], gS[i], wS, kLime);
        tip = gS[i];
      } else {
        // Part of this little line: stop exactly where the pen is.
        float f = (want - gSLen[i - 1]) / (gSLen[i] - gSLen[i - 1]);
        tip.x = gS[i - 1].x + (int32_t)lroundf(f * (gS[i].x - gS[i - 1].x));
        tip.y = gS[i - 1].y + (int32_t)lroundf(f * (gS[i].y - gS[i - 1].y));
        segment(layer, gS[i - 1], tip, wS, kLime);
        break;
      }
    }
    if (pen < 1.0f) {
      dot(layer, tip, 7, kLime, LV_OPA_30);
      dot(layer, tip, 3, kText, LV_OPA_COVER);
    }
  }

  // 4. "Signed": one ring ripples out from the logo and fades away.
  float pulse = phase(gT, 1700, 650);
  if (pulse > 0.0f && pulse < 1.0f) {
    int r = rOuter + (int)lroundf(easeOut(pulse) * 30.0f);
    arc(layer, r, 2, 0, 360, kLime, (lv_opa_t)(150 * (1.0f - pulse)), false);
  }
}

}  // namespace

void bootAnimationPlay(const char* deviceName) {
  buildS();

  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, kGround, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // The logo: an empty, see-through object that we paint ourselves in drawLogo().
  lv_obj_t* logo = lv_obj_create(scr);
  lv_obj_remove_style_all(logo);  // no background, border or padding
  lv_obj_set_size(logo, kLogoBox, kLogoBox);
  lv_obj_set_pos(logo, kCx - kLogoBox / 2, kCy - kLogoBox / 2);
  lv_obj_add_event_cb(logo, drawLogo, LV_EVENT_DRAW_MAIN, nullptr);

  // Text fades with text_opa (cheap). Whole-object opacity would make LVGL paint into an extra
  // 24 KB buffer every frame, which this small board can't spare.
  lv_obj_t* name = lv_label_create(scr);
  lv_label_set_text(name, deviceName);
  lv_obj_set_style_text_font(name, &inter_28_bold, 0);
  lv_obj_set_style_text_color(name, kText, 0);
  lv_obj_set_style_text_letter_space(name, 1, 0);
  lv_obj_set_style_text_opa(name, LV_OPA_TRANSP, 0);

  lv_obj_t* sub = lv_label_create(scr);
  lv_label_set_text(sub, "Solana signer");
  lv_obj_set_style_text_font(sub, &inter_12, 0);
  lv_obj_set_style_text_color(sub, kMuted, 0);
  lv_obj_set_style_text_opa(sub, LV_OPA_TRANSP, 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, kSubY);

  // Bar: a dark track and a lime fill whose width grows.
  lv_obj_t* track = lv_obj_create(scr);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, kBarW, kBarH);
  lv_obj_set_style_radius(track, kBarH / 2, 0);
  lv_obj_set_style_bg_color(track, kTrack, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_TRANSP, 0);
  lv_obj_align(track, LV_ALIGN_TOP_MID, 0, kBarY);
  lv_obj_t* fill = lv_obj_create(track);
  lv_obj_remove_style_all(fill);
  lv_obj_set_size(fill, 0, kBarH);
  lv_obj_set_style_radius(fill, kBarH / 2, 0);
  lv_obj_set_style_bg_color(fill, kLime, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

  // DEVNET badge, bottom centre like the design.
  lv_obj_t* badge = lv_label_create(scr);
  lv_label_set_text(badge, "DEVNET");
  lv_obj_set_style_text_font(badge, &inter_12_bold, 0);
  lv_obj_set_style_text_color(badge, kGround, 0);
  lv_obj_set_style_bg_color(badge, kLime, 0);
  lv_obj_set_style_radius(badge, 4, 0);
  lv_obj_set_style_pad_hor(badge, 6, 0);
  lv_obj_set_style_pad_ver(badge, 1, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
  lv_obj_set_style_text_opa(badge, LV_OPA_TRANSP, 0);
  lv_obj_align(badge, LV_ALIGN_BOTTOM_MID, 0, -14);

  lv_screen_load(scr);

  uint32_t start = millis();
  do {
    gT = millis() - start;

    // The name rises 12 px into place while it fades in.
    float rise = easeOut(phase(gT, 1500, 500));
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, kNameY + (int)lroundf(12.0f * (1.0f - rise)));
    lv_obj_set_style_text_opa(name, (lv_opa_t)(255 * rise), 0);
    lv_obj_set_style_text_opa(sub, (lv_opa_t)(255 * phase(gT, 1900, 400)), 0);

    lv_obj_set_style_bg_opa(track, (lv_opa_t)(255 * phase(gT, 1900, 200)), 0);
    lv_obj_set_width(fill, (int32_t)lroundf(kBarW * easeInOut(phase(gT, 2000, 1000))));

    lv_opa_t b = (lv_opa_t)(255 * phase(gT, 2400, 300));
    lv_obj_set_style_bg_opa(badge, b, 0);
    lv_obj_set_style_text_opa(badge, b, 0);

    lv_obj_invalidate(logo);  // "repaint me": LVGL calls drawLogo on the next refresh
    lv_timer_handler();       // LVGL redraws what changed and pushes it to the screen
    delay(5);
  } while (gT < kTotalMs);
}

#endif  // BLEND_CYD
