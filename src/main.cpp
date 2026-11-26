// ESP32 Solana signer: speaks the Ledger Solana protocol over BLE, logs every byte,
// signs with its own random key after a BOOT-button press. DEVNET ONLY for now.

#include <Arduino.h>

#include "apdu.h"
#include "approve.h"
#include "ble_transport.h"
#include "keys.h"
#include "seed_store.h"
#include "log.h"

// Name shown in the phone's Bluetooth scan. Deliberately not "Ledger".
const char* DEVICE_NAME = "Sigly";

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== ESP32 Solana signer: own random key (DEVNET ONLY, no backup yet), BOOT approves ===");
  setApprovalCallback(waitForApproval);  // signing asks the BOOT button
  bleTransportBegin(DEVICE_NAME);

  // After Bluetooth is on, so the random generator has radio noise to work with.
  uint8_t seed[64];
  if (loadOrCreateSeed(seed)) {
    setSeed(seed);
  } else {
    Serial.println("!!! no seed: finish the TODO in seed_store.cpp. All key requests refused.");
  }
  wipe(seed, sizeof seed);  // keys.cpp keeps its own copy
}

void loop() {
  bleTransportService();

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
