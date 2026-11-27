#!/usr/bin/env python3
"""Rebuilds src/bip39_english.h from the official BIP39 English list.

    curl -O https://raw.githubusercontent.com/bitcoin/bips/master/bip-0039/english.txt
    python3 scripts/make_wordlist.py english.txt

Refuses to write anything if the file is not exactly the official one.
"""
import hashlib
import sys

OFFICIAL_SHA256 = "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda"

data = open(sys.argv[1], "rb").read()
if hashlib.sha256(data).hexdigest() != OFFICIAL_SHA256:
    sys.exit("not the official BIP39 English list (sha256 differs)")
words = data.decode().split()
assert len(words) == 2048 and words == sorted(words)

out = [
    "// The official BIP39 English word list (2048 words, sorted), from",
    "// https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt",
    "// sha256 of that file: " + OFFICIAL_SHA256,
    "// Made by scripts/make_wordlist.py. Do not edit by hand: one changed letter changes everyone's keys.",
    "//",
    "// Each word gets 9 bytes (8 letters at most + the closing zero), so the list is one flat",
    "// block of 18 KB that stays in flash, with no pointers to fix up.",
    "#pragma once",
    "",
    "static const char kBip39Words[2048][9] = {",
]
line = "   "
for w in words:
    item = ' "%s",' % w
    if len(line) + len(item) > 96:
        out.append(line)
        line = "   "
    line += item
out.append(line)
out.append("};")
open("src/bip39_english.h", "w").write("\n".join(out) + "\n")
print("wrote src/bip39_english.h")
