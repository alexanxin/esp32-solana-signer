<p align="center">
  <img src="docs/screens/sign-flow.png" alt="Sigly sign flow: review, hold, signing, receipt" width="100%">
</p>

# Sigly

<p align="center">
  <img src="docs/badges/devnet.svg" alt="Devnet only">
  <img src="docs/badges/board.svg" alt="ESP32 CYD">
  <img src="docs/badges/wallets.svg" alt="Phantom and Jupiter">
  <img src="docs/badges/lvgl.svg" alt="LVGL 9">
  <img src="docs/badges/licence.svg" alt="MIT">
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

**Not safe for real funds yet.** On the CYD the seed is locked with your PIN
(Argon2id + XChaCha20-Poly1305) and backed up as 24 recovery words, but flash
encryption is not on yet: someone who takes the board and reads its flash chip could
try PINs on a computer, where the 3-try limit doesn't apply. Use devnet.

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

### 24 recovery words

<p align="center">
  <img src="docs/screens/recovery-words.png" alt="Set up, write down the words, check them, restore" width="100%">
</p>

- **New wallet:** Sigly shows 24 words (BIP39) once, on two pages. Write them on paper.
- **Backup check:** it then asks for 3 random positions ("Which is word #16?") and you
  tap the right word out of 4. Wrong answer: look at your paper and try again.
- **Restore:** type the words on an A to Z keyboard where letters that can't lead to a
  word are greyed out. After 2 to 4 letters you tap the word. A wrong word is caught by
  the checksum, and before choosing a PIN you see the address the words give, to
  compare with your wallet app.
- **Standard words:** the same words work in Phantom, Solflare or a Ledger
  (BIP39 → SLIP-10 `44'/501'/n'/0'`), so the backup is useful even without Sigly.
- **3 wrong PINs erase Sigly.** Your words bring the wallet back.
- A wallet from older firmware has no words (its seed was plain random bytes). After
  you unlock, Sigly offers a new wallet with words (new address) or "Later".

### More screens

<p align="center">
  <img src="docs/screens/more-screens.png" alt="Idle screen, unknown transaction, receive address" width="100%">
</p>

- **Idle:** connection status ("Phone connected" or "Waiting for your phone").
- **My address:** your account 1 address as a QR code, shown by the device itself,
  so you can check it against what the wallet app says.
- **Settings:** device name, network, backup status, brightness.

### Works with the wallets you already use

- Shows up as a Bluetooth Ledger running the Solana app.
- Derives addresses exactly like Phantom and Ledger (SLIP-10, `44'/501'/n'/0'`), so
  your accounts line up.
- Makes its recovery words from the ESP32 hardware RNG (with the radio on, so it is
  truly random).

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
│   ├── bip39.*           # 24 recovery words: word list, checksum, words -> seed
│   ├── pin_vault.*       # the seed locked with your PIN (Argon2id + XChaCha20-Poly1305)
│   ├── seed_store.*      # the vault in flash (NVS), wrong-PIN counter, wipe
│   ├── tx_decode.*       # reads the transaction: amount, recipient, fee, programs, base58
│   ├── ui_cyd.*          # CYD screens in LVGL 9 (sign, hold, receipt, idle, address)
│   ├── approve.*         # BOOT-button approval (C3)
│   ├── fonts/            # Inter, generated by scripts/make_fonts.sh
│   └── main.cpp          # glue
├── lib/monocypher/       # Ed25519, vendored
├── test/                 # Unity tests (APDU, transaction decoding, vault, BIP39 vectors)
└── platformio.ini        # cyd, c3 and native environments
```

| Layer | Detail |
|---|---|
| Board | ESP32-WROOM (CYD), ESP32-C3 |
| Bluetooth | NimBLE-Arduino 2.x, Ledger Nano X GATT profile |
| Crypto | BIP39, SLIP-10, Ed25519, Argon2id via Monocypher 4 |
| Screen | TFT_eSPI + LVGL 9.2, Inter fonts, XPT2046 touch on the shared SPI bus |
| Tests | Unity on the host (`pio test -e native`) |

---

## Roadmap

1. **Probe:** do Phantom and Jupiter accept it? *(done)*
2. **Single key:** real Ed25519 key, approval, devnet. *(done)*
3. **CYD touchscreen:** see what you sign, press and hold, receipt. *(done)*
4. **Protect the seed:** PIN + encryption *(done)*, 24 recovery words and wipe after
   3 wrong PINs *(done)*. Later, a second key half sent from the phone as a short sound
   chirp to a microphone on the board. Flash encryption last.
5. **Dashboard mode:** lock screen, portfolio, activity and prices over Wi-Fi,
   brightness setting, a "≈ $" value on the sign screen.
6. **More decoding:** SPL token transfers and common swaps, so fewer transactions
   show up as unknown.

---

## Licence

MIT, see `LICENSE`. Bundles Monocypher (`lib/monocypher`, CC0 / BSD-2-Clause) and the
Inter font (`src/fonts`, SIL Open Font Licence).
