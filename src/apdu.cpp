#include "apdu.h"

#include <string.h>

#include "keys.h"

namespace {

constexpr uint16_t SW_OK = 0x9000;
constexpr uint16_t SW_WRONG_LENGTH = 0x6700;
constexpr uint16_t SW_USER_REFUSED = 0x6985;
constexpr uint16_t SW_INVALID_MESSAGE = 0x6a80;
constexpr uint16_t SW_MESSAGE_TOO_BIG = 0x6a83;
constexpr uint16_t SW_INVALID_PATH = 0x6a84;
constexpr uint16_t SW_UNKNOWN_INS = 0x6d00;
constexpr uint16_t SW_UNKNOWN_CLA = 0x6e00;

// Version of the real Ledger Solana app we claim to be (app-solana Makefile, Nov 2026).
// Wallets sometimes refuse apps older than some minimum, so report a current one.
constexpr uint8_t kAppMajor = 1, kAppMinor = 16, kAppPatch = 0;

size_t putSw(uint8_t* out, size_t at, uint16_t sw) {
  out[at] = sw >> 8;
  out[at + 1] = sw & 0xff;
  return at + 2;
}

// E0 04: [blind signing enabled][pubkey display mode][major][minor][patch]
size_t getAppConfiguration(uint8_t* out) {
  out[0] = 0x01;  // blind signing on, so wallets don't stop at "enable blind signing"
  out[1] = 0x00;  // long pubkey display
  out[2] = kAppMajor;
  out[3] = kAppMinor;
  out[4] = kAppPatch;
  return putSw(out, 5, SW_OK);
}

// Reads a derivation path: [count n][n x uint32 big-endian], e.g. 44'/501'/0'/0'.
// Drops the 0x80 "hardened" bit (deriveKey adds it back; Solana paths are always hardened).
// Returns how many bytes it used, or 0 if the path is broken.
size_t readPath(const uint8_t* data, size_t dataLen, uint32_t path[5], size_t* pathLen) {
  if (dataLen < 1 || data[0] == 0 || data[0] > 5 || dataLen < 1 + data[0] * 4u) return 0;
  *pathLen = data[0];
  for (size_t i = 0; i < *pathLen; i++) {
    const uint8_t* p = data + 1 + i * 4;
    path[i] = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) &
              0x7fffffffu;
  }
  return 1 + *pathLen * 4;
}

// E0 05: answers with the real Solana address for the path (see keys.cpp).
size_t getPublicKey(const uint8_t* data, size_t dataLen, uint8_t* out) {
  uint32_t path[5];
  size_t pathLen;
  if (!readPath(data, dataLen, path, &pathLen)) return putSw(out, 0, SW_INVALID_PATH);

  uint8_t privateKey[32];
  deriveKey(path, pathLen, privateKey);
  publicKeyFromPrivate(privateKey, out);
  wipe(privateKey, sizeof privateKey);
  return putSw(out, 32, SW_OK);
}

// ---- Signing (E0 06 transaction, E0 07 off-chain message) ----
//
// First part:  data = [signer count = 1][path][message bytes...]
// Later parts: data = [more message bytes...]          (P2 has the 0x01 "extend" bit)
// Every part except the last has the 0x02 "more" bit; we answer 90 00 and keep collecting.
// On the last part we ask the human, then sign with Ed25519 and return the 64-byte signature.

constexpr size_t kMaxMessage = 1232;  // biggest possible Solana transaction
uint8_t gMessage[kMaxMessage];
size_t gMessageLen = 0;
uint32_t gSignPath[5];
size_t gSignPathLen = 0;
bool gCollecting = false;
ApprovalFn gApprove = nullptr;
SignedFn gSigned = nullptr;

