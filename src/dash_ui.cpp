// Dashboard screens on the CYD (design: Main, Activity, Market, Heatmap, Receive, Settings with the "J" Send
// button: a lime quarter circle in the bottom-right corner). Dashboard mode never asks for
// the PIN and never touches the seed: Send restarts the board into Signer mode for that.
#ifdef BLEND_CYD
#include "dash_ui.h"

#include <Arduino.h>
#include <esp_system.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "activity.h"
#include "dash_net.h"
#include "dash_store.h"
#include "heatmap.h"
#include "icons/token_icons.h"
#include "mode.h"
#include "portfolio.h"
#include "ui_kit.h"

namespace {

enum Screen { kScrNone, kScrHome, kScrActivity, kScrMarket, kScrHeatmap, kScrToken, kScrReceive, kScrSettings, kScrSetup, kScrNoAddress };
Screen gShown = kScrNone;
Screen gNav = kScrNone;  // set by buttons, acted on in dashUiService (never delete inside an event)
bool gSendTapped = false;
bool gSetupTapped = false;
bool gRetryTapped = false;

char gAddress[45] = "";
bool gHaveSetup = false;
DashSetup gSetup;
bool gPortalOpen = false;

Portfolio gPf;            // what the screens show
bool gHavePf = false;
uint32_t gPfVersion = 0;  // which refresh gPf came from
uint32_t gPfAt = 0;       // unix time of that refresh (0 = unknown)

Activity* gAct = nullptr;  // the Activity tab's list (heap: DRAM is nearly full)
bool gHaveAct = false;     // from flash or from the network
uint32_t gActVersion = 0;
Chart* gChart = nullptr;  // the latest price chart from the task (heap, made in dashUiBegin)
uint32_t gChartVersion = 0;

lv_obj_t* gClock = nullptr;    // "12:40" in the status bar
lv_obj_t* gWifi = nullptr;     // Wi-Fi icon in the status bar
lv_obj_t* gStatus = nullptr;   // "Updated 12:38" / "No Wi-Fi..." on Home and Tokens
NetState gShownState = NetState::kOff;
uint32_t gLastClock = 0;

// ---- Pieces every Dashboard screen has ---------------------------------------------------

void hhmm(uint32_t unixTime, char out[8]) {
  out[0] = '\0';
  if (unixTime == 0) return;
  time_t t = unixTime;
  struct tm local;
  localtime_r(&t, &local);
  strftime(out, 8, "%H:%M", &local);
}

void updateClock() {
  if (gClock == nullptr) return;
  time_t now = time(nullptr);
  char s[8];
  hhmm(now > 1700000000 ? (uint32_t)now : 0, s);  // before 2023 = clock not set yet
  lv_label_set_text(gClock, s);
  if (gWifi != nullptr) {  // hidden while Wi-Fi isn't in use (setup, no address yet)
    show(gWifi, dashNetState() != NetState::kOff);
    lv_obj_set_style_text_color(gWifi, dashNetConnected() ? kMuted : kOrange, 0);
  }
}

void statusBar(lv_obj_t* scr) {
  devnetBadge(scr);
  gClock = text(scr, "", &inter_12, kMuted, 0, 0);
  lv_obj_align(gClock, LV_ALIGN_TOP_RIGHT, -10, 5);
  gWifi = text(scr, LV_SYMBOL_WIFI, &inter_12, kMuted, 0, 0);
  lv_obj_align(gWifi, LV_ALIGN_TOP_RIGHT, -48, 5);
  updateClock();
}

void onSend(lv_event_t*) { gSendTapped = true; }

// Design "J": a lime quarter circle (radius 64) in the bottom-right corner. It is a full
// 128x128 circle placed so three quarters of it hang off the screen; only the visible
// quarter can be touched.
void sendCorner(lv_obj_t* scr) {
  lv_obj_t* c = lv_button_create(scr);
  lv_obj_set_pos(c, kScreenW - 64, kScreenH - 64);
  lv_obj_set_size(c, 128, 128);
  lv_obj_set_style_radius(c, 64, 0);
  lv_obj_set_style_bg_color(c, kLime, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_add_event_cb(c, onSend, LV_EVENT_CLICKED, nullptr);
  // Only the top-left quarter of this circle is on the screen: a quarter disk of radius 64
  // whose square corner is the screen's corner (64, 64 inside the button). The point that
  // looks like its middle is the quarter disk's centre of mass ("centroid"), which sits
  // 4r / (3 pi) = 4 * 64 / 9.42 = 27 px in from the corner on both axes: at (37, 37).
  // (The middle of the 64 x 64 square, (32, 32), would look too close to the curved edge.)
  // Icon and word are stacked as one block, and that block is centred on that point.
  constexpr int kCentre = 64 - 27;
  constexpr int kGap = 2;  // between icon and word
  lv_obj_t* plane = text(c, SIG_SYMBOL_PAPER_PLANE, &inter_16_semibold, kGround, 0, 0);
  lv_obj_t* label = text(c, "Send", &inter_12_bold, kGround, 0, 0);
  lv_obj_update_layout(c);  // so the labels know their size
  int planeW = lv_obj_get_width(plane), planeH = lv_obj_get_height(plane);
  int labelW = lv_obj_get_width(label), labelH = lv_obj_get_height(label);
  int top = kCentre - (planeH + kGap + labelH) / 2;
  lv_obj_set_pos(plane, kCentre - planeW / 2, top);
  lv_obj_set_pos(label, kCentre - labelW / 2, top + planeH + kGap);
}

void onTab(lv_event_t* e) { gNav = (Screen)(intptr_t)lv_event_get_user_data(e); }

void tab(lv_obj_t* bar, int i, const char* icon, const char* label, Screen to, bool on) {
  lv_obj_t* b = lv_button_create(bar);
  lv_obj_set_pos(b, i * 44, 0);
  lv_obj_set_size(b, 44, 52);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);

  // 44 px is too narrow for four labels side by side, so only the open tab shows its name.
  lv_color_t c = on ? kLime : kMuted;
  lv_obj_t* ic = text(b, icon, &inter_16_semibold, c, 0, 0);
  if (on) {
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 9);
    lv_obj_t* l = text(bar, label, &inter_12_bold, c, 0, 0);   
    lv_obj_align_to(l, b, LV_ALIGN_TOP_MID, i == 3 ? -3 : 0, 30);
  } else {
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, 0);
  }
  lv_obj_add_event_cb(b, onTab, LV_EVENT_CLICKED, (void*)(intptr_t)to);
}

