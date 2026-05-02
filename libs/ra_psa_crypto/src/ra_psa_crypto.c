/**
 * @file ra_psa_crypto.c
 * @brief Implementation of the ``ra_psa_crypto`` facade over TF-PSA-Crypto.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Hosts the static key-handle pool and the thin translation layer
 * between PSA Crypto status codes (``psa_status_t``) and ``ra_err_t``.
 *
 * TF-PSA-Crypto is only linked into the firmware build when
 * ``RA_USE_MBEDTLS=ON`` (the same option that pulls in Mbed TLS). The
 * host unit-test build (``tests/CMakeLists.txt``) defines
 * ``RA_SIMULATOR_MODE`` for every translation unit and intentionally
 * does not link the heavy PSA object library; in that mode this file
 * substitutes a tiny in-memory crypto stand-in:
 *
 * - SHA-256 is implemented inline (FIPS 180-4 reference algorithm) so
 *   ``ra_psa_hash_compute`` can produce real digests in the test suite.
 * - AES-GCM is replaced by a deterministic XOR-and-checksum cipher
 *   that exercises the AEAD contract (ciphertext + tag layout, tag
 *   tampering detection) without requiring the real AES core.
 * - ECDSA sign / verify is replaced by an HMAC-style "signature" that
 *   binds (key bytes, hash) and verifies by recomputation. This is
 *   not cryptographically secure -- it exists only so the round-trip
 *   sign+verify test in ``test_ra_psa_crypto.c`` can pass without
 *   pulling in P-256.
 *
 * The public ``ra_psa_*`` surface is identical in either build, so a
 * caller migrating from simulator mode to a real firmware image only
 * has to flip the build flag.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_psa_crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

#ifndef RA_SIMULATOR_MODE
#include "psa/crypto.h"
#endif

/* =============================================================================
 * Logging tag
 * =============================================================================
 */

/** @brief Logging tag prefix used by every ``ra_psa_crypto`` log line. */
static const char* const k_ra_psa_tag = "ra_psa_crypto";

/* =============================================================================
 * Internal typed constants (no magic numbers)
 * =============================================================================
 */

/**
 * @enum ra_psa_internal_const_t
 * @brief Internal numeric constants used by the crypto facade.
 *
 * @details
 * Centralizes all sizes, byte offsets, and bit counts referenced by the
 * SHA-256 reference implementation and AEAD simulator paths so that no
 * magic numbers leak into the source.
 */
typedef enum : uint16_t {
  k_ra_psa_sha256_block_bytes   = 64U,   /**< SHA-256 message block size.         */
  k_ra_psa_sha256_state_words   = 8U,    /**< SHA-256 working state words.        */
  k_ra_psa_sha256_schedule_len  = 64U,   /**< Length of message schedule W[].     */
  k_ra_psa_sha256_init_words    = 16U,   /**< Initial copy from block to W[].     */
  k_ra_psa_sha256_pad_buf_len   = 128U,  /**< Two-block padding scratch.          */
  k_ra_psa_sha256_pad_threshold = 56U,   /**< If remaining < this, one tail block.*/
  k_ra_psa_sim_scratch_bytes    = 256U,  /**< AEAD-sim scratch buffer size.       */
  k_ra_psa_word_bits            = 32U,   /**< Word width in bits.                 */
  k_ra_psa_byte_bits            = 8U,    /**< Bits per byte.                      */
  k_ra_psa_bytes_per_word       = 4U,    /**< Bytes packed per 32-bit word.       */
  k_ra_psa_pad_marker           = 0x80U, /**< SHA-256 padding sentinel byte.      */
  k_ra_psa_length_field_bytes   = 8U,    /**< 64-bit big-endian length tail.      */
  k_ra_psa_shift_b3             = 24U,   /**< Shift for big-endian byte 3.        */
  k_ra_psa_shift_b2             = 16U,   /**< Shift for big-endian byte 2.        */
  k_ra_psa_shift_b1             = 8U,    /**< Shift for big-endian byte 1.        */
  k_ra_psa_rot_s0_a             = 7U,    /**< sigma0(W[i-15]) rot a.              */
  k_ra_psa_rot_s0_b             = 18U,   /**< sigma0(W[i-15]) rot b.              */
  k_ra_psa_shr_s0               = 3U,    /**< sigma0(W[i-15]) shr.                */
  k_ra_psa_rot_s1_a             = 17U,   /**< sigma1(W[i-2]) rot a.               */
  k_ra_psa_rot_s1_b             = 19U,   /**< sigma1(W[i-2]) rot b.               */
  k_ra_psa_shr_s1               = 10U,   /**< sigma1(W[i-2]) shr.                 */
  k_ra_psa_rot_e_a              = 6U,    /**< Sigma1(e) rot a.                    */
  k_ra_psa_rot_e_b              = 11U,   /**< Sigma1(e) rot b.                    */
  k_ra_psa_rot_e_c              = 25U,   /**< Sigma1(e) rot c.                    */
  k_ra_psa_rot_a_a              = 2U,    /**< Sigma0(a) rot a.                    */
  k_ra_psa_rot_a_b              = 13U,   /**< Sigma0(a) rot b.                    */
  k_ra_psa_rot_a_c              = 22U,   /**< Sigma0(a) rot c.                    */
  k_ra_psa_w_back_15            = 15U,   /**< Schedule offset W[i-15].            */
  k_ra_psa_w_back_2             = 2U,    /**< Schedule offset W[i-2].             */
  k_ra_psa_w_back_16            = 16U,   /**< Schedule offset W[i-16].            */
  k_ra_psa_w_back_7             = 7U,    /**< Schedule offset W[i-7].             */
  k_ra_psa_state_idx_a          = 0U,    /**< SHA-256 working register a slot. */
  k_ra_psa_state_idx_b          = 1U,    /**< SHA-256 working register b slot. */
  k_ra_psa_state_idx_c          = 2U,    /**< SHA-256 working register c slot. */
  k_ra_psa_state_idx_d          = 3U,    /**< SHA-256 working register d slot. */
  k_ra_psa_state_idx_e          = 4U,    /**< SHA-256 working register e slot. */
  k_ra_psa_state_idx_f          = 5U,    /**< SHA-256 working register f slot. */
  k_ra_psa_state_idx_g          = 6U,    /**< SHA-256 working register g slot. */
  k_ra_psa_state_idx_h          = 7U,    /**< SHA-256 working register h slot. */
} ra_psa_internal_const_t;

