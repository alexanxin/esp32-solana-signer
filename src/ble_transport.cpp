#include "ble_transport.h"

#include <NimBLEDevice.h>

#include "log.h"

namespace {

// Nano X GATT layout, copied from @ledgerhq/devices (bluetoothSpec).
// Stax uses ...6004..., Flex ...3004..., Nano X ...0004... in the 3rd group.
constexpr const char* kServiceUuid = "13d63400-2c97-0004-0000-4c6564676572";
constexpr const char* kNotifyUuid = "13d63400-2c97-0004-0001-4c6564676572";
constexpr const char* kWriteUuid = "13d63400-2c97-0004-0002-4c6564676572";
constexpr const char* kWriteCmdUuid = "13d63400-2c97-0004-0003-4c6564676572";

constexpr uint8_t kTagApdu = 0x05;
constexpr uint8_t kTagMtu = 0x08;

// A Solana transaction is at most 1232 bytes; leave room for path + header.
constexpr size_t kMaxApdu = 1600;
constexpr size_t kMaxFrame = 255;  // the 0x08 answer is a single byte

NimBLECharacteristic* gNotify = nullptr;
volatile bool gConnected = false;  // set from the Bluetooth task, read from loop()
volatile uint16_t gFramePayload = 20;  // ATT MTU - 3; 20 until the phone negotiates more

uint8_t gRx[kMaxApdu];
size_t gRxExpected = 0;
size_t gRxGot = 0;
uint16_t gRxNextSeq = 0;
volatile bool gApduReady = false;
volatile bool gMtuAsked = false;

void resetRx() {
  gRxExpected = 0;
  gRxGot = 0;
  gRxNextSeq = 0;
}

// Runs on the NimBLE task for every GATT write.
void onFrame(const uint8_t* d, size_t n) {
  logHex("frame <-", d, n);
  if (n == 0) return;

  if (d[0] == kTagMtu) {
    gMtuAsked = true;  // answered from loop(), not from inside the BLE callback
    return;
  }
  if (d[0] != kTagApdu || n < 3) {
    Serial.printf("  ignoring unknown tag 0x%02x\n", d[0]);
    return;
  }

  uint16_t seq = (d[1] << 8) | d[2];
  size_t offset = 3;
  if (seq == 0) {
    if (n < 5) return;
    resetRx();
    gRxExpected = (d[3] << 8) | d[4];
    offset = 5;
    if (gRxExpected > kMaxApdu) {
      Serial.printf("  APDU too big (%u), dropping\n", (unsigned)gRxExpected);
      resetRx();
      return;
    }
  } else if (seq != gRxNextSeq) {
    Serial.printf("  out-of-order frame %u (wanted %u), dropping\n", seq, gRxNextSeq);
    resetRx();
    return;
  }

  size_t take = n - offset;
  if (take > gRxExpected - gRxGot) take = gRxExpected - gRxGot;
  memcpy(gRx + gRxGot, d + offset, take);
  gRxGot += take;
  gRxNextSeq = seq + 1;
  if (gRxGot == gRxExpected) gApduReady = true;
}

class WriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    onFrame(v.data(), v.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    Serial.printf("connected: %s\n", info.getAddress().toString().c_str());
    gConnected = true;
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("disconnected, reason 0x%x\n", reason);
    gConnected = false;
    resetRx();
    gApduReady = false;
    gFramePayload = 20;
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    uint16_t payload = mtu - 3;
    gFramePayload = payload > kMaxFrame ? kMaxFrame : payload;
    Serial.printf("MTU %u -> %u bytes per frame\n", mtu, gFramePayload);
  }
};

WriteCallbacks gWriteCallbacks;
ServerCallbacks gServerCallbacks;

}  // namespace

void bleTransportBegin(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&gServerCallbacks);

  NimBLEService* svc = server->createService(kServiceUuid);
  gNotify = svc->createCharacteristic(kNotifyUuid, NIMBLE_PROPERTY::NOTIFY);
  svc->createCharacteristic(kWriteUuid, NIMBLE_PROPERTY::WRITE)->setCallbacks(&gWriteCallbacks);
  svc->createCharacteristic(kWriteCmdUuid, NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(&gWriteCallbacks);

  // A 128-bit UUID fills most of the 31-byte advert, so the name goes in the scan response.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(kServiceUuid);
  NimBLEAdvertisementData scanData;
  scanData.setName(deviceName);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();

  Serial.printf("advertising as \"%s\" with service %s\n", deviceName, kServiceUuid);
}

bool bleTransportTakeApdu(const uint8_t** apdu, size_t* len) {
  if (!gApduReady) return false;
  gApduReady = false;
  *apdu = gRx;
  *len = gRxExpected;
  return true;
}

void bleTransportSend(const uint8_t* data, size_t len) {
  uint8_t frame[kMaxFrame];
  uint16_t seq = 0;
  size_t offset = 0;
  do {
    size_t header = (seq == 0) ? 5 : 3;
    size_t take = len - offset;
    if (take > gFramePayload - header) take = gFramePayload - header;
    frame[0] = kTagApdu;
    frame[1] = seq >> 8;
    frame[2] = seq & 0xff;
    if (seq == 0) {
      frame[3] = len >> 8;
      frame[4] = len & 0xff;
    }
    memcpy(frame + header, data + offset, take);
    gNotify->setValue(frame, header + take);
    gNotify->notify();
    logHex("frame ->", frame, header + take);
    offset += take;
    seq++;
  } while (offset < len);
}

bool bleTransportConnected() { return gConnected; }

void bleTransportService() {
  if (!gMtuAsked) return;
  gMtuAsked = false;
  uint8_t answer[6] = {kTagMtu, 0, 0, 0, 0, (uint8_t)gFramePayload};
  gNotify->setValue(answer, sizeof answer);
  gNotify->notify();
  logHex("frame ->", answer, sizeof answer);
}
