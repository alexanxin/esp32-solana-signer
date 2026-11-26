#pragma once
#include <stddef.h>
#include <stdint.h>

// The Ledger BLE transport, as the wallets' @ledgerhq/*-transport-ble libraries expect it.
//
// Layer 1, GATT: one service with three characteristics (same UUIDs as a Nano X,
//   because the wallets scan for exactly those UUIDs).
//     ...0001  notify     device -> phone
//     ...0002  write      phone -> device (with response)
//     ...0003  write-cmd  phone -> device (without response)
//
// Layer 2, framing: an APDU can be bigger than one BLE packet, so it is cut into frames:
//     first frame:  [0x05][seq=0x0000][total length, 2 bytes][payload...]
//     next frames:  [0x05][seq, 2 bytes]                     [payload...]
//   Plus one special frame: the phone sends [0x08 00 00 00 00] to ask how big a frame
//   may be, and we answer [0x08 00 00 00 00 <bytes per frame>].
//
// Layer 3, APDU: the actual command (see apdu.h). This file never looks inside it.

void bleTransportBegin(const char* deviceName);

// Call from loop(). Returns true once a whole APDU has arrived; *apdu stays valid
// until the next call.
bool bleTransportTakeApdu(const uint8_t** apdu, size_t* len);

// Frames and notifies a response APDU (data + 2-byte status word).
void bleTransportSend(const uint8_t* data, size_t len);

// Answers a pending 0x08 frame-size question, if any. Call from loop().
void bleTransportService();