/**
 * @enum ra_psa_sha256_iv_t
 * @brief SHA-256 initial hash values H0..H7 (FIPS 180-4 Sec 5.3.3).
 */
typedef enum : uint32_t {
  k_ra_psa_sha256_h0 = 0x6a09e667U,
  k_ra_psa_sha256_h1 = 0xbb67ae85U,
  k_ra_psa_sha256_h2 = 0x3c6ef372U,
  k_ra_psa_sha256_h3 = 0xa54ff53aU,
  k_ra_psa_sha256_h4 = 0x510e527fU,
  k_ra_psa_sha256_h5 = 0x9b05688cU,
  k_ra_psa_sha256_h6 = 0x1f83d9abU,
  k_ra_psa_sha256_h7 = 0x5be0cd19U,
} ra_psa_sha256_iv_t;

/* =============================================================================
 * Pool slot definition
 * =============================================================================
 */

/**
 * @struct ra_psa_key_handle
 * @brief Concrete pool slot backing one ``ra_psa_key_t``.
 *
 * @details
 * One slot per imported key. ``in_use`` doubles as the bitmap bit; the
 * array index is the slot index ``[0, k_ra_psa_max_keys)``.
 *
 * @invariant ``in_use`` is true if and only if ``key_len > 0``.
 */
struct ra_psa_key_handle {
  bool              in_use;                      /**< Slot allocated.            */
  ra_psa_key_attr_t attr;                        /**< Cached caller attributes.  */
  size_t            key_len;                     /**< Bytes valid in ``key``.    */
  uint8_t           key[k_ra_psa_max_key_bytes]; /**< Raw key material (sim).    */
#ifndef RA_SIMULATOR_MODE
  psa_key_id_t psa_id; /**< Underlying PSA key identifier.                       */
#endif
};

/** @brief Per-handle state pool sized at compile time. */
static struct ra_psa_key_handle s_key_pool[k_ra_psa_max_keys];

/** @brief One-shot global init flag protecting the pool. */
static bool s_initialised;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate that a typed handle points into the static pool.
 *
 * @param[in] h Handle to validate.
 * @return ``true`` when ``h`` resolves to an in-use pool slot.
 */
static bool internal_handle_valid(ra_psa_key_t handle)
{
  if (handle == NULL) {
    return false;
  }
  const struct ra_psa_key_handle* base = &s_key_pool[0];
  const struct ra_psa_key_handle* end  = &s_key_pool[k_ra_psa_max_keys];
  if ((handle < base) || (handle >= end)) {
    return false;
  }
  return handle->in_use;
}

/**
 * @brief Locate the first free slot in the pool.
 *
 * @return Slot pointer or NULL when the pool is full.
 */
static struct ra_psa_key_handle* internal_alloc_slot(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_psa_max_keys; ++i) {
    if (!s_key_pool[i].in_use) {
      return &s_key_pool[i];
    }
  }
  return NULL;
}

#ifdef RA_SIMULATOR_MODE

/* ===========================================================================
 * Simulator-mode crypto primitives
 * ===========================================================================
 *
 * Real SHA-256 + tiny stand-ins for AES-GCM and ECDSA. These are
 * exercised by the host unit tests and never run on the target.
 */

/* -- SHA-256 (FIPS 180-4 reference) ---------------------------------------- */

/** @brief SHA-256 round constants K[0..63] (FIPS 180-4 Sec 4.2.2). */
static const uint32_t k_sha256_round_const[k_ra_psa_sha256_schedule_len] = {
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

static inline uint32_t internal_sha256_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (k_ra_psa_word_bits - n));
}

