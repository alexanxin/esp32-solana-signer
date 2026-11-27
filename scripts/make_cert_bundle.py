#!/usr/bin/env python3
"""Rebuilds src/cert_bundle.h: the web's root certificates in the ESP32 "certificate bundle"
format, so the Dashboard can check who it is talking to over HTTPS.

Why: HTTPS only protects you if the board checks the server's certificate chain ends at a
root it trusts. Without roots, anyone on the Wi-Fi could pretend to be the RPC and show you
fake balances. The roots come from Mozilla's list (the same one Firefox uses), via the
Python "certifi" package.

Format (what WiFiClientSecure's esp_crt_bundle.c reads): 2 bytes number of certificates, then
per certificate: 2 bytes subject length, 2 bytes public-key length, the subject (DER), the
public key (DER). Sorted by subject, because the board finds the issuer by binary search.
Only the subject and key are kept, not whole certificates: that's all a root is needed for.

Usage: pip install certifi cryptography && python3 scripts/make_cert_bundle.py
"""
import datetime
import os
import struct

import certifi
from cryptography import x509
from cryptography.hazmat.primitives import serialization

pem = open(certifi.where(), "rb").read()
# Plus a few retired roots that big sites still chain up to (see that file for why).
pem += open(os.path.join(os.path.dirname(__file__), "extra_roots.pem"), "rb").read()
now = datetime.datetime.now(datetime.timezone.utc)
entries = {}
for cert in x509.load_pem_x509_certificates(pem):
    if cert.not_valid_after_utc < now:
        continue  # expired roots can't sign anything valid
    name = cert.subject.public_bytes()
    key = cert.public_key().public_bytes(serialization.Encoding.DER,
                                         serialization.PublicFormat.SubjectPublicKeyInfo)
    entries[name] = key  # same subject twice: keep one (the board can only match one)

blob = struct.pack(">H", len(entries))
for name in sorted(entries):
    key = entries[name]
    blob += struct.pack(">HH", len(name), len(key)) + name + key

out = os.path.join(os.path.dirname(__file__), "..", "src", "cert_bundle.h")
with open(out, "w") as f:
    f.write("// Made by scripts/make_cert_bundle.py from Mozilla's root list (certifi %s)\n"
            "// plus scripts/extra_roots.pem.\n" % certifi.__version__)
    f.write("// %d roots, %d bytes. Don't edit: run the script again.\n" % (len(entries), len(blob)))
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write("static const uint8_t kCertBundle[] = {\n")
    for i in range(0, len(blob), 16):
        f.write("    " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ",\n")
    f.write("};\n")
print("%d roots, %d bytes -> %s" % (len(entries), len(blob), os.path.normpath(out)))
