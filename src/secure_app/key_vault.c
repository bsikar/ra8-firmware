/**
 * @file key_vault.c
 * @brief Secure-only symmetric key store implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * implementation. Stores 8 symmetric keys in a static
 * array that lives in the secure SRAM partition. Provides three
 * operations:
 *
 * - ``ra_key_vault_init`` zero every slot.
 * - ``ra_key_vault_store`` copy a 32-byte key into a slot.
 * - ``ra_key_vault_sha256_xor_challenge`` compute
 * SHA-256(key XOR challenge) -- the only operation the
 * NS world can reach via the NSC veneer.
 *
 * The SHA-256 implementation is a small fixed-iteration sponge
 * baked into this file -- no external libraries, no heap. It is
 * NIST FIPS 180-4 compliant for the single-block case used here
 * (32-byte input, 32-byte output, single padding block).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "key_vault.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

/**
 * @struct ra_key_vault_slot_t
 * @brief A single symmetric-key slot.
 */
typedef struct {
  uint8_t key[k_ra_key_vault_key_bytes];
} ra_key_vault_slot_t;

static ra_key_vault_slot_t s_vault[k_ra_key_vault_slots];
static const char*         s_tag = "KEYV";

/* =============================================================================
 * Tiny SHA-256 (single-block, 32-byte input)
 * =============================================================================
 *
 * Implements just enough of FIPS 180-4 to compute SHA-256 over a
 * 32-byte input. Used by ra_key_vault_sha256_xor_challenge to
 * scramble the key in a way that does not reveal the key bytes.
 *
 * The SHA-256 sponge is a transcription of the FIPS 180-4 spec.
 * Every magic number below comes directly from the standard;
 * giving them names would inflate the code without adding clarity.
 * Same for the function size: SHA-256 is one big loop and is
 * routinely written as a single function.
 */

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)

static const uint32_t k_sha256_k[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
  0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
  0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
  0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
  0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
  0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
  0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
  0xc67178f2U,
};

static const uint32_t k_sha256_h0[8] = {
  0x6a09e667U,
  0xbb67ae85U,
  0x3c6ef372U,
  0xa54ff53aU,
  0x510e527fU,
  0x9b05688cU,
  0x1f83d9abU,
  0x5be0cd19U,
};

static uint32_t internal_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (32U - n));
}

/**
 * @brief Hash a 32-byte input. Padding produces a single 64-byte block.
 */
static void internal_sha256_32(const uint8_t* in32, uint8_t* out32)
{
  /* Build the 64-byte block: 32 input bytes, 0x80, zeros, then
   * the 64-bit length in bits at the end (little-endian quirk:
   * SHA-256 uses BIG endian length). */
  uint8_t block[64];
  for (uint32_t i = 0U; i < 32U; ++i) {
    block[i] = in32[i];
  }
  block[32] = 0x80U;
  for (uint32_t i = 33U; i < 56U; ++i) {
    block[i] = 0U;
  }
  /* Length = 32 bytes = 256 bits. */
  block[56] = 0U;
  block[57] = 0U;
  block[58] = 0U;
  block[59] = 0U;
  block[60] = 0U;
  block[61] = 0U;
  block[62] = 0x01U; /* 0x0100 = 256 in big-endian. */
  block[63] = 0x00U;

  uint32_t w[64];
  for (uint32_t i = 0U; i < 16U; ++i) {
    w[i] = ((uint32_t)block[(i * 4U) + 0U] << 24U) | ((uint32_t)block[(i * 4U) + 1U] << 16U) |
           ((uint32_t)block[(i * 4U) + 2U] << 8U) | ((uint32_t)block[(i * 4U) + 3U]);
  }
  for (uint32_t i = 16U; i < 64U; ++i) {
    const uint32_t s0 =
      internal_rotr(w[i - 15U], 7U) ^ internal_rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
    const uint32_t s1 =
      internal_rotr(w[i - 2U], 17U) ^ internal_rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
    w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
  }

  uint32_t a = k_sha256_h0[0];
  uint32_t b = k_sha256_h0[1];
  uint32_t c = k_sha256_h0[2];
  uint32_t d = k_sha256_h0[3];
  uint32_t e = k_sha256_h0[4];
  uint32_t f = k_sha256_h0[5];
  uint32_t g = k_sha256_h0[6];
  uint32_t h = k_sha256_h0[7];

  for (uint32_t i = 0U; i < 64U; ++i) {
    const uint32_t s1    = internal_rotr(e, 6U) ^ internal_rotr(e, 11U) ^ internal_rotr(e, 25U);
    const uint32_t ch    = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + k_sha256_k[i] + w[i];
    const uint32_t s0    = internal_rotr(a, 2U) ^ internal_rotr(a, 13U) ^ internal_rotr(a, 22U);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    h                    = g;
    g                    = f;
    f                    = e;
    e                    = d + temp1;
    d                    = c;
    c                    = b;
    b                    = a;
    a                    = temp1 + temp2;
  }

  const uint32_t hh[8] = {
    a + k_sha256_h0[0],
    b + k_sha256_h0[1],
    c + k_sha256_h0[2],
    d + k_sha256_h0[3],
    e + k_sha256_h0[4],
    f + k_sha256_h0[5],
    g + k_sha256_h0[6],
    h + k_sha256_h0[7],
  };
  for (uint32_t i = 0U; i < 8U; ++i) {
    out32[(i * 4U) + 0U] = (uint8_t)(hh[i] >> 24U);
    out32[(i * 4U) + 1U] = (uint8_t)(hh[i] >> 16U);
    out32[(i * 4U) + 2U] = (uint8_t)(hh[i] >> 8U);
    out32[(i * 4U) + 3U] = (uint8_t)(hh[i]);
  }
}

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_key_vault_init(void)
{
  for (uint16_t s = 0U; s < (uint16_t)k_ra_key_vault_slots; ++s) {
    for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_key_bytes; ++i) {
      s_vault[s].key[i] = 0U;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_key_vault_store(uint16_t slot, const uint8_t* key)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "store: key");
  if (slot >= (uint16_t)k_ra_key_vault_slots) {
    return k_ra_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_key_bytes; ++i) {
    s_vault[slot].key[i] = key[i];
  }
  return k_ra_ok;
}

ra_err_t ra_key_vault_sha256_xor_challenge(uint16_t slot, const uint8_t* challenge, uint8_t* out)
{
  RA_CHECK_NULL_PTR((void*)challenge, s_tag, "challenge: challenge");
  RA_CHECK_NULL_PTR(out, s_tag, "challenge: out");
  if (slot >= (uint16_t)k_ra_key_vault_slots) {
    return k_ra_err_invalid_arg;
  }
  uint8_t scratch[k_ra_key_vault_key_bytes] = {};
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_key_bytes; ++i) {
    scratch[i] = (uint8_t)(s_vault[slot].key[i] ^ challenge[i]);
  }
  internal_sha256_32(scratch, out);
  /* Wipe the scratch buffer so the XOR result does not linger
   * on the secure stack. cppcheck flags the writes as unused
   * because the function returns immediately after, but that is
   * exactly the secure-erase pattern -- we want the bytes gone
   * before the stack frame is reused. */
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_key_bytes; ++i) {
    /* cppcheck-suppress unreadVariable */
    scratch[i] = 0U;
  }
  return k_ra_ok;
}