// Four tabs on the left 176 px, the Send corner on the right. Settings is the gear at the
// top of Home. Home shows every holding (there used to be a Tokens tab saying the same).
void tabBar(lv_obj_t* scr, Screen current) {
  lv_obj_t* bar = box(scr, 0, kScreenH - 52, kScreenW, 52, kCard, 0);
  tab(bar, 0, LV_SYMBOL_HOME, "Home", kScrHome, current == kScrHome);
  tab(bar, 1, SIG_SYMBOL_EXCHANGE, "Activity", kScrActivity, current == kScrActivity);
  tab(bar, 2, SIG_SYMBOL_CHART_LINE, "Market", kScrMarket, current == kScrMarket);
  tab(bar, 3, SIG_SYMBOL_QRCODE, "Receive", kScrReceive, current == kScrReceive);
  sendCorner(scr);
}

// The line under the numbers: when they are from, or what went wrong.
void updateStatus() {
  if (gStatus == nullptr) return;
  NetState s = dashNetState();
  gShownState = s;
  char when[8], buf[96];
  hhmm(gPfAt, when);
  lv_color_t color = kMuted;
  switch (s) {
    case NetState::kConnecting:
      snprintf(buf, sizeof buf, "Joining %s...", gSetup.ssid);
      break;
    case NetState::kFetching:
      snprintf(buf, sizeof buf, "Updating...");
      break;
    case NetState::kNoWifi:
      snprintf(buf, sizeof buf, "Can't join %s. %s", gSetup.ssid,
               gHavePf ? "Showing the last numbers." : "Check Settings > Wi-Fi.");
      color = kAmber;
      break;
    case NetState::kRpcError:
      snprintf(buf, sizeof buf, "Solana RPC not answering.%s",
               gHavePf ? " Showing the last numbers." : "");
      color = kAmber;
      break;
    case NetState::kPriceError:
      snprintf(buf, sizeof buf, "Balances updated %s. Prices busy, trying again.", when);
      color = kAmber;
      break;
    default:
      if (when[0] != '\0') snprintf(buf, sizeof buf, "Updated %s", when);
      else snprintf(buf, sizeof buf, "Updated");
  }
  lv_label_set_text(gStatus, buf);
  lv_obj_set_style_text_color(gStatus, color, 0);
}

// The new screen goes up empty and the old one is deleted BEFORE the new one is filled,
// so the two never take LVGL's memory (40 KB) at the same time. Nothing is drawn until
// lv_timer_handler() runs, so you never see the empty screen.
lv_obj_t* newDashScreen(Screen which) {
  gClock = gWifi = gStatus = nullptr;  // their old screen is about to go
  gShown = which;
  lv_obj_t* scr = newScreen();
  loadAndDropOld(scr);
  return scr;
}

// ---- Home (design "Main") ----------------------------------------------------------------

void holdingsList(lv_obj_t* scr, int x, int y, int w, int h);  // defined with the rows below
// Settings: a gear in the status bar, left of the Wi-Fi icon. The touch area (40 x 28) is
// bigger than the icon, so a finger finds it.
void settingsGear(lv_obj_t* scr) {
  lv_obj_t* b = lv_button_create(scr);
  lv_obj_set_size(b, 40, 28);
  lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -64, -2);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_t* ic = text(b, LV_SYMBOL_SETTINGS, &inter_16_semibold, kMuted, 0, 0);
  lv_obj_center(ic);
  lv_obj_add_event_cb(b, [](lv_event_t*) { gNav = kScrSettings; }, LV_EVENT_CLICKED, nullptr);
}

void showHome() {
  lv_obj_t* scr = newDashScreen(kScrHome);
  statusBar(scr);
  settingsGear(scr);
  text(scr, "Net worth \xC2\xB7 Account 1", &inter_12, kMuted, 16, 30);

  char total[24] = "$...", change[48] = "";
  bool up = true;
  if (gHavePf && gPf.anyPriced) {
    formatUsd(gPf.totalUsd, total, sizeof total);
    if (gPf.totalUsd24hAgo > 0) {
      formatChange(gPf.totalUsd, gPf.totalUsd24hAgo, change, sizeof change, &up);
      strncat(change, " 24h", sizeof change - strlen(change) - 1);
    }
  } else if (gHavePf) {
    snprintf(change, sizeof change, "No prices yet");
  }
  text(scr, total, &inter_28_bold, gHavePf ? kText : kMuted, 16, 46);
  text(scr, change, &inter_12_bold, up ? kLime : kOrange, 16, 84);

  if (gHavePf && gPf.hidden > 0) {
    char hid[24];
    snprintf(hid, sizeof hid, "%u hidden", gPf.hidden);
    lv_obj_t* h = text(scr, hid, &inter_12, kMuted, 0, 0);
    lv_obj_align(h, LV_ALIGN_TOP_RIGHT, -16, 84);
  }
  // Every holding, most valuable first, in a list that scrolls with a finger.
  if (gHavePf) holdingsList(scr, 8, 106, 224, 142);
  gStatus = text(scr, "", &inter_12, kMuted, 16, 250, 150);
  lv_label_set_long_mode(gStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_height(gStatus, 15);  // one line: the tab bar starts right under it
  updateStatus();
  tabBar(scr, kScrHome);
  loadAndDropOld(scr);
}

// ---- Holding rows (Home) ------------------------------------------------------------------

// Token rows use shared styles instead of text(): text() gives every label its own copy of
// font and colour ("local" styles), which costs memory per label. A list of rows has many
// labels, so here they all point at the same few styles.
lv_style_t gStBold, gStMuted, gStUp, gStDown, gStRow, gStRowPressed, gStDot, gStSkel;
bool gStylesReady = false;

void initRowStyles() {
  if (gStylesReady) return;
  gStylesReady = true;
  lv_style_init(&gStBold);
  lv_style_set_text_font(&gStBold, &inter_12_bold);
  lv_style_set_text_color(&gStBold, kText);
  lv_style_init(&gStMuted);
  lv_style_set_text_font(&gStMuted, &inter_12);
  lv_style_set_text_color(&gStMuted, kMuted);
  lv_style_init(&gStUp);
  lv_style_set_text_font(&gStUp, &inter_12_bold);
  lv_style_set_text_color(&gStUp, kLime);
  lv_style_init(&gStDown);
  lv_style_set_text_font(&gStDown, &inter_12_bold);
  lv_style_set_text_color(&gStDown, kOrange);
  lv_style_init(&gStRow);
  lv_style_set_bg_color(&gStRow, kCard);
  lv_style_set_bg_opa(&gStRow, LV_OPA_COVER);
  lv_style_set_radius(&gStRow, 8);
  lv_style_set_border_width(&gStRow, 0);
  lv_style_set_pad_all(&gStRow, 0);
  lv_style_init(&gStRowPressed);  // a row or card you are touching lights up a little
  lv_style_set_bg_color(&gStRowPressed, kTrack);
  lv_style_init(&gStDot);
  lv_style_set_bg_color(&gStDot, kTrack);
  lv_style_set_bg_opa(&gStDot, LV_OPA_COVER);
  lv_style_set_radius(&gStDot, 12);
  lv_style_set_border_width(&gStDot, 0);
  lv_style_set_pad_all(&gStDot, 0);
  lv_style_set_text_font(&gStDot, &inter_12_bold);
  lv_style_set_text_color(&gStDot, kLime);
  lv_style_set_text_align(&gStDot, LV_TEXT_ALIGN_CENTER);
  lv_style_set_pad_top(&gStDot, 5);
  lv_style_init(&gStSkel);
  lv_style_set_bg_color(&gStSkel, kTrack);
  lv_style_set_bg_opa(&gStSkel, LV_OPA_COVER);
  lv_style_set_radius(&gStSkel, 5);
  lv_style_set_border_width(&gStSkel, 0);
}

lv_obj_t* rowLabel(lv_obj_t* parent, const char* s, lv_style_t* st) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);  // no theme styles either: only ours
  lv_obj_add_style(l, st, 0);
  lv_label_set_text(l, s);
  return l;
}