/** @brief Build the 64-word message schedule W[] from a single block. */
static void internal_sha256_schedule(uint32_t      schedule[k_ra_psa_sha256_schedule_len],
                                     const uint8_t block[k_ra_psa_sha256_block_bytes])
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_sha256_init_words; ++i) {
    schedule[i] =
      ((uint32_t)block[(i * (uint32_t)k_ra_psa_bytes_per_word) + 0U] << k_ra_psa_shift_b3) |
      ((uint32_t)block[(i * (uint32_t)k_ra_psa_bytes_per_word) + 1U] << k_ra_psa_shift_b2) |
      ((uint32_t)block[(i * (uint32_t)k_ra_psa_bytes_per_word) + 2U] << k_ra_psa_shift_b1) |
      ((uint32_t)block[(i * (uint32_t)k_ra_psa_bytes_per_word) + 3U]);
  }
  for (uint32_t i = (uint32_t)k_ra_psa_sha256_init_words;
       i < (uint32_t)k_ra_psa_sha256_schedule_len;
       ++i) {
    const uint32_t w15 = schedule[i - (uint32_t)k_ra_psa_w_back_15];
    const uint32_t w2  = schedule[i - (uint32_t)k_ra_psa_w_back_2];
    const uint32_t s0  = internal_sha256_rotr(w15, k_ra_psa_rot_s0_a) ^
                         internal_sha256_rotr(w15, k_ra_psa_rot_s0_b) ^ (w15 >> k_ra_psa_shr_s0);
    const uint32_t s1  = internal_sha256_rotr(w2, k_ra_psa_rot_s1_a) ^
                         internal_sha256_rotr(w2, k_ra_psa_rot_s1_b) ^ (w2 >> k_ra_psa_shr_s1);
    schedule[i]        = schedule[i - (uint32_t)k_ra_psa_w_back_16] + s0 +
                         schedule[i - (uint32_t)k_ra_psa_w_back_7] + s1;
  }
}

/** @brief Run the 64 SHA-256 compression rounds, updating ``state`` in place. */
static void internal_sha256_rounds(uint32_t       state[k_ra_psa_sha256_state_words],
                                   const uint32_t schedule[k_ra_psa_sha256_schedule_len])
{
  uint32_t a = state[k_ra_psa_state_idx_a];
  uint32_t b = state[k_ra_psa_state_idx_b];
  uint32_t c = state[k_ra_psa_state_idx_c];
  uint32_t d = state[k_ra_psa_state_idx_d];
  uint32_t e = state[k_ra_psa_state_idx_e];
  uint32_t f = state[k_ra_psa_state_idx_f];
  uint32_t g = state[k_ra_psa_state_idx_g];
  uint32_t h = state[k_ra_psa_state_idx_h];
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_sha256_schedule_len; ++i) {
    const uint32_t big_sigma1 = internal_sha256_rotr(e, k_ra_psa_rot_e_a) ^
                                internal_sha256_rotr(e, k_ra_psa_rot_e_b) ^
                                internal_sha256_rotr(e, k_ra_psa_rot_e_c);
    const uint32_t ch         = (e & f) ^ ((~e) & g);
    const uint32_t temp1      = h + big_sigma1 + ch + k_sha256_round_const[i] + schedule[i];
    const uint32_t big_sigma0 = internal_sha256_rotr(a, k_ra_psa_rot_a_a) ^
                                internal_sha256_rotr(a, k_ra_psa_rot_a_b) ^
                                internal_sha256_rotr(a, k_ra_psa_rot_a_c);
    const uint32_t maj        = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2      = big_sigma0 + maj;
    h                         = g;
    g                         = f;
    f                         = e;
    e                         = d + temp1;
    d                         = c;
    c                         = b;
    b                         = a;
    a                         = temp1 + temp2;
  }
  state[k_ra_psa_state_idx_a] += a;
  state[k_ra_psa_state_idx_b] += b;
  state[k_ra_psa_state_idx_c] += c;
  state[k_ra_psa_state_idx_d] += d;
  state[k_ra_psa_state_idx_e] += e;
  state[k_ra_psa_state_idx_f] += f;
  state[k_ra_psa_state_idx_g] += g;
  state[k_ra_psa_state_idx_h] += h;
}

/** @brief Process one 64-byte block into the hash ``state``. */
static void internal_sha256_block(uint32_t      state[k_ra_psa_sha256_state_words],
                                  const uint8_t block[k_ra_psa_sha256_block_bytes])
{
  uint32_t schedule[k_ra_psa_sha256_schedule_len];
  internal_sha256_schedule(schedule, block);
  internal_sha256_rounds(state, schedule);
}

