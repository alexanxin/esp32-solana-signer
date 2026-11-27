// ESP32 Solana signer: speaks the Ledger Solana protocol over BLE, logs every byte,
// signs after you approve (CYD: press and hold; C3: BOOT button). DEVNET ONLY for now.

#include <Arduino.h>

#include "apdu.h"
#include "approve.h"
#include "ble_transport.h"
#include "keys.h"
#include "seed_store.h"
#include "log.h"
#ifdef BLEND_CYD
#include "ui_cyd.h"
#endif

// Name shown in the phone's Bluetooth scan. Deliberately not "Ledger".
const char* DEVICE_NAME = "Sigly";

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Sigly: Solana signer (DEVNET ONLY) ===");
#ifdef BLEND_CYD
  setApprovalCallback(uiApprove);  // signing asks the touchscreen: press and hold
  setSignedCallback(uiShowSignature);  // and the receipt shows the signature
  uiBegin(DEVICE_NAME);
#else
  setApprovalCallback(waitForApproval);  // no screen: signing asks the BOOT button
#endif
  bleTransportBegin(DEVICE_NAME);

  // After Bluetooth is on, so the random generator has radio noise to work with.
#ifdef BLEND_CYD
  uiStartLocked();  // PIN screen; the seed is loaded once you unlock
#else
  // C3: no screen to type a PIN on or show words, so the seed stays unlocked in flash
  // and has no recovery words for now.
  uint8_t seed[64];
  if (loadOrCreateSeed(seed)) {
    setSeed(seed);
  } else {
    Serial.println("!!! no seed saved. All key requests refused.");
  }
  wipe(seed, sizeof seed);  // keys.cpp keeps its own copy
#endif
}

void loop() {
  bleTransportService();
#ifdef BLEND_CYD
  uiService();
#endif

  const uint8_t* apdu;
  size_t len;
  if (bleTransportTakeApdu(&apdu, &len)) {
    logHex("APDU <-", apdu, len);
    uint8_t response[128];  // a signature is 64 bytes + 2 status bytes
    size_t n = handleApdu(apdu, len, response, sizeof response);
    logHex("APDU ->", response, n);
    bleTransportSend(response, n);
  }
  delay(2);
}