// The round 24 px badge at the start of a row: the token's logo when the firmware has one
// (built in from Jupiter's logos, see scripts/make_token_icons.py), else its first letter.
// Symbols come from our own list of known mints, never from the token itself, so a scam
// token calling itself "USDC" shows "Ab3x..." and a letter, not the USDC logo.
void tokenBadge(lv_obj_t* row, const char* symbol, int x, int y) {
  const lv_image_dsc_t* icon = tokenIcon(strcmp(symbol, "wSOL") == 0 ? "SOL" : symbol);
  if (icon != nullptr) {
    lv_obj_t* img = lv_image_create(row);
    lv_image_set_src(img, icon);
    lv_obj_set_pos(img, x, y);
    return;
  }
  // The "initial" badge is a label with a background: one object instead of two.
  char initial[2] = {symbol[0], '\0'};
  lv_obj_t* dot = rowLabel(row, initial, &gStDot);
  lv_obj_set_size(dot, 24, 24);
  lv_obj_set_pos(dot, x, y);
}

// ---- Opening a token's detail screen ------------------------------------------------------

// Which token the detail screen shows. We keep its mint, not its place in the list: a
// refresh can re-sort the list while the screen is open.
struct DetailPick {
  char symbol[12];
  char mint[45];
  Screen from;  // where the back arrow goes (Home or Market)
} gDetail;
ChartRange gRange = ChartRange::k1D;  // 1D / 1W / 1M, kept from one token to the next
bool gChartAsked = false;             // the chart for gDetail + gRange is asked for

// A tap on a Home row or a Market card. user_data says which one: 0..23 = gPf.items[i],
// 100 + i = gPf.watch[i] (the Market grid).
constexpr int kWatchTap = 100;
constexpr int kHeatTap = 200;  // next to kWatchTap

void onTokenTap(lv_event_t* e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  const Holding* h;              // a pointer: "which holding", decided below
  switch (i / 100) {
    case 1:                      // a Market card
      h = &gPf.watch[i - kWatchTap];
      gDetail.from = kScrMarket;
      break;                     // done: without break, C runs into the next case too!
    case 2:                      // a heatmap tile
      h = &gPf.items[i - kHeatTap];
      gDetail.from = kScrHeatmap;
      break;
    default:                     // anything else (0): a Home row
      h = &gPf.items[i];
      gDetail.from = kScrHome;
      break;
  }
  snprintf(gDetail.symbol, sizeof gDetail.symbol, "%s", h->symbol);
  snprintf(gDetail.mint, sizeof gDetail.mint, "%s", h->mint);
  gChartAsked = false;
  gNav = kScrToken;
}

// Makes a row or card tappable. It stays non-scrollable, so a finger dragging over it still
// scrolls the list it sits in (LVGL hands the drag to the parent).
void tappable(lv_obj_t* obj, int which) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_style(obj, &gStRowPressed, LV_STATE_PRESSED);
  lv_obj_add_event_cb(obj, onTokenTap, LV_EVENT_CLICKED, (void*)(intptr_t)which);
}

void tokenRow(lv_obj_t* list, const Holding& h, int index) {
  initRowStyles();
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_add_style(row, &gStRow, 0);
  lv_obj_set_size(row, 224, 42);
  lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  tappable(row, index);

  tokenBadge(row, h.symbol, 8, 9);
  lv_obj_set_pos(rowLabel(row, h.symbol, &gStBold), 40, 5);
  char s[24];
  formatAmount(h.amount, s, sizeof s);
  lv_obj_set_pos(rowLabel(row, s, &gStMuted), 40, 21);

  if (h.priced) formatUsd(h.amount * h.usdPrice, s, sizeof s);
  else snprintf(s, sizeof s, "-");
  lv_obj_align(rowLabel(row, s, &gStBold), LV_ALIGN_TOP_RIGHT, -8, 5);
  bool up = true;
  if (h.hasChange) formatPercent(h.change24h, s, sizeof s, &up);
  else snprintf(s, sizeof s, h.priced ? "" : "no price");
  lv_obj_t* ch = rowLabel(row, s, h.hasChange ? (up ? &gStUp : &gStDown) : &gStMuted);
  lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, -8, 21);
}

bool gSkeletonShown = false;  // a skeleton is on screen: redraw when loading ends

// True while a price may still arrive: joining Wi-Fi or asking right now.
bool loading() {
  NetState s = dashNetState();
  return s == NetState::kConnecting || s == NetState::kFetching;
}

