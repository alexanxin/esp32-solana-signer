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
#include <esp_bt.h>  // esp_bt_controller_mem_release()

#include "dash_ui.h"
#include "mode.h"
#include "ui_cyd.h"
#endif

// Name shown in the phone's Bluetooth scan. Deliberately not "Ledger".
const char* DEVICE_NAME = "Sigly";

#ifdef BLEND_CYD
// Which mode this start is in (see mode.h). Dashboard = Wi-Fi, no Bluetooth, seed locked.
bool gDashboard = false;
#endif

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Sigly: Solana signer (DEVNET ONLY) ===");
  
#ifdef BLEND_CYD
  SiglyMode mode = modeAtBoot();
  // The very first start (no PIN vault yet) goes to the Signer: that's where you create or
  // restore your wallet, and the Dashboard has no address to show before that.
  if (seedState() != SeedState::kVault) mode = SiglyMode::kSigner;
  gDashboard = mode == SiglyMode::kDashboard;
  Serial.printf("mode: %s\n", gDashboard ? "Dashboard (Wi-Fi, keys locked)" : "Signer (Bluetooth)");
  if (gDashboard) {
    // No Bluetooth in this mode, ever: hand its reserved memory back (until the next
    // restart) so Wi-Fi and HTTPS have more room. The seed is never loaded.
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    uiBegin(DEVICE_NAME);  // screen + boot animation
    dashUiBegin();
    return;
  }
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
#ifdef BLEND_CYD
  if (gDashboard) {
    dashUiService();
    delay(2);
    return;
  }
#endif
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
