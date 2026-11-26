#!/bin/sh
# Rebuilds src/fonts/*.c (LVGL bitmap fonts) from the Inter typeface (SIL OFL, see
# src/fonts/Inter-LICENSE.txt). Needs Node: npx downloads lv_font_conv on first run.
# Usage: scripts/make_fonts.sh /path/to/Inter-4.0/extras/ttf
set -e
TTF=${1:?path to Inter extras/ttf folder}
RANGE="0x20-0x7E,0xB7,0x2026,0x2248"  # ASCII, middle dot, ellipsis, "approximately"
gen() {  # file size name
  npx -y lv_font_conv@1.5.2 --no-compress --bpp 4 --format lvgl --lv-include lvgl.h \
    --size "$2" --font "$TTF/$1" -r "$RANGE" -o "src/fonts/$3.c"
}
gen Inter-Regular.ttf 12 inter_12
gen Inter-Bold.ttf 12 inter_12_bold
gen Inter-SemiBold.ttf 16 inter_16_semibold
gen Inter-Bold.ttf 28 inter_28_bold
