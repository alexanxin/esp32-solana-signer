# Sigly: an ESP32 Solana signer

Free, open-source firmware that turns a cheap ESP32 board into a Bluetooth signer
that speaks the open **Ledger Solana app protocol**, so wallets that already support
Ledger over Bluetooth might work with it unchanged.

Not affiliated with Ledger. "Ledger" is their trademark; this project only implements
the publicly documented protocol.

## Status: step 2, single key, DEVNET ONLY

Working today (tested with Phantom and Jupiter Mobile on Android):

- Shows up in wallets as a Bluetooth "Ledger" running the Solana app.
- Makes its own random seed on first boot (hardware RNG) and keeps it in flash.
- Derives Solana addresses exactly like Phantom/Ledger (SLIP-10, `44'/501'/n'/0'`).
- Signs transactions after you press the BOOT button (blind signing: no screen yet).

**Not safe for real funds yet:** the seed is stored unencrypted, there is no backup
phrase, and the device can't show what it signs. Use devnet.

| Blend | Board | Approve with | Status |
|---|---|---|---|
| `cyd` | Cheap Yellow Display (ESP32-WROOM, no PSRAM) | Touchscreen: see the amount and recipient, press and hold to sign. BOOT rejects. | step 2 + screen (new) |
| `c3` | ESP32-C3 SuperMini / DevKitM (no screen) | BOOT button (blind signing) | not built or tested yet |

Both are Bluetooth-only: classic ESP32 has no native USB and the C3's USB is a fixed
serial/JTAG port, so browser extensions (USB HID) can't talk to them. Mobile wallets can.

## Try it

```
pio run -e cyd -t upload && pio device monitor     # or -e c3
pio test -e native                                 # APDU unit tests, no board
```

Then in a mobile wallet: add hardware wallet -> Ledger -> Bluetooth, and watch the log.

## Layers

1. `ble_transport.*` GATT service + Ledger BLE framing (0x05 frames, 0x08 MTU query).
2. `apdu.*` the Solana app commands (E0 04 config, E0 05 pubkey, E0 06/07 sign).
3. `keys.*` SLIP-10 derivation + Ed25519 (via `lib/monocypher`).
4. `seed_store.*` random seed in flash (NVS). `approve.*` BOOT-button approval (C3).
5. `tx_decode.*` reads the transaction: SOL transfer amount, recipient, fee; base58 addresses.
6. `ui_cyd.*` CYD screens with LVGL 9 (sign request, press-and-hold, result, idle).
7. `main.cpp` glue.

## Roadmap

1. Probe: does Phantom / Jupiter accept it? *(done)*
2. Single key: real Ed25519 key, button approve, devnet. *(done)*
3. Protect the seed: PIN + encryption now; later a second key half sent from the phone
   as a short sound chirp to a microphone on the board. Flash encryption last.
4. CYD touchscreen: show what you sign, press-and-hold to approve, plus a Wi-Fi
   dashboard mode (portfolio, activity, prices). Screen designs are done in Claude Design.

## Licence

MIT, see `LICENSE`. Bundles Monocypher (`lib/monocypher`, CC0 / BSD-2-Clause).
