#pragma once
#include <stddef.h>
#include <stdint.h>

// An APDU command is: [CLA][INS][P1][P2][Lc][data: Lc bytes]
// A response is:      [data...][SW1][SW2]   where 0x9000 means "OK".
//
// Commands the probe answers:
//   B0 01  get app and version  (OS level: "which app is open?")  -> TODO(human)
//   E0 04  Solana: get app configuration
//   E0 05  Solana: get public key for a derivation path
//   E0 06  Solana: sign transaction        -> asks the human, then signs
//   E0 07  Solana: sign off-chain message  -> asks the human, then signs
// Anything else is logged and answered "unknown instruction" (0x6D00), so the serial
// log tells us exactly which extra commands a wallet expects.

size_t handleApdu(const uint8_t* apdu, size_t len, uint8_t* out, size_t outCap);

// Who decides whether to sign. main.cpp plugs in the BOOT button (approve.cpp);
// the unit tests plug in a fake that always says yes or no.
// Returns true = sign, false = refuse. Without a callback, everything is refused.
typedef bool (*ApprovalFn)(const uint8_t* message, size_t len);
void setApprovalCallback(ApprovalFn fn);

// Exposed so it can be unit-tested.
size_t buildAppAndVersion(uint8_t* out, size_t outCap);