/** @brief One-shot SHA-256 over ``(in, in_len)`` -> 32-byte ``out``. */
static void
internal_sha256_oneshot(const uint8_t* in, size_t in_len, uint8_t out[k_ra_psa_sha256_len])
{
  uint32_t state[k_ra_psa_sha256_state_words] = {
    k_ra_psa_sha256_h0,
    k_ra_psa_sha256_h1,
    k_ra_psa_sha256_h2,
    k_ra_psa_sha256_h3,
    k_ra_psa_sha256_h4,
    k_ra_psa_sha256_h5,
    k_ra_psa_sha256_h6,
    k_ra_psa_sha256_h7,
  };

  /* Process complete blocks. */
  const size_t   complete_blocks = in_len / (size_t)k_ra_psa_sha256_block_bytes;
  const size_t   remaining       = in_len - (complete_blocks * (size_t)k_ra_psa_sha256_block_bytes);
  const uint8_t* p               = in;
  for (size_t b = 0U; b < complete_blocks; ++b) {
    internal_sha256_block(state, p);
    p += (size_t)k_ra_psa_sha256_block_bytes;
  }

  /* Final block(s) with padding. ``remaining`` is guaranteed < 64 by the
   * arithmetic above; the explicit clamp keeps clang-analyzer's bound
   * tracking happy when reading the tail byte-copy loop. */
  uint8_t      tail[k_ra_psa_sha256_pad_buf_len] = {};
  const size_t tail_copy_len                     = (remaining < (size_t)k_ra_psa_sha256_block_bytes)
                                                     ? remaining
                                                     : (size_t)k_ra_psa_sha256_block_bytes - 1U;
  (void)memcpy(tail, p, tail_copy_len);
  tail[tail_copy_len]        = (uint8_t)k_ra_psa_pad_marker;
  const size_t   tail_blocks = (remaining < (size_t)k_ra_psa_sha256_pad_threshold) ? 1U : 2U;
  const uint64_t bitlen      = (uint64_t)in_len * (uint64_t)k_ra_psa_byte_bits;
  const size_t   tail_len    = tail_blocks * (size_t)k_ra_psa_sha256_block_bytes;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_length_field_bytes; ++i) {
    tail[tail_len - 1U - i] = (uint8_t)(bitlen >> (i * (uint32_t)k_ra_psa_byte_bits));
  }
  for (size_t b = 0U; b < tail_blocks; ++b) {
    internal_sha256_block(state, &tail[b * (size_t)k_ra_psa_sha256_block_bytes]);
  }

  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_sha256_state_words; ++i) {
    out[(i * (uint32_t)k_ra_psa_bytes_per_word) + 0U] = (uint8_t)(state[i] >> k_ra_psa_shift_b3);
    out[(i * (uint32_t)k_ra_psa_bytes_per_word) + 1U] = (uint8_t)(state[i] >> k_ra_psa_shift_b2);
    out[(i * (uint32_t)k_ra_psa_bytes_per_word) + 2U] = (uint8_t)(state[i] >> k_ra_psa_shift_b1);
    out[(i * (uint32_t)k_ra_psa_bytes_per_word) + 3U] = (uint8_t)(state[i]);
  }
}

/**
 * @brief Compute the AEAD tag stand-in for the simulator path.
 *
 * @details
 * Real AES-GCM produces a 16-byte tag from key, nonce, ciphertext and
 * AAD. The simulator replaces it with SHA-256(key || nonce || aad ||
 * cipher) truncated to 16 bytes. This is *not* cryptographically
 * secure but reproduces the same input dependencies, so tampering any
 * byte changes the tag and the decrypt-side check fails.
 */
