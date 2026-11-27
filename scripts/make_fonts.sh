#!/bin/sh
# Rebuilds src/fonts/*.c (LVGL bitmap fonts) from the Inter typeface (SIL OFL, see
# src/fonts/Inter-LICENSE.txt) plus a handful of icons from the Font Awesome 5 file that ships
# with LVGL (SIL OFL / CC BY 4.0; LVGL's own Montserrat fonts carry the same icons).
# Putting the icons inside the Inter fonts lets lv_conf.h switch Montserrat off (~60 KB).
# Needs Node: npx downloads lv_font_conv on first run.
# Usage: scripts/make_fonts.sh /path/to/Inter-4.0/extras/ttf /path/to/FontAwesome5-Solid+Brands+Regular.woff
#   (the .woff is lvgl/scripts/built_in_font/ in the LVGL 9.2.2 source)
set -e
TTF=$(cd "${1:?path to Inter extras/ttf folder}" && pwd)
FA=$(cd "$(dirname "${2:?path to LVGL FontAwesome5 .woff}")" && pwd)/$(basename "$2")
OUT=$(pwd)/src/fonts
RANGE="0x20-0x7E,0xB7,0x2026,0x2248,0x25B2,0x25BC"  # ASCII, middle dot, ellipsis, "approximately", up/down triangles

# Icons, by the code LVGL gives them in lv_symbol_def.h (LV_SYMBOL_...):
# OK F00C, CLOSE F00D, LIST F00B, SETTINGS F013, HOME F015, DOWNLOAD F019, REFRESH F021,
# LEFT F053, RIGHT F054, EYE_OPEN F06E, EYE_CLOSE F070, WARNING F071, UP F077, DOWN F078,
# UPLOAD F093, BLUETOOTH F293, BACKSPACE F55A, WIFI F1EB, LOCK F023.
# Font Awesome icons LVGL has no LV_SYMBOL_ for (ui_kit.h names them SIG_SYMBOL_...):
# QRCODE F029, PAPER_PLANE F1D8, COINS F51E, ARROW_UP F062, ARROW_DOWN F063, CUBE F1B2,
# EXCHANGE F362 (Activity: received, sent, app, swap)
ICONS_SMALL="0xF00C,0xF00D,0xF023,0xF054,0xF071,0xF1EB,0xF293"
ICONS_FULL="0xF00B,0xF00C,0xF00D,0xF013,0xF015,0xF019,0xF021,0xF023,0xF029,0xF053,0xF054,0xF06E,0xF070,0xF071,0xF077,0xF078,0xF093,0xF1D8,0xF1EB,0xF293,0xF51E,0xF55A,0xF062,0xF063,0xF1B2,0xF362"

gen() {  # file size name icons
  # Run inside the font folder so the "Opts:" line in each .c file has no local paths.
  (cd "$TTF" && npx -y lv_font_conv@1.5.2 --no-compress --bpp 4 --format lvgl --lv-include lvgl.h \
    --size "$2" --font "$1" -r "$RANGE" \
    --font "$FA" -r "$4" -o "$OUT/$3.c")
  # Same path cleanup for the icon file and the output file. And make the font "const" on
  # LVGL 9 too: lv_font_conv's version check only matches LVGL 8, and lv_conf.h declares
  # inter_16_semibold as const (it is the default font), so both must agree.
  sed -i.bak -e "s|$FA|FontAwesome5-Solid+Brands+Regular.woff|" -e "s|$OUT/|src/fonts/|" "$OUT/$3.c"
  perl -0pi -e 's/#if LV_VERSION_CHECK\(8, 0, 0\)\nconst lv_font_t/#if LVGL_VERSION_MAJOR >= 8\nconst lv_font_t/' "$OUT/$3.c"
  rm -f "$OUT/$3.c.bak"
}
gen Inter-Regular.ttf 12 inter_12 "$ICONS_SMALL"
gen Inter-Bold.ttf 12 inter_12_bold "$ICONS_SMALL"
gen Inter-SemiBold.ttf 16 inter_16_semibold "$ICONS_FULL"
gen Inter-Bold.ttf 28 inter_28_bold "$ICONS_SMALL"
