<p align="center">
  <img src="docs/screens/sign-flow.png" alt="Sigly sign flow: review, hold, signing, receipt" width="100%">
</p>

# Sigly

<p align="center">
  <img src="https://img.shields.io/badge/network-devnet%20only-c4f07a?style=flat-square" alt="Devnet only">
  <img src="https://img.shields.io/badge/board-ESP32%20CYD-f5c542?style=flat-square" alt="ESP32 CYD">
  <img src="https://img.shields.io/badge/wallets-Phantom%20%7C%20Jupiter-8b5cf6?style=flat-square" alt="Phantom and Jupiter">
  <img src="https://img.shields.io/badge/UI-LVGL%209-3b82f6?style=flat-square" alt="LVGL 9">
  <img src="https://img.shields.io/badge/licence-MIT-lightgrey?style=flat-square" alt="MIT">
</p>

**A Solana hardware signer you can build for the price of a sandwich.**

Sigly is free, open-source firmware that turns a cheap ESP32 board into a Bluetooth
hardware signer. It speaks the open **Ledger Solana app protocol**, so wallets that
already support Ledger over Bluetooth work with it unchanged. Phantom and Jupiter
Mobile both connect, list your accounts and send through it today.

On the Cheap Yellow Display (a ~$10 ESP32 board with a 2.8" touchscreen) you see
exactly what you are signing, then press and hold to approve. Your phone never
holds the key.

> Not affiliated with Ledger. "Ledger" is their trademark; this project only
> implements the publicly documented protocol.

---

## Status: devnet only

**Not safe for real funds yet.** The seed is stored unencrypted in flash and there
is no backup phrase. Use devnet. Protecting the seed is the next milestone (see the
roadmap).

---

## What it does

### See what you sign

The sign screen decodes the transaction on the device, not on the phone:

- **Amount and recipient** for SOL transfers, with the full address in groups of
  four. The first and last four characters are highlighted, so you can check them
  against the address you meant to pay.
- **Network fee**, worked out from the compute budget instructions.
- **Unknown transactions** (swaps, NFTs, anything it can't read yet) get an amber
  warning, a list of the programs involved (Jupiter, Token, Token-2022, Memo and
  others are named) and a longer 3 second hold.
- **Not a transaction at all?** It tells you to reject it.

### Press and hold to approve

A tap is not enough: you hold the button and a full-screen ring fills in 1.5 s
(3 s for unknown transactions). A thin bar at the top of the screen fills too, in
case your finger covers the ring. Let go early and nothing is signed. The BOOT
button only ever rejects, never approves.

### A receipt after signing

After you sign, the device shows what went out: amount, recipient, fee and the
transaction signature (the same id you'll see in the explorer). It stays up until
you tap Done.

### More screens

<p align="center">
  <img src="docs/screens/more-screens.png" alt="Idle screen, unknown transaction, receive address" width="100%">
</p>

- **Idle:** connection status ("Phone connected" or "Waiting for your phone").
- **My address:** your account 1 address as a QR code, shown by the device itself,
  so you can check it against what the wallet app says.
- **Settings:** device name, network, account, brightness (read-only for now).

### Works with the wallets you already use

- Shows up as a Bluetooth Ledger running the Solana app.
- Derives addresses exactly like Phantom and Ledger (SLIP-10, `44'/501'/n'/0'`), so
  your accounts line up.
- Makes its own random seed on first boot from the ESP32 hardware RNG.

---

## Hardware

| Blend | Board | Approve with | Status |
|---|---|---|---|
| `cyd` | Cheap Yellow Display (ESP32-WROOM, ILI9341 2.8" touch, no PSRAM) | Touchscreen: read it, then press and hold. BOOT rejects. | Working |
| `c3` | ESP32-C3 SuperMini / DevKitM (no screen) | BOOT button (blind signing) | Not built or tested yet |

Where to get the CYD: the board used for development is [this one on Temu](https://share.temu.com/Ir7ckV5MjvB).
Any "ESP32-2432S028R" Cheap Yellow Display should work; if the screen stays dark, check the
backlight pin (GPIO 27 here) in `platformio.ini`.

Both are Bluetooth-only: the classic ESP32 has no native USB and the C3's USB is a
fixed serial/JTAG port, so browser extensions (USB HID) can't talk to them. Mobile
wallets can.

---

## Try it

You need [PlatformIO](https://platformio.org/).

```bash
pio run -e cyd -t upload && pio device monitor     # or -e c3
pio test -e native                                 # unit tests, no board needed
```

Then in a mobile wallet: **Add hardware wallet → Ledger → Bluetooth**. Switch the
wallet to devnet, send a small amount, and approve it on the screen.

---

## How it's built

```
esp32-solana-signer/
├── src/
│   ├── ble_transport.*   # GATT service + Ledger BLE framing (0x05 frames, MTU query)
│   ├── apdu.*            # Solana app commands (E0 04 config, E0 05 pubkey, E0 06/07 sign)
│   ├── keys.*            # SLIP-10 derivation + Ed25519 (Monocypher)
│   ├── seed_store.*      # random seed in flash (NVS)
│   ├── tx_decode.*       # reads the transaction: amount, recipient, fee, programs, base58
│   ├── ui_cyd.*          # CYD screens in LVGL 9 (sign, hold, receipt, idle, address)
│   ├── approve.*         # BOOT-button approval (C3)
│   ├── fonts/            # Inter, generated by scripts/make_fonts.sh
│   └── main.cpp          # glue
├── lib/monocypher/       # Ed25519, vendored
├── test/                 # Unity tests (APDU parsing, transaction decoding)
└── platformio.ini        # cyd, c3 and native environments
```

| Layer | Detail |
|---|---|
| Board | ESP32-WROOM (CYD), ESP32-C3 |
| Bluetooth | NimBLE-Arduino 2.x, Ledger Nano X GATT profile |
| Crypto | SLIP-10, Ed25519 via Monocypher 4 |
| Screen | TFT_eSPI + LVGL 9.2, Inter fonts, XPT2046 touch on the shared SPI bus |
| Tests | Unity on the host (`pio test -e native`) |

---

## Roadmap

1. **Probe:** do Phantom and Jupiter accept it? *(done)*
2. **Single key:** real Ed25519 key, approval, devnet. *(done)*
3. **CYD touchscreen:** see what you sign, press and hold, receipt. *(done)*
4. **Protect the seed:** PIN + encryption first. Later, a second key half sent from
   the phone as a short sound chirp to a microphone on the board. Flash encryption last.
5. **Dashboard mode:** lock screen, portfolio, activity and prices over Wi-Fi,
   brightness setting, a "≈ $" value on the sign screen.
6. **More decoding:** SPL token transfers and common swaps, so fewer transactions
   show up as unknown.

---

## Licence

MIT, see `LICENSE`. Bundles Monocypher (`lib/monocypher`, CC0 / BSD-2-Clause) and the
Inter font (`src/fonts`, SIL Open Font Licence).