static void internal_sim_aead_tag(const uint8_t* key,
                                  size_t         key_len,
                                  const uint8_t* nonce,
                                  size_t         nonce_len,
                                  const uint8_t* aad,
                                  size_t         aad_len,
                                  const uint8_t* cipher,
                                  size_t         cipher_len,
                                  uint8_t        out_tag[k_ra_psa_gcm_tag_len])
{
  uint8_t buf[k_ra_psa_sim_scratch_bytes];
  size_t  off = 0U;
  for (size_t i = 0U; (i < key_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = key[i];
  }
  for (size_t i = 0U; (i < nonce_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = nonce[i];
  }
  for (size_t i = 0U; (i < aad_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = aad[i];
  }
  for (size_t i = 0U; (i < cipher_len) && (off < sizeof(buf)); ++i) {
    buf[off++] = cipher[i];
  }
  uint8_t digest[k_ra_psa_sha256_len];
  internal_sha256_oneshot(buf, off, digest);
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_gcm_tag_len; ++i) {
    out_tag[i] = digest[i];
  }
}

/**
 * @brief Generate the keystream byte for offset ``i`` (sim AES-GCM).
 *
 * @details
 * Derives a per-position keystream from SHA-256(key || nonce || "ks").
 * Pseudo-AES, sufficient for round-trip tests.
 */
static void internal_sim_keystream(const uint8_t* key,
                                   size_t         key_len,
                                   const uint8_t* nonce,
                                   size_t         nonce_len,
                                   uint8_t*       dst,
                                   size_t         len)
{
  uint8_t seed[k_ra_psa_sim_scratch_bytes];
  size_t  off = 0U;
  for (size_t i = 0U; (i < key_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = key[i];
  }
  for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = nonce[i];
  }
  uint8_t  block[k_ra_psa_sha256_len];
  uint32_t counter  = 0U;
  size_t   produced = 0U;
  while (produced < len) {
    seed[off]      = (uint8_t)(counter >> k_ra_psa_shift_b3);
    seed[off + 1U] = (uint8_t)(counter >> k_ra_psa_shift_b2);
    seed[off + 2U] = (uint8_t)(counter >> k_ra_psa_shift_b1);
    seed[off + 3U] = (uint8_t)counter;
    internal_sha256_oneshot(seed, off + (size_t)k_ra_psa_bytes_per_word, block);
    const size_t take = ((len - produced) < (size_t)k_ra_psa_sha256_len)
                          ? (len - produced)
                          : (size_t)k_ra_psa_sha256_len;
    for (size_t i = 0U; i < take; ++i) {
      dst[produced + i] = block[i];
    }
    produced += take;
    ++counter;
  }
}

/**
 * @brief Simulator AEAD encrypt body shared by ``ra_psa_aead_encrypt``.
 *
 * @details
 * Encrypts ``plain`` into ``out`` with the simulator XOR keystream and
 * appends a 16-byte SHA-256 tag at ``out[plain_len..plain_len+16)``.
 */
static ra_err_t internal_sim_aead_encrypt(const struct ra_psa_key_handle* slot,
                                          const uint8_t*                  nonce,
                                          size_t                          nonce_len,
                                          const uint8_t*                  aad,
                                          size_t                          aad_len,
                                          const uint8_t*                  plain,
                                          size_t                          plain_len,
                                          uint8_t*                        out,
                                          size_t*                         out_len)
{
  if (plain_len > 0U) {
    uint8_t ks[k_ra_psa_sim_scratch_bytes];
    if (plain_len > sizeof(ks)) {
      return k_ra_err_invalid_size;
    }
    internal_sim_keystream(slot->key, slot->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(plain[i] ^ ks[i]);
    }
  }
  internal_sim_aead_tag(slot->key,
                        slot->key_len,
                        nonce,
                        nonce_len,
                        aad,
                        aad_len,
                        out,
                        plain_len,
                        &out[plain_len]);
  *out_len = plain_len + (size_t)k_ra_psa_gcm_tag_len;
  return k_ra_ok;
}

/**
 * @brief Simulator AEAD decrypt body shared by ``ra_psa_aead_decrypt``.
 *
 * @details
 * Verifies the 16-byte tag at the tail of ``cipher`` and, on success,
 * writes ``plain_len`` bytes of plaintext into ``out``.
 */
static ra_err_t internal_sim_aead_decrypt(const struct ra_psa_key_handle* slot,
                                          const uint8_t*                  nonce,
                                          size_t                          nonce_len,
                                          const uint8_t*                  aad,
                                          size_t                          aad_len,
                                          const uint8_t*                  cipher,
                                          size_t                          plain_len,
                                          uint8_t*                        out,
                                          size_t*                         out_len)
{
  uint8_t expected_tag[k_ra_psa_gcm_tag_len];
  internal_sim_aead_tag(slot->key,
                        slot->key_len,
                        nonce,
                        nonce_len,
                        aad,
                        aad_len,
                        cipher,
                        plain_len,
                        expected_tag);
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_gcm_tag_len; ++i) {
    diff |= (uint8_t)(expected_tag[i] ^ cipher[plain_len + i]);
  }
  if (diff != 0U) {
    return k_ra_err_crc_mismatch;
  }
  if (plain_len > 0U) {
    uint8_t ks[k_ra_psa_sim_scratch_bytes];
    if (plain_len > sizeof(ks)) {
      return k_ra_err_invalid_size;
    }
    internal_sim_keystream(slot->key, slot->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(cipher[i] ^ ks[i]);
    }
  }
  *out_len = plain_len;
  return k_ra_ok;
}

#endif /* RA_SIMULATOR_MODE */

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_psa_crypto_init(void)
{
  if (s_initialised) {
    return k_ra_err_exists;
  }

#ifndef RA_SIMULATOR_MODE
  const psa_status_t st = psa_crypto_init();
  if (st != PSA_SUCCESS) {
    ra_log_error(k_ra_psa_tag, "psa_crypto_init failed");
    return k_ra_err_hw_error;
  }
#endif

  for (uint8_t i = 0U; i < (uint8_t)k_ra_psa_max_keys; ++i) {
    s_key_pool[i].in_use  = false;
    s_key_pool[i].key_len = 0U;
  }

  s_initialised = true;
  return k_ra_ok;
}

ra_err_t ra_psa_crypto_deinit(void)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }

  for (uint8_t i = 0U; i < (uint8_t)k_ra_psa_max_keys; ++i) {
    if (s_key_pool[i].in_use) {
#ifndef RA_SIMULATOR_MODE
      (void)psa_destroy_key(s_key_pool[i].psa_id);
#endif
      (void)memset(s_key_pool[i].key, 0, sizeof(s_key_pool[i].key));
      s_key_pool[i].in_use  = false;
      s_key_pool[i].key_len = 0U;
    }
  }