// A skeleton bar: a grey placeholder the size of the text that will replace it. It
// "breathes" (fades between dim and bright) so it's clearly waiting, not broken.
// LVGL stops the animation by itself when the screen and the bar are deleted.
void skeletonBar(lv_obj_t* parent, int w, lv_align_t align, int x) {
  lv_obj_t* bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_add_style(bar, &gStSkel, 0);
  lv_obj_set_size(bar, w, 10);
  lv_obj_align(bar, align, x, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bar);
  lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
  });
  lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
  lv_anim_set_duration(&a, 700);
  lv_anim_set_playback_duration(&a, 700);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  gSkeletonShown = true;
}

// Your holdings as rows (logo, name, amount, value, 24 h move) in a scrolling list.
void holdingsList(lv_obj_t* scr, int x, int y, int w, int h) {
  lv_obj_t* list = box(scr, x, y, w, h, kGround, 0);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 2, 0);
  // At most 6 rows: each row is 5-6 LVGL objects, and LVGL has 40 KB for every screen
  // (measured in the simulator).
  constexpr int kRows = 6;
  int shown = gPf.count < kRows ? gPf.count : kRows;
  for (int i = 0; i < shown; i++) tokenRow(list, gPf.items[i], i);
  if (gPf.count > kRows) {
    char more[32];
    snprintf(more, sizeof more, "+%d smaller ones", gPf.count - kRows);
    text(list, more, &inter_12, kMuted, 0, 0);
  }
}

// ---- Activity ----------------------------------------------------------------------------

lv_style_t gStIcon;  // the round icon at the start of an activity row

void activityRow(lv_obj_t* list, const TxItem& t, uint32_t now) {
  if (gStIcon.prop_cnt == 0) {
    lv_style_init(&gStIcon);
    lv_style_set_bg_color(&gStIcon, kTrack);
    lv_style_set_bg_opa(&gStIcon, LV_OPA_COVER);
    lv_style_set_radius(&gStIcon, 12);
    lv_style_set_text_font(&gStIcon, &inter_16_semibold);
    lv_style_set_text_align(&gStIcon, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&gStIcon, 2);  // (24 px circle - 20 px line) / 2: icon in the middle
  }
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_add_style(row, &gStRow, 0);
  lv_obj_set_size(row, 224, 42);
  lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  // What happened, in words and as an icon. Colour: lime = something came in.
  const char* icon = "\xE2\x80\xA6";  // "..." while the details are on their way
  const char* title = "Checking...";
  lv_color_t color = kMuted;
  char sub[48] = "", amount[32] = "", who[16];
  char n[16], n2[16];
  formatAmount(t.amount, n, sizeof n);
  shortAddress(t.other, who, sizeof who);
  switch (t.kind) {
    case TxKind::kReceived:
      icon = SIG_SYMBOL_ARROW_DOWN, title = "Received", color = kLime;
      snprintf(amount, sizeof amount, "+%s %s", n, t.symbol);
      if (t.other[0] != '\0') snprintf(sub, sizeof sub, "from %s", who);
      break;
    case TxKind::kSent:
      icon = SIG_SYMBOL_ARROW_UP, title = "Sent", color = kText;
      snprintf(amount, sizeof amount, "-%s %s", n, t.symbol);
      if (t.other[0] != '\0') snprintf(sub, sizeof sub, "to %s", who);
      break;
    case TxKind::kSwap:
      icon = SIG_SYMBOL_EXCHANGE, title = "Swapped", color = kLime;
      snprintf(amount, sizeof amount, "+%s %s", n, t.symbol);
      formatAmount(t.outAmount, n2, sizeof n2);
      snprintf(sub, sizeof sub, "for %s %s", n2, t.outSymbol);
      break;
    case TxKind::kApp:
      icon = SIG_SYMBOL_CUBE, title = "App interaction";
      snprintf(sub, sizeof sub, "no tokens moved");
      break;
    case TxKind::kFailed:
      icon = LV_SYMBOL_CLOSE, title = "Failed", color = kOrange;
      snprintf(sub, sizeof sub, "nothing moved");
      break;
    case TxKind::kPending:
      break;
  }
  lv_obj_t* dot = rowLabel(row, icon, &gStIcon);
  lv_obj_set_style_text_color(dot, color, 0);
  lv_obj_set_size(dot, 24, 24);
  lv_obj_set_pos(dot, 8, 9);
  lv_obj_set_pos(rowLabel(row, title, &gStBold), 40, 5);
  lv_obj_set_pos(rowLabel(row, sub, &gStMuted), 40, 21);
  bool in = t.kind == TxKind::kReceived || t.kind == TxKind::kSwap;
  lv_obj_align(rowLabel(row, amount, in ? &gStUp : &gStBold), LV_ALIGN_TOP_RIGHT, -8, 5);
  char ago[16];
  formatAgo(now, t.blockTime, ago, sizeof ago);
  lv_obj_align(rowLabel(row, ago, &gStMuted), LV_ALIGN_TOP_RIGHT, -8, 21);
}

// A grey placeholder row while the first list is on its way.
void activitySkeleton(lv_obj_t* list) {
  lv_obj_t* row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_add_style(row, &gStRow, 0);
  lv_obj_set_size(row, 224, 42);
  lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  skeletonBar(row, 24, LV_ALIGN_LEFT_MID, 8);
  skeletonBar(row, 70, LV_ALIGN_LEFT_MID, 40);
  skeletonBar(row, 50, LV_ALIGN_RIGHT_MID, -8);
}

void showActivity() {
  gSkeletonShown = false;
  initRowStyles();
  lv_obj_t* scr = newDashScreen(kScrActivity);
  statusBar(scr);
  text(scr, "Activity", &inter_16_semibold, kText, 16, 30);
  lv_obj_t* list = box(scr, 8, 58, 224, 190, kGround, 0);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 2, 0);
  time_t now = time(nullptr);
  if (gHaveAct && gAct->count > 0) {
    for (int i = 0; i < gAct->count; i++) activityRow(list, gAct->items[i], (uint32_t)now);
  } else if (!gHaveAct && loading()) {
    for (int i = 0; i < 3; i++) activitySkeleton(list);
  } else {
    lv_obj_t* e = text(scr, gHaveAct ? "No transactions yet" : "Activity not loaded yet",
                       &inter_12_bold, kText, 0, 0);
    lv_obj_align(e, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t* h = text(scr,
                       gHaveAct ? "They show here within a couple of minutes."
                                : "It loads after the balances.",
                       &inter_12, kMuted, 0, 0, 200);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 130);
  }
  gStatus = text(scr, "", &inter_12, kMuted, 16, 250, 150);
  lv_label_set_long_mode(gStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_height(gStatus, 15);
  updateStatus();
  tabBar(scr, kScrActivity);
  loadAndDropOld(scr);
}

