#pragma once
#include <stddef.h>
#include <stdint.h>

// Solana keys, the same way every Solana wallet makes them:
//
//   seed (64 bytes) --SLIP-10--> private key for a path like 44'/501'/0'/0'
//   private key     --Ed25519--> public key = the Solana address
//
// The seed comes from setSeed(): random bytes saved in flash on the board (seed_store.cpp),
// or the public test phrase "abandon ... about" in the unit tests.

void setSeed(const uint8_t seed[64]);
bool hasSeed();

// path: e.g. {44, 501, 0, 0}. Every level is "hardened" (the ' in 44'), as Solana requires.
void deriveKey(const uint32_t* path, size_t pathLen, uint8_t privateKey[32]);

void publicKeyFromPrivate(const uint8_t privateKey[32], uint8_t publicKey[32]);

// Ed25519 signature over message (for Solana: the serialized transaction message).
void signMessage(const uint8_t privateKey[32], const uint8_t* message, size_t len,
                 uint8_t signature[64]);

// Overwrites secret bytes with zeros so they don't linger in RAM.
void wipe(void* secret, size_t len);