#ifndef RA_SIMULATOR_MODE
  mbedtls_psa_crypto_free();
#endif

  s_initialised = false;
  return k_ra_ok;
}

#ifndef RA_SIMULATOR_MODE

/** @brief Translate ``ra_psa_key_type_t`` to its ``PSA_KEY_TYPE_*`` peer. */
static psa_key_type_t internal_map_key_type(ra_psa_key_type_t type)
{
  switch (type) {
    case k_ra_psa_key_type_aes:
      return PSA_KEY_TYPE_AES;
    case k_ra_psa_key_type_hmac:
      return PSA_KEY_TYPE_HMAC;
    case k_ra_psa_key_type_ecc_p256_priv:
      return PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
    case k_ra_psa_key_type_ecc_p256_pub:
      return PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);
    case k_ra_psa_key_type_raw:
    default:
      return PSA_KEY_TYPE_RAW_DATA;
  }
}

/** @brief Translate ``ra_psa_alg_t`` to its ``PSA_ALG_*`` peer. */
static psa_algorithm_t internal_map_alg(ra_psa_alg_t alg)
{
  switch (alg) {
    case k_ra_psa_alg_aes_gcm:
      return PSA_ALG_GCM;
    case k_ra_psa_alg_ecdsa_sha_256:
      return PSA_ALG_ECDSA(PSA_ALG_SHA_256);
    case k_ra_psa_alg_sha_256:
      return PSA_ALG_SHA_256;
    case k_ra_psa_alg_none:
    default:
      return 0;
  }
}

/** @brief Translate ``ra_psa_key_usage_t`` bitmask to ``PSA_KEY_USAGE_*`` flags. */
static psa_key_usage_t internal_map_usage(ra_psa_key_usage_t usage)
{
  psa_key_usage_t out = 0;
  if ((usage & k_ra_psa_usage_sign) != 0U) {
    out |= PSA_KEY_USAGE_SIGN_HASH;
  }
  if ((usage & k_ra_psa_usage_verify) != 0U) {
    out |= PSA_KEY_USAGE_VERIFY_HASH;
  }
  if ((usage & k_ra_psa_usage_encrypt) != 0U) {
    out |= PSA_KEY_USAGE_ENCRYPT;
  }
  if ((usage & k_ra_psa_usage_decrypt) != 0U) {
    out |= PSA_KEY_USAGE_DECRYPT;
  }
  if ((usage & k_ra_psa_usage_derive) != 0U) {
    out |= PSA_KEY_USAGE_DERIVE;
  }
  return out;
}

/** @brief Run the PSA-side import + slot wire-up for ``ra_psa_key_import``. */
static ra_err_t internal_psa_import_into_slot(struct ra_psa_key_handle* slot,
                                              const ra_psa_key_attr_t*  attr,
                                              const uint8_t*            data,
                                              size_t                    data_len)
{
  psa_key_attributes_t pa = psa_key_attributes_init();
  psa_set_key_type(&pa, internal_map_key_type(attr->type));
  psa_set_key_algorithm(&pa, internal_map_alg(attr->alg));
  psa_set_key_usage_flags(&pa, internal_map_usage(attr->usage));

  psa_key_id_t       kid = 0;
  const psa_status_t st  = psa_import_key(&pa, data, data_len, &kid);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  slot->psa_id = kid;
  return k_ra_ok;
}

#endif /* !RA_SIMULATOR_MODE */

ra_err_t ra_psa_key_import(ra_psa_key_t*            out_handle,
                           const ra_psa_key_attr_t* attr,
                           const uint8_t*           data,
                           size_t                   data_len)
{
  if (out_handle != NULL) {
    *out_handle = NULL;
  }
  if ((out_handle == NULL) || (attr == NULL) || (data == NULL) || (data_len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (data_len > (size_t)k_ra_psa_max_key_bytes) {
    return k_ra_err_invalid_size;
  }
  if (attr->usage == k_ra_psa_usage_none) {
    return k_ra_err_invalid_arg;
  }

  struct ra_psa_key_handle* slot = internal_alloc_slot();
  if (slot == NULL) {
    return k_ra_err_no_mem;
  }

#ifndef RA_SIMULATOR_MODE
  const ra_err_t pst = internal_psa_import_into_slot(slot, attr, data, data_len);
  if (pst != k_ra_ok) {
    return pst;
  }
#endif

  slot->in_use = true;
  slot->attr   = *attr;
  (void)memcpy(slot->key, data, data_len);
  slot->key_len = data_len;

  *out_handle = slot;
  return k_ra_ok;
}

ra_err_t ra_psa_key_destroy(ra_psa_key_t handle)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra_err_invalid_arg;
  }

#ifndef RA_SIMULATOR_MODE
  (void)psa_destroy_key(handle->psa_id);
#endif
  (void)memset(handle->key, 0, sizeof(handle->key));
  handle->key_len = 0U;
  handle->in_use  = false;
  return k_ra_ok;
}