// ---- Market (design "Market" + "Heatmap") -------------------------------------------------

// One coin of the watchlist: logo and name, its 24 h move, and the price in big type.
void marketCard(lv_obj_t* scr, int x, int y, const Holding& h, int index) {
  lv_obj_t* card = lv_obj_create(scr);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &gStRow, 0);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, 108, 60);
  lv_obj_remove_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  tappable(card, kWatchTap + index);
  // Name and move stacked next to the logo, price under them: side by side they collide
  // in a 108 px card ("BONK" + "▼ 3.2%").
  tokenBadge(card, h.symbol, 8, 8);
  lv_obj_set_pos(rowLabel(card, h.symbol, &gStBold), 38, 4);
  if (!h.priced && loading()) {  // no price yet, but one is on its way
    skeletonBar(card, 64, LV_ALIGN_BOTTOM_LEFT, 8);
    lv_obj_align(lv_obj_get_child(card, -1), LV_ALIGN_BOTTOM_LEFT, 8, -13);  // where the price goes
    return;
  }
  char s[24];
  bool up = true;
  if (h.hasChange) {
    formatPercent(h.change24h, s, sizeof s, &up);
    lv_obj_set_pos(rowLabel(card, s, up ? &gStUp : &gStDown), 38, 19);
  }
  if (h.priced) formatUsd(h.usdPrice, s, sizeof s);
  else snprintf(s, sizeof s, "-");
  lv_obj_t* price = text(card, s, &inter_16_semibold, kText, 0, 0);
  lv_obj_align(price, LV_ALIGN_BOTTOM_LEFT, 8, -3);
}

