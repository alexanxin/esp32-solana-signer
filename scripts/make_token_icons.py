#!/usr/bin/env python3
"""Rebuilds src/icons/token_icons.c: small round token logos for the Dashboard's token rows.

The logos come from Jupiter's token list (the same place wallets get them), are shrunk to
24 x 24 pixels, cut into a circle, and stored as LVGL images (RGB565 colours + an 8-bit
alpha plane = 1,728 bytes each). They are built into the firmware instead of downloaded by
the board: a logo can be a PNG, WebP or SVG on any website, and decoding those needs far
more RAM than the CYD has. Tokens without a logo here keep their round first-letter badge.

Needs Python 3 with Pillow (pip install pillow) and internet access.
Usage: python3 scripts/make_token_icons.py
"""
import io
import json
import os
import sys
import urllib.request

from PIL import Image, ImageDraw

SIZE = 24
# Symbol shown on the board -> mainnet mint whose logo to use. Keep in step with kKnown and
# kWatch in src/portfolio.cpp (devnet USDC shows as "USDC" and SOL as "SOL", so they share).
TOKENS = [
    ("SOL", "So11111111111111111111111111111111111111112"),
    ("USDC", "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
    ("USDT", "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"),
    ("JUP", "JUPyiwrYJFskUPiHa7hkeR8VUtAeFoSYbKedZNsDvCN"),
    ("BONK", "DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263"),
    ("PYTH", "HZ1JovNiVvGrGNiiYvEozEVgZ58xaU3RKwX8eACQBCt3"),
    ("JitoSOL", "J1toso1uCk3RLmjorhTtrVwY9HJ7X8V9yYac6Y7kGCPn"),
    ("mSOL", "mSoLzYCxHdYgdzU16g5QSh3i5K3z3KZK7ytfqcJm7So"),
    ("bSOL", "bSo13r4TkiE4KumL71LsHTPpL2euBYLFx6h9HP3piy1"),
    ("WIF", "EKpQGSJtjMFqKZ9KQanSqYXRcF8fBopzLHYxdM65zcjm"),
    ("RAY", "4k3Dyjzvzp8eMZWUXbBCjEvwSkkk59S5iCNLY3QrkX6R"),
    ("JTO", "jtojtomepa8beP8AuQc6eXt5FriJwfFMwQx2v2f9mCL"),
    ("BTC", "cbbtcf3aa214zXHbiAZQwf4122FBYbraNdFqgw4iMij"),
]
UA = {"User-Agent": "sigly-icon-builder", "Accept": "*/*"}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=20) as r:
        return r.read()


def logo_url(mint):
    hits = json.loads(get("https://lite-api.jup.ag/tokens/v2/search?query=" + mint))
    for t in hits:
        if t.get("id") == mint and t.get("icon"):
            return t["icon"]
    raise RuntimeError("Jupiter has no icon for " + mint)


def to_icon(png_bytes):
    """Shrinks a logo to SIZE x SIZE and cuts it into a circle (4x oversampled edge)."""
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    big = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(big).ellipse((0, 0, SIZE * 4 - 1, SIZE * 4 - 1), fill=255)
    circle = big.resize((SIZE, SIZE), Image.LANCZOS)
    alpha = Image.composite(img.getchannel("A"), Image.new("L", (SIZE, SIZE), 0), circle)
    img.putalpha(alpha)
    return img


def rgb565a8(img):
    """LVGL's RGB565A8 layout: all pixels as little-endian RGB565, then all alpha bytes."""
    colours, alphas = bytearray(), bytearray()
    for r, g, b, a in img.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        colours += bytes((v & 0xFF, v >> 8))
        alphas.append(a)
    return bytes(colours + alphas)


def c_name(symbol):
    return "icon_" + symbol.lower()


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "icons",
                       "token_icons.c")
    parts = [
        "// Made by scripts/make_token_icons.py from Jupiter's token logos. Don't edit by hand.\n"
        "// Each logo: 24 x 24, LVGL RGB565A8 (colours, then an alpha plane), 1,728 bytes.\n"
        "#ifdef BLEND_CYD\n#include <string.h>\n\n#include \"token_icons.h\"\n\n"
    ]
    table = []
    for symbol, mint in TOKENS:
        url = logo_url(mint)
        print(f"{symbol:8} {url}", file=sys.stderr)
        data = rgb565a8(to_icon(get(url)))
        name = c_name(symbol)
        rows = ",\n".join(
            "  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16))
        parts.append(
            f"static const uint8_t {name}_data[] = {{\n{rows}\n}};\n"
            f"static const lv_image_dsc_t {name} = {{\n"
            f"  .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,\n"
            f"             .flags = 0, .w = {SIZE}, .h = {SIZE}, .stride = {SIZE * 2}}},\n"
            f"  .data_size = sizeof({name}_data),\n  .data = {name}_data,\n}};\n\n")
        table.append(f"    {{\"{symbol}\", &{name}}},\n")
    parts.append(
        "static const struct {\n  const char* symbol;\n  const lv_image_dsc_t* icon;\n} kIcons[] = {\n"
        + "".join(table) + "};\n\n"
        "const lv_image_dsc_t* tokenIcon(const char* symbol) {\n"
        "  for (size_t i = 0; i < sizeof kIcons / sizeof kIcons[0]; i++) {\n"
        "    if (strcmp(kIcons[i].symbol, symbol) == 0) return kIcons[i].icon;\n"
        "  }\n  return NULL;\n}\n#endif  // BLEND_CYD\n")
    with open(out, "w") as f:
        f.write("".join(parts))
    print(f"wrote {os.path.normpath(out)} ({len(TOKENS)} icons)", file=sys.stderr)


if __name__ == "__main__":
    main()