size_t sign(uint8_t p2, const uint8_t* data, size_t dataLen, uint8_t* out) {
  bool extend = p2 & 0x01;
  bool more = p2 & 0x02;

  if (!extend) {  // first part: starts a new request
    gMessageLen = 0;
    gCollecting = false;
    if (dataLen < 1 || data[0] != 1) return putSw(out, 0, SW_INVALID_MESSAGE);
    size_t used = readPath(data + 1, dataLen - 1, gSignPath, &gSignPathLen);
    if (!used) return putSw(out, 0, SW_INVALID_PATH);
    data += 1 + used;
    dataLen -= 1 + used;
    gCollecting = true;
  } else if (!gCollecting) {
    return putSw(out, 0, SW_INVALID_MESSAGE);  // an "extend" with no first part
  }

  if (gMessageLen + dataLen > kMaxMessage) {
    gCollecting = false;
    return putSw(out, 0, SW_MESSAGE_TOO_BIG);
  }
  memcpy(gMessage + gMessageLen, data, dataLen);
  gMessageLen += dataLen;
  if (more) return putSw(out, 0, SW_OK);  // wait for the rest

  gCollecting = false;
  if (gApprove == nullptr || !gApprove(gMessage, gMessageLen)) {
    return putSw(out, 0, SW_USER_REFUSED);
  }

  uint8_t privateKey[32];
  deriveKey(gSignPath, gSignPathLen, privateKey);
  signMessage(privateKey, gMessage, gMessageLen, out);  // 64-byte signature into out[0..63]
  wipe(privateKey, sizeof privateKey);
  if (gSigned != nullptr) gSigned(out);
  return putSw(out, 64, SW_OK);
}

}  // namespace

void setApprovalCallback(ApprovalFn fn) { gApprove = fn; }
void setSignedCallback(SignedFn fn) { gSigned = fn; }

// B0 01: the OS-level "which app is running?" question. Wallets ask it first, to
// check that the Solana app is open (and not the home screen or another app).
//
// The answer is a row of bytes. In C a "row of bytes" is an array: out[0] is the
// first byte, out[1] the second, and so on. We fill it one byte at a time:
//
//   01                 format, always 1
//   06 S o l a n a     length of the name (6), then the 6 letters
//   06 1 . 1 6 . 0     length of the version (6), then the 6 characters
//   01 00              length of the flags (1), then the flag byte
//   90 00              status word "OK"
//
// 'S' in single quotes is C for "the byte of the letter S" (0x53).
size_t buildAppAndVersion(uint8_t* out, size_t outCap) {
  if (outCap < 19) return 0;  // not enough room: give up

  out[0] = 0x01;  // format

  out[1] = 6;  // name length
  out[2] = 'S';
  out[3] = 'o';
  out[4] = 'l';
  out[5] = 'a';
  out[6] = 'n';
  out[7] = 'a';

  out[8] = 6;  // version length
memcpy(out + 9, "1.16.0", 6);
  // TODO(human): write the 6 version characters '1' '.' '1' '6' '.' '0'
  // into out[9] to out[14], the same way the name is written above.

  out[15] = 1;  // flags length
  out[16] = 0;  // flags

  return putSw(out, 17, SW_OK);  // adds 90 00 at out[17], out[18]; returns 19
}

size_t handleApdu(const uint8_t* apdu, size_t len, uint8_t* out, size_t outCap) {
  if (len < 4) return putSw(out, 0, SW_WRONG_LENGTH);
  uint8_t cla = apdu[0], ins = apdu[1];
  const uint8_t* data = len > 5 ? apdu + 5 : nullptr;
  size_t dataLen = len > 5 ? apdu[4] : 0;
  if (dataLen > len - 5) return putSw(out, 0, SW_WRONG_LENGTH);

  if (cla == 0xb0 && ins == 0x01) return buildAppAndVersion(out, outCap);
  if (cla == 0xb0) return putSw(out, 0, SW_UNKNOWN_INS);
  if (cla != 0xe0) return putSw(out, 0, SW_UNKNOWN_CLA);

  // No seed loaded = no keys. Say "not ready" (0x6985) instead of using zeros.
  if ((ins == 0x05 || ins == 0x06 || ins == 0x07) && !hasSeed()) {
    return putSw(out, 0, SW_USER_REFUSED);
  }

  switch (ins) {
    case 0x04:
      return getAppConfiguration(out);
    case 0x05:
      return getPublicKey(data, dataLen, out);
    case 0x06:  // sign transaction
    case 0x07:  // sign off-chain message
      if (outCap < 66) return putSw(out, 0, SW_WRONG_LENGTH);
      return sign(apdu[3], data, dataLen, out);
    default:
      return putSw(out, 0, SW_UNKNOWN_INS);
  }
}