void showMarket() {
  gSkeletonShown = false;
  initRowStyles();
  lv_obj_t* scr = newDashScreen(kScrMarket);
  statusBar(scr);
  text(scr, "Market", &inter_16_semibold, kText, 16, 30);
  // "Heatmap >" opens your portfolio as tiles. A button bigger than its words, for a finger.
  lv_obj_t* hm = lv_button_create(scr);
  lv_obj_set_size(hm, 96, 30);
  lv_obj_align(hm, LV_ALIGN_TOP_RIGHT, -4, 26);
  lv_obj_set_style_bg_opa(hm, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(hm, 0, 0);
  lv_obj_set_style_pad_all(hm, 0, 0);
  lv_obj_t* hl = text(hm, "Heatmap " LV_SYMBOL_RIGHT, &inter_12_bold, kLime, 0, 0);
  lv_obj_align(hl, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_add_event_cb(hm, [](lv_event_t*) { gNav = kScrHeatmap; }, LV_EVENT_CLICKED, nullptr);
  // The grid shows even before the first answer: names right away, prices as skeletons.
  if (!gHavePf) portfolioInitWatch(&gPf);
  for (int i = 0; i < kWatchCount; i++) {
    marketCard(scr, i % 2 == 0 ? 8 : 124, 60 + (i / 2) * 64, gPf.watch[i], i);
  }
  gStatus = text(scr, "", &inter_12, kMuted, 16, 254, 150);
  lv_label_set_long_mode(gStatus, LV_LABEL_LONG_DOT);
  lv_obj_set_height(gStatus, 15);
  updateStatus();
  tabBar(scr, kScrMarket);
  loadAndDropOld(scr);
}

void onBackMarket(lv_event_t*) { gNav = kScrMarket; }

// A tile's colour: lime for up, orange for down, stronger for a bigger move (full colour
// from 2%), grey for no move. lv_color_mix(a, b, ratio) blends a into b, 255 = all a.
lv_color_t moveColour(const Tile& t) {
  float m = t.hasChange ? t.change24h : 0;
  float strength = m < 0 ? -m / 2.0f : m / 2.0f;
  if (strength < 0.05f) return kTrack;
  if (strength > 1) strength = 1;
  uint8_t ratio = (uint8_t)(130 + strength * 125);  // never paler than about half
  return lv_color_mix(m > 0 ? kLime : kOrange, kTrack, ratio);
}

void showHeatmap() {
  lv_obj_t* scr = newDashScreen(kScrHeatmap);
  backHeader(scr, "Portfolio heatmap", onBackMarket);
  constexpr int kX = 8, kY = 44, kW = 224, kH = 236, kGap = 4;
  Tile tiles[kMaxTiles];
  int n = gHavePf ? heatmapLayout(gPf, kW + kGap, kH + kGap, tiles) : 0;
  if (n == 0) {
    lv_obj_t* e = text(scr, gHavePf ? "Nothing with a price yet" : "Loading your tokens...",
                       &inter_12_bold, kText, 0, 0);
    lv_obj_align(e, LV_ALIGN_TOP_MID, 0, 140);
  }
  for (int i = 0; i < n; i++) {
    const Tile& t = tiles[i];
    // The layout box is one gap bigger than the screen area; each tile gives up one gap on
    // its right and bottom, so the gaps between tiles are all equal and the edges line up.
    lv_color_t bg = moveColour(t);
    lv_color_t ink = bg.red + bg.green > 300 ? kGround : kText;  // dark text on light tiles
    lv_obj_t* tile = box(scr, kX + t.x, kY + t.y, t.w - kGap, t.h - kGap, bg, 8);
	if (t.item >= 0) tappable(tile, kHeatTap + t.item);
    char name[16], line[40], pct[16];
    bool up = true;
    if (t.item >= 0) snprintf(name, sizeof name, "%s", gPf.items[t.item].symbol);
    else snprintf(name, sizeof name, "+%u more", t.moreCount);
    if (t.hasChange) formatPercent(t.change24h, pct, sizeof pct, &up);
    else snprintf(pct, sizeof pct, "-");
    snprintf(line, sizeof line, "%.0f%% \xC2\xB7 %s", t.share * 100, pct);
    bool big = t.w > 110 && t.h > 90;
    int tw = t.w - kGap;
    if (tw < 50 || t.h - kGap < 34) {  // too small for words: the name only, if it fits
      if (t.item < 0) snprintf(name, sizeof name, "+%u", t.moreCount);  // "+3": fits 18 px
      if (tw >= (t.item < 0 ? 18 : 30) && t.h - kGap >= 18) {
        lv_obj_t* l = text(tile, name, &inter_12_bold, ink, 0, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 4, -2);
      }
      continue;
    }
    lv_obj_t* nl = text(tile, name, big ? &inter_28_bold : &inter_12_bold, ink, 0, 0);
    lv_obj_t* ll = text(tile, line, &inter_12, ink, 0, 0, tw - 16);
    lv_obj_align(ll, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_align_to(nl, ll, LV_ALIGN_OUT_TOP_LEFT, 0, big ? 0 : -2);
  }
  text(scr, "Size = share    Colour = 24h move", &inter_12, kMuted, 16, 290);
  loadAndDropOld(scr);
}

// ---- Token detail (design "TokenDetail") --------------------------------------------------

// The full name under the symbol, for the coins we know. Like symbols, names come from our
// own list, never from the token (a scam token can call itself anything).
const char* tokenName(const char* symbol) {
  static const char* const kNames[][2] = {
      {"SOL", "Solana"},     {"USDC", "USD Coin"},  {"USDT", "Tether"},
      {"JUP", "Jupiter"},    {"BONK", "Bonk"},      {"WIF", "dogwifhat"},
      {"BTC", "Coinbase BTC"}, {"ETH", "Ether (Wormhole)"}, {"PYTH", "Pyth"},
  };
  for (const auto& n : kNames) {
    if (strcmp(n[0], symbol) == 0) return n[1];
  }
  return "";
}

void onBackDetail(lv_event_t*) { gNav = gDetail.from; }

// The 1D / 1W / 1M pills under the chart. user_data = the range.
void onRange(lv_event_t* e) {
  gRange = (ChartRange)(intptr_t)lv_event_get_user_data(e);
  gChartAsked = false;
  gNav = kScrToken;  // draw again: the new range's chart is asked for there
}

void rangePills(lv_obj_t* scr, int y) {
  static const char* const kLabels[] = {"1D", "1W", "1M"};
  for (int i = 0; i < 3; i++) {
    bool on = (int)gRange == i;
    lv_obj_t* b = lv_button_create(scr);
    lv_obj_set_pos(b, 12 + i * 48, y);
    lv_obj_set_size(b, 40, 24);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, kTrack, 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_center(text(b, kLabels[i], &inter_12_bold, on ? kText : kMuted, 0, 0));
    lv_obj_add_event_cb(b, onRange, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

// The price line, drawn with lv_line: a list of (x, y) points joined by straight lines.
// LVGL keeps a pointer to the points instead of copying them, so they must outlive the
// screen: they live in one array on the heap, made once.
void priceLine(lv_obj_t* scr, const Chart& c, int x, int y, int w, int h) {
  static lv_point_precise_t* pts = new lv_point_precise_t[kChartPoints];
  static lv_point_precise_t base[2];
  float lo = c.close[0], hi = c.close[0];
  for (int i = 1; i < c.count; i++) {
    if (c.close[i] < lo) lo = c.close[i];
    if (c.close[i] > hi) hi = c.close[i];
  }
  if (hi - lo < hi * 0.001f) {  // a flat line (a stablecoin): keep it in the middle
    lo -= hi * 0.001f;
    hi += hi * 0.001f;
  }
  // Price to pixel: the highest price at the top (y = 0), the lowest at the bottom (y = h).
  auto yOf = [&](float price) { return (int32_t)((hi - price) / (hi - lo) * (h - 4)) + 2; };
  for (int i = 0; i < c.count; i++) {
    pts[i].x = i * (w - 1) / (c.count - 1);
    pts[i].y = yOf(c.close[i]);
  }
  bool up = c.close[c.count - 1] >= c.close[0];
  // A dashed line at the starting price: above it = up over this range, below = down.
  base[0] = {0, yOf(c.close[0])};
  base[1] = {w - 1, yOf(c.close[0])};
  lv_obj_t* dash = lv_line_create(scr);
  lv_line_set_points(dash, base, 2);
  lv_obj_set_pos(dash, x, y);
  lv_obj_set_style_line_color(dash, kTrack, 0);
  lv_obj_set_style_line_width(dash, 1, 0);
  lv_obj_set_style_line_dash_width(dash, 3, 0);
  lv_obj_set_style_line_dash_gap(dash, 3, 0);
  lv_obj_t* line = lv_line_create(scr);
  lv_line_set_points(line, pts, c.count);
  lv_obj_set_pos(line, x, y);
  lv_obj_set_style_line_color(line, up ? kLime : kOrange, 0);
  lv_obj_set_style_line_width(line, 2, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
}

// The chart area: the line, or why there isn't one. Asks the task for the chart once.
void chartArea(lv_obj_t* scr, const Holding* h, int x, int y, int w, int ht) {
  auto note = [&](const char* msg) {
    lv_obj_t* l = text(scr, msg, &inter_12, kMuted, 0, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y + ht / 2 - 8);
  };
  if (h == nullptr || !h->priced) {
    note("No price history");
    return;
  }
  bool mine = gChartVersion > 0 && strcmp(gChart->mint, gDetail.mint) == 0 &&
              gChart->range == gRange;
  if (mine && gChart->ok) {
    priceLine(scr, *gChart, x, y, w, ht);
  } else if (mine) {
    note("No chart for this token");
  } else {
    if (!gChartAsked) {
      dashNetRequestChart(gDetail.mint, gRange);
      gChartAsked = true;
    }
    if (dashNetState() == NetState::kNoWifi) {
      note("The chart needs Wi-Fi");
    } else {
      lv_obj_t* area = box(scr, x, y, w, ht, kGround, 0);
      skeletonBar(area, 120, LV_ALIGN_CENTER, 0);
    }
  }
}

void showToken() {
  initRowStyles();
  lv_obj_t* scr = newDashScreen(kScrToken);
  // The header: back arrow, logo, symbol and name ("< (o) SOL Solana").
  backHeader(scr, "", onBackDetail);
  tokenBadge(scr, gDetail.symbol, 48, 10);
  lv_obj_t* sym = text(scr, gDetail.symbol, &inter_16_semibold, kText, 80, 11);
  lv_obj_t* name = text(scr, tokenName(gDetail.symbol), &inter_12, kMuted, 0, 0);
  lv_obj_align_to(name, sym, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -2);

  // The numbers come from the latest refresh: your holding if you have one, else the
  // Market watchlist's entry (a coin you watch but don't hold).
  const Holding* held = gHavePf ? portfolioFindHeld(gPf, gDetail.mint) : nullptr;
  const Holding* h = held != nullptr ? held : portfolioFindWatch(gPf, gDetail.mint);

  char s[48];
  bool up = true;
  if (h != nullptr && h->priced) formatUsd(h->usdPrice, s, sizeof s);
  else snprintf(s, sizeof s, "No price");
  text(scr, s, &inter_28_bold, h != nullptr && h->priced ? kText : kMuted, 16, 44);
  if (h != nullptr && h->priced && h->hasChange) {
    // Yesterday's price from today's and the change: today = yesterday x (1 + change).
    double before = h->usdPrice / (1 + h->change24h / 100.0);
    formatChange(h->usdPrice, before, s, sizeof s, &up);
    strncat(s, " \xC2\xB7 24h", sizeof s - strlen(s) - 1);
    text(scr, s, &inter_12_bold, up ? kLime : kOrange, 16, 82);
  }

  chartArea(scr, h, 12, 104, 216, 72);
  rangePills(scr, 182);

  // Your holding: amount, value, and its share of your net worth.
  lv_obj_t* card = box(scr, 8, 214, 224, 58, kCard, 8);
  if (held != nullptr) {
    text(card, "You hold", &inter_12, kMuted, 12, 8);
    formatAmount(held->amount, s, sizeof s);
    strncat(s, " ", sizeof s - strlen(s) - 1);
    strncat(s, held->symbol, sizeof s - strlen(s) - 1);
    text(card, s, &inter_16_semibold, kText, 12, 26);
    if (held->priced) {
      double value = held->amount * held->usdPrice;
      formatUsd(value, s, sizeof s);
      lv_obj_t* v = text(card, s, &inter_16_semibold, kText, 0, 0);
      lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -12, 26);
      if (gPf.totalUsd > 0) {
        snprintf(s, sizeof s, "%.0f%% of total", value / gPf.totalUsd * 100);
        lv_obj_t* share = text(card, s, &inter_12, kMuted, 0, 0);
        lv_obj_align(share, LV_ALIGN_TOP_RIGHT, -12, 8);
      }
    }
  } else {
    text(card, "You hold", &inter_12, kMuted, 12, 8);
    text(card, "None", &inter_16_semibold, kMuted, 12, 26);
  }
  // Where it lives on Solana: the honest way to tell a real token from a look-alike.
  if (gDetail.mint[0] == '\0' || strcmp(gDetail.mint, kWrappedSolMint) == 0) {
    snprintf(s, sizeof s, "Solana's own coin \xC2\xB7 prices: Jupiter, GeckoTerminal");
  } else {
    char shortMint[16];
    shortAddress(gDetail.mint, shortMint, sizeof shortMint);
    snprintf(s, sizeof s, "Mint %s \xC2\xB7 Jupiter, GeckoTerminal", shortMint);
  }
  text(scr, s, &inter_12, kMuted, 16, 282, 208);
  loadAndDropOld(scr);
}

// ---- Receive (a tab) and Settings (a sub-screen with a back arrow) ------------------------

void onBackHome(lv_event_t*) { gNav = kScrHome; }

void showReceive() {
  lv_obj_t* scr = newDashScreen(kScrReceive);
  statusBar(scr);
  // A smaller QR (108 px, still about 3.5 px per square) so the address and the hint fit
  // above the tab bar.
  addressBlock(scr, gAddress, 30, 108);
  tabBar(scr, kScrReceive);
  loadAndDropOld(scr);
}

void onSetupAgain(lv_event_t*) { gSetupTapped = true; }

void showSettings() {
  lv_obj_t* scr = newDashScreen(kScrSettings);
  backHeader(scr, "Settings", onBackHome);
  settingsRow(scr, 48, "Network", "Devnet");
  // The Wi-Fi row is a button: it opens the setup network again.
  lv_obj_t* wifi = lv_button_create(scr);
  lv_obj_set_pos(wifi, 8, 94);
  lv_obj_set_size(wifi, 224, 44);
  lv_obj_set_style_bg_color(wifi, kCard, 0);
  lv_obj_set_style_radius(wifi, 8, 0);
  lv_obj_set_style_shadow_width(wifi, 0, 0);
  lv_obj_set_style_pad_all(wifi, 0, 0);
  lv_obj_add_event_cb(wifi, onSetupAgain, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* l = text(wifi, "Wi-Fi and data", &inter_12_bold, kText, 0, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t* v = text(wifi, "Set up again " LV_SYMBOL_RIGHT, &inter_12, kMuted, 0, 0);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
  settingsRow(scr, 140, "Balances from", gSetup.rpcUrl[0] ? "Your RPC" : "Public Solana RPC");
  brightnessRow(scr, 186);
  lv_obj_t* note = text(scr, "Prices: Jupiter. Devnet SOL is shown at the real SOL price.",
                        &inter_12, kMuted, 16, 266, 208);
  (void)note;
  loadAndDropOld(scr);
}

// ---- Setup network and first-start screens -----------------------------------------------

void showSetup() {
  static char ssid[16], pass[12];  // kept: the screen can be drawn again while it's open
  if (!gPortalOpen) {
    dashPortalBegin(ssid, pass);
    gPortalOpen = true;
  }
  lv_obj_t* scr = newDashScreen(kScrSetup);
  statusBar(scr);
  text(scr, "Set up Wi-Fi", &inter_16_semibold, kText, 16, 30);
  text(scr, "1. On your phone, scan this or join the Wi-Fi below.", &inter_12, kMuted, 16, 54, 208);

  char wifiQr[64];  // the standard "join this Wi-Fi" QR that phone cameras understand
  snprintf(wifiQr, sizeof wifiQr, "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
  lv_obj_t* frame = box(scr, 16, 88, 104, 104, kText, 6);
  lv_obj_t* qr = lv_qrcode_create(frame);
  lv_qrcode_set_size(qr, 92);
  lv_qrcode_set_dark_color(qr, kGround);
  lv_qrcode_set_light_color(qr, kText);
  lv_qrcode_update(qr, wifiQr, strlen(wifiQr));
  lv_obj_center(qr);
  text(scr, "Wi-Fi", &inter_12, kMuted, 130, 92);
  text(scr, ssid, &inter_12_bold, kText, 130, 108);
  text(scr, "Password", &inter_12, kMuted, 130, 134);
  text(scr, pass, &inter_12_bold, kLime, 130, 150);
  text(scr, "2. The setup page opens by itself. If it doesn't, open:", &inter_12, kMuted, 16,
       202, 150);
  text(scr, "192.168.4.1", &inter_12_bold, kText, 16, 236);
  sendCorner(scr);  // signing still works without Wi-Fi
  loadAndDropOld(scr);
}

void showNoAddress() {
  lv_obj_t* scr = newDashScreen(kScrNoAddress);
  statusBar(scr);
  lv_obj_t* t = text(scr, "Unlock the Signer once", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_t* b = text(scr,
                     "The Dashboard shows the address the Signer saves when you unlock it. "
                     "Tap Send, enter your PIN, and Sigly comes back here by itself after "
                     "2 minutes (or press BOOT).",
                     &inter_12, kMuted, 0, 0, 200);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 128);
  sendCorner(scr);
  loadAndDropOld(scr);
}

// Shown instead of starting Wi-Fi when the last run ended in a crash, so a bug in the
// online part can never trap the board in a restart loop: one crash, then it waits for you.
void showCrashed() {
  lv_obj_t* scr = newDashScreen(kScrNone);
  statusBar(scr);
  lv_obj_t* t = text(scr, LV_SYMBOL_WARNING " Sigly restarted after a crash", &inter_16_semibold,
                     kAmber, 0, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_t* b = text(scr,
                     "Wi-Fi stays off until you tap Try again. Your keys are safe: the "
                     "Dashboard never unlocks them. Send still works.",
                     &inter_12, kMuted, 0, 0, 220);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 104);
  pillButton(scr, 50, 168, 120, 40, "Try again",
             [](lv_event_t*) { gRetryTapped = true; });
  sendCorner(scr);
  loadAndDropOld(scr);
}

void showStartingSigner() {
  lv_obj_t* scr = newDashScreen(kScrNone);
  lv_obj_t* t = text(scr, "Starting the Signer...", &inter_16_semibold, kText, 0, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -10);
  lv_obj_t* s = text(scr, "Wi-Fi off, Bluetooth on", &inter_12, kMuted, 0, 0);
  lv_obj_align(s, LV_ALIGN_CENTER, 0, 16);
  loadAndDropOld(scr);
}

void go(Screen s) {
  switch (s) {
    case kScrHome: showHome(); break;
    case kScrActivity: showActivity(); break;
    case kScrMarket: showMarket(); break;
    case kScrHeatmap: showHeatmap(); break;
    case kScrToken: showToken(); break;
    case kScrReceive: showReceive(); break;
    case kScrSettings: showSettings(); break;
    case kScrSetup: showSetup(); break;
    case kScrNoAddress: showNoAddress(); break;
    default: break;
  }
}

}  // namespace

void dashUiBegin() {
  gHaveSetup = dashLoadSetup(&gSetup);
  if (!dashAddress(gAddress)) {
    Serial.println("dash: no address yet (unlock the Signer once)");
    showNoAddress();  // no Wi-Fi either: there is nothing to look up
    return;
  }
  Serial.printf("dash: account 1 = %s\n", gAddress);
  gHavePf = dashLoadPortfolio(&gPf, &gPfAt);  // last numbers, shown until fresh ones arrive
  gAct = new Activity();
  gChart = new Chart();
  uint32_t actAt = 0;
  gHaveAct = dashLoadActivity(gAct, &actAt);
  if (!gHaveSetup || dashTakeSetupRequest()) {
    showSetup();
    return;
  }
  esp_reset_reason_t why = esp_reset_reason();
  if (why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT ||
      why == ESP_RST_WDT) {
    Serial.printf("dash: last run crashed (reset reason %d), Wi-Fi stays off\n", (int)why);
    showCrashed();
    return;
  }
  dashNetBegin(gAddress);
  showHome();
}

void dashUiService() {
  if (gSendTapped) {
    gSendTapped = false;
    showStartingSigner();
    lv_refr_now(nullptr);  // draw it now: the restart comes before the next frame
    restartInto(SiglyMode::kSigner);
  }
  if (gSetupTapped) {
    gSetupTapped = false;
    // Wi-Fi is busy refreshing; the clean way over is a restart straight into the setup
    // screen. The old settings stay until the form is saved, so a restart without saving
    // brings the Dashboard back as it was.
    dashRequestSetup();
    restartInto(SiglyMode::kDashboard);
  }
  if (gRetryTapped) {
    gRetryTapped = false;
    // A normal restart (not a crash), so the next start goes online as usual.
    restartInto(SiglyMode::kDashboard);
  }
  if (gPortalOpen && dashPortalService()) {
    delay(1500);  // let the "Saved" page reach the phone
    restartInto(SiglyMode::kDashboard);
  }
  if (gNav != kScrNone) {
    Screen s = gNav;
    gNav = kScrNone;
    go(s);
  }

  uint32_t v = dashNetTakePortfolio(gPfVersion, &gPf, &gPfAt);
  if (v != gPfVersion) {
    gPfVersion = v;
    gHavePf = true;
    if (gShown == kScrHome || gShown == kScrMarket || gShown == kScrHeatmap ||
        gShown == kScrToken) {
      go(gShown);  // redraw with new numbers
    }
  }
  if (gAct != nullptr) {
    uint32_t av = dashNetTakeActivity(gActVersion, gAct);
    if (av != gActVersion) {
      gActVersion = av;
      gHaveAct = true;
      if (gShown == kScrActivity) go(kScrActivity);
    }
  }
  if (gChart != nullptr) {
    uint32_t cv = dashNetTakeChart(gChartVersion, gChart);
    if (cv != gChartVersion) {
      gChartVersion = cv;
      if (gShown == kScrToken) go(kScrToken);
    }
  }
  if (dashNetState() != gShownState) {
    updateStatus();
    // Loading ended without an answer (an error): swap the skeletons for "-".
    if (gSkeletonShown && !loading() && (gShown == kScrMarket || gShown == kScrActivity)) {
      go(gShown);
    }
  }
  if (millis() - gLastClock > 5000) {
    gLastClock = millis();
    updateClock();
  }
  lv_timer_handler();
}
#endif  // BLEND_CYD
