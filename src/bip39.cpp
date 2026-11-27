#include "bip39.h"

#include <string.h>

#include "bip39_english.h"
#include "keys.h"  // wipe()
#include "monocypher-ed25519.h"

// ---- Word list ---------------------------------------------------------------------------

const char* bip39Word(int index) {
  if (index < 0 || index >= 2048) return nullptr;
  return kBip39Words[index];
}

// The list is sorted, so "binary search": look at the middle, throw away the half that
// can't hold the word, repeat. 11 looks at most instead of up to 2048.
int bip39Find(const char* word) {
  int lo = 0, hi = 2047;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = strcmp(word, kBip39Words[mid]);
    if (c == 0) return mid;
    if (c < 0) hi = mid - 1;
    else lo = mid + 1;
  }
  return -1;
}

int bip39Matches(const char* prefix, int* first) {
  size_t n = strlen(prefix);
  int count = 0;
  int start = -1;
  // 2048 short compares: a few microseconds, no need to be clever here.
  for (int i = 0; i < 2048; i++) {
    if (strncmp(kBip39Words[i], prefix, n) == 0) {
      if (start < 0) start = i;
      count++;
    } else if (start >= 0) {
      break;  // sorted: once the matches stop, there are no more
    }
  }
  if (first != nullptr) *first = start;
  return count;
}

uint32_t bip39NextLetters(const char* prefix) {
  size_t n = strlen(prefix);
  int first;
  int count = bip39Matches(prefix, &first);
  uint32_t letters = 0;
  for (int i = first; count > 0 && i < first + count; i++) {
    char next = kBip39Words[i][n];  // the letter after the prefix ('\0' = word ends here)
    if (next >= 'a' && next <= 'z') letters |= 1u << (next - 'a');
  }
  return letters;
}

// ---- SHA-256 -----------------------------------------------------------------------------
// The standard (FIPS 180-4), written out plainly. Only used for the 8 checksum bits.

namespace {

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
           (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  wipe(w, sizeof w);
}

}  // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t full = len / 64;
  for (size_t i = 0; i < full; i++) sha256Block(h, data + 64 * i);

  // Last block(s): the leftover bytes, a 1 bit (0x80), zeros, then the length in bits.
  uint8_t tail[128] = {0};
  size_t rest = len - full * 64;
  memcpy(tail, data + full * 64, rest);
  tail[rest] = 0x80;
  size_t tailLen = rest + 9 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) tail[tailLen - 1 - i] = (uint8_t)(bits >> (8 * i));
  sha256Block(h, tail);
  if (tailLen == 128) sha256Block(h, tail + 64);
  wipe(tail, sizeof tail);

  for (int i = 0; i < 8; i++) {
    out[4 * i] = h[i] >> 24;
    out[4 * i + 1] = h[i] >> 16;
    out[4 * i + 2] = h[i] >> 8;
    out[4 * i + 3] = h[i];
  }
}

// ---- Entropy <-> words -------------------------------------------------------------------

// Bit number `bit` of a byte string, counting from the top bit of byte 0.
static int getBit(const uint8_t* bytes, int bit) { return (bytes[bit / 8] >> (7 - bit % 8)) & 1; }

void bip39FromEntropy(const uint8_t entropy[kEntropySize], uint16_t words[kWordCount]) {
  // 33 bytes = the 32 random bytes + the checksum byte. 24 x 11 = 264 bits = exactly 33 bytes.
  uint8_t bits[33];
  memcpy(bits, entropy, 32);
  uint8_t hash[32];
  sha256(entropy, 32, hash);
  bits[32] = hash[0];

  for (int w = 0; w < kWordCount; w++) {
    uint16_t n = 0;
    for (int b = 0; b < 11; b++) n = (n << 1) | getBit(bits, w * 11 + b);
    words[w] = n;
  }
  wipe(bits, sizeof bits);
  wipe(hash, sizeof hash);
}

bool bip39ChecksumOk(const uint16_t words[kWordCount]) {
  // Put the 24 x 11 bits back into 33 bytes, then redo the checksum from the first 32.
  uint8_t bits[33] = {0};
  for (int w = 0; w < kWordCount; w++) {
    for (int b = 0; b < 11; b++) {
      int bit = w * 11 + b;
      if ((words[w] >> (10 - b)) & 1) bits[bit / 8] |= 0x80 >> (bit % 8);
    }
  }
  uint8_t hash[32];
  sha256(bits, 32, hash);
  bool ok = hash[0] == bits[32];
  wipe(bits, sizeof bits);
  wipe(hash, sizeof hash);
  return ok;
}

// ---- Words -> seed -----------------------------------------------------------------------

// PBKDF2-HMAC-SHA512 with a 64-byte result (exactly one block, so no block counter loop):
//   U1 = HMAC(phrase, "mnemonic" + passphrase + 00 00 00 01)
//   U2 = HMAC(phrase, U1), U3 = HMAC(phrase, U2), ... up to U2048
//   seed = U1 xor U2 xor ... xor U2048
void bip39PhraseToSeed(const char* phrase, const char* passphrase, uint8_t seed[64]) {
  crypto_sha512_hmac_ctx keyed, ctx;
  // The HMAC key (the phrase) is the same 2048 times, so set it up once and copy it:
  // saves a quarter of the work.
  crypto_sha512_hmac_init(&keyed, (const uint8_t*)phrase, strlen(phrase));

  uint8_t u[64];
  ctx = keyed;
  crypto_sha512_hmac_update(&ctx, (const uint8_t*)"mnemonic", 8);
  crypto_sha512_hmac_update(&ctx, (const uint8_t*)passphrase, strlen(passphrase));
  const uint8_t blockOne[4] = {0, 0, 0, 1};
  crypto_sha512_hmac_update(&ctx, blockOne, 4);
  crypto_sha512_hmac_final(&ctx, u);
  memcpy(seed, u, 64);

  for (int round = 1; round < 2048; round++) {
    ctx = keyed;
    crypto_sha512_hmac_update(&ctx, u, 64);
    crypto_sha512_hmac_final(&ctx, u);
    for (int i = 0; i < 64; i++) seed[i] ^= u[i];
  }
  wipe(u, sizeof u);
  wipe(&keyed, sizeof keyed);
  wipe(&ctx, sizeof ctx);
}

void bip39ToSeed(const uint16_t words[kWordCount], const char* passphrase, uint8_t seed[64]) {
  // "word word word ...": 24 words of at most 8 letters + spaces = 215 characters at most.
  char phrase[kWordCount * 9];
  size_t n = 0;
  for (int w = 0; w < kWordCount; w++) {
    const char* word = bip39Word(words[w]);
    if (w > 0) phrase[n++] = ' ';
    size_t len = strlen(word);
    memcpy(phrase + n, word, len);
    n += len;
  }
  phrase[n] = '\0';
  bip39PhraseToSeed(phrase, passphrase, seed);
  wipe(phrase, sizeof phrase);
}