ra_err_t ra_psa_hash_compute(ra_psa_alg_t   alg,
                             const uint8_t* input,
                             size_t         input_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len)
{
  if (out_len != NULL) {
    *out_len = 0U;
  }
  if ((out == NULL) || (out_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if ((input == NULL) && (input_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (alg != k_ra_psa_alg_sha_256) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (out_cap < (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_invalid_size;
  }

#ifdef RA_SIMULATOR_MODE
  internal_sha256_oneshot(input, input_len, out);
#else
  size_t             produced = 0U;
  const psa_status_t st =
    psa_hash_compute(PSA_ALG_SHA_256, input, input_len, out, out_cap, &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  (void)produced;
#endif

  *out_len = (size_t)k_ra_psa_sha256_len;
  return k_ra_ok;
}

ra_err_t ra_psa_sign_hash(ra_psa_key_t   handle,
                          ra_psa_alg_t   alg,
                          const uint8_t* hash,
                          size_t         hash_len,
                          uint8_t*       sig,
                          size_t         sig_cap,
                          size_t*        sig_len)
{
  if (sig_len != NULL) {
    *sig_len = 0U;
  }
  if ((hash == NULL) || (sig == NULL) || (sig_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra_err_invalid_arg;
  }
  if (alg != k_ra_psa_alg_ecdsa_sha_256) {
    return k_ra_err_not_supported;
  }
  if ((handle->attr.usage & k_ra_psa_usage_sign) == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (hash_len != (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_invalid_size;
  }
  if (sig_cap < (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_invalid_size;
  }

#ifdef RA_SIMULATOR_MODE
  /* Sim "signature" = SHA-256(key || hash). 32 bytes. */
  uint8_t buf[k_ra_psa_max_key_bytes + k_ra_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  internal_sha256_oneshot(buf, handle->key_len + hash_len, digest);
  (void)memcpy(sig, digest, sizeof(digest));
  *sig_len = sizeof(digest);
#else
  size_t             produced = 0U;
  const psa_status_t st       = psa_sign_hash(handle->psa_id,
                                              PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                                              hash,
                                              hash_len,
                                              sig,
                                              sig_cap,
                                              &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  *sig_len = produced;
#endif

  return k_ra_ok;
}

ra_err_t ra_psa_verify_hash(ra_psa_key_t   handle,
                            ra_psa_alg_t   alg,
                            const uint8_t* hash,
                            size_t         hash_len,
                            const uint8_t* sig,
                            size_t         sig_len)
{
  if ((hash == NULL) || (sig == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra_err_invalid_arg;
  }
  if (alg != k_ra_psa_alg_ecdsa_sha_256) {
    return k_ra_err_not_supported;
  }
  if ((handle->attr.usage & k_ra_psa_usage_verify) == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (hash_len != (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_invalid_size;
  }

#ifdef RA_SIMULATOR_MODE
  if (sig_len != (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_crc_mismatch;
  }
  uint8_t buf[k_ra_psa_max_key_bytes + k_ra_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  internal_sha256_oneshot(buf, handle->key_len + hash_len, digest);
  /* Constant-time compare. */
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_sha256_len; ++i) {
    diff |= (uint8_t)(digest[i] ^ sig[i]);
  }
  return (diff == 0U) ? k_ra_ok : k_ra_err_crc_mismatch;
#else
  const psa_status_t st =
    psa_verify_hash(handle->psa_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len, sig, sig_len);
  if (st == PSA_SUCCESS) {
    return k_ra_ok;
  }
  if (st == PSA_ERROR_INVALID_SIGNATURE) {
    return k_ra_err_crc_mismatch;
  }
  return k_ra_err_hw_error;
#endif
}

/** @brief Shared precondition checks for ``ra_psa_aead_encrypt``. */
static ra_err_t internal_aead_encrypt_check(ra_psa_key_t   handle,
                                            ra_psa_alg_t   alg,
                                            const uint8_t* nonce,
                                            size_t         nonce_len,
                                            const uint8_t* aad,
                                            size_t         aad_len,
                                            const uint8_t* plain,
                                            size_t         plain_len,
                                            const uint8_t* out,
                                            size_t         out_cap,
                                            const size_t*  out_len)
{
  if ((nonce == NULL) || (out == NULL) || (out_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if (((plain == NULL) && (plain_len != 0U)) || ((aad == NULL) && (aad_len != 0U))) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle) || (alg != k_ra_psa_alg_aes_gcm) ||
      ((handle->attr.usage & k_ra_psa_usage_encrypt) == 0U)) {
    return k_ra_err_invalid_arg;
  }
  if ((nonce_len != (size_t)k_ra_psa_gcm_nonce_len) ||
      (out_cap < (plain_len + (size_t)k_ra_psa_gcm_tag_len))) {
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

ra_err_t ra_psa_aead_encrypt(ra_psa_key_t   handle,
                             ra_psa_alg_t   alg,
                             const uint8_t* nonce,
                             size_t         nonce_len,
                             const uint8_t* aad,
                             size_t         aad_len,
                             const uint8_t* plain,
                             size_t         plain_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len)
{
  if (out_len != NULL) {
    *out_len = 0U;
  }
  const ra_err_t ck = internal_aead_encrypt_check(handle,
                                                  alg,
                                                  nonce,
                                                  nonce_len,
                                                  aad,
                                                  aad_len,
                                                  plain,
                                                  plain_len,
                                                  out,
                                                  out_cap,
                                                  out_len);
  if (ck != k_ra_ok) {
    return ck;
  }

#ifdef RA_SIMULATOR_MODE
  /* Encrypt = plaintext XOR keystream(key, nonce). Tag = SHA-256-trunc16. */
  const ra_err_t ser = internal_sim_aead_encrypt(handle,
                                                 nonce,
                                                 nonce_len,
                                                 aad,
                                                 aad_len,
                                                 plain,
                                                 plain_len,
                                                 out,
                                                 out_len);
  if (ser != k_ra_ok) {
    return ser;
  }
#else
  size_t             produced = 0U;
  const psa_status_t st       = psa_aead_encrypt(handle->psa_id,
                                                 PSA_ALG_GCM,
                                                 nonce,
                                                 nonce_len,
                                                 aad,
                                                 aad_len,
                                                 plain,
                                                 plain_len,
                                                 out,
                                                 out_cap,
                                                 &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  *out_len = produced;
#endif

  return k_ra_ok;
}

/** @brief Shared precondition checks for ``ra_psa_aead_decrypt``. */
static ra_err_t internal_aead_decrypt_check(ra_psa_key_t   handle,
                                            ra_psa_alg_t   alg,
                                            const uint8_t* nonce,
                                            size_t         nonce_len,
                                            const uint8_t* aad,
                                            size_t         aad_len,
                                            const uint8_t* cipher,
                                            size_t         cipher_len,
                                            const uint8_t* out,
                                            size_t         out_cap,
                                            const size_t*  out_len,
                                            size_t*        out_plain_len)
{
  if ((nonce == NULL) || (cipher == NULL) || (out_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if ((aad == NULL) && (aad_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle) || (alg != k_ra_psa_alg_aes_gcm) ||
      ((handle->attr.usage & k_ra_psa_usage_decrypt) == 0U)) {
    return k_ra_err_invalid_arg;
  }
  if ((nonce_len != (size_t)k_ra_psa_gcm_nonce_len) ||
      (cipher_len < (size_t)k_ra_psa_gcm_tag_len)) {
    return k_ra_err_invalid_size;
  }
  const size_t plain_len = cipher_len - (size_t)k_ra_psa_gcm_tag_len;
  if ((out == NULL) && (plain_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (out_cap < plain_len) {
    return k_ra_err_invalid_size;
  }
  *out_plain_len = plain_len;
  return k_ra_ok;
}

ra_err_t ra_psa_aead_decrypt(ra_psa_key_t   handle,
                             ra_psa_alg_t   alg,
                             const uint8_t* nonce,
                             size_t         nonce_len,
                             const uint8_t* aad,
                             size_t         aad_len,
                             const uint8_t* cipher,
                             size_t         cipher_len,
                             uint8_t*       out,
                             size_t         out_cap,
                             size_t*        out_len)
{
  if (out_len != NULL) {
    *out_len = 0U;
  }
  size_t         plain_len = 0U;
  const ra_err_t ck        = internal_aead_decrypt_check(handle,
                                                         alg,
                                                         nonce,
                                                         nonce_len,
                                                         aad,
                                                         aad_len,
                                                         cipher,
                                                         cipher_len,
                                                         out,
                                                         out_cap,
                                                         out_len,
                                                         &plain_len);
  if (ck != k_ra_ok) {
    return ck;
  }

#ifdef RA_SIMULATOR_MODE
  const ra_err_t sdr = internal_sim_aead_decrypt(handle,
                                                 nonce,
                                                 nonce_len,
                                                 aad,
                                                 aad_len,
                                                 cipher,
                                                 plain_len,
                                                 out,
                                                 out_len);
  if (sdr != k_ra_ok) {
    return sdr;
  }
#else
  size_t             produced = 0U;
  const psa_status_t st       = psa_aead_decrypt(handle->psa_id,
                                                 PSA_ALG_GCM,
                                                 nonce,
                                                 nonce_len,
                                                 aad,
                                                 aad_len,
                                                 cipher,
                                                 cipher_len,
                                                 out,
                                                 out_cap,
                                                 &produced);
  if (st == PSA_ERROR_INVALID_SIGNATURE) {
    return k_ra_err_crc_mismatch;
  }
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  *out_len = produced;
#endif

  return k_ra_ok;
}
