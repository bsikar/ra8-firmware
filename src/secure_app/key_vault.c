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
 * - ``ra8_key_vault_init`` zero every slot.
 * - ``ra8_key_vault_store`` copy a 32-byte key into a slot.
 * - ``ra8_key_vault_sha256_xor_challenge`` compute
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

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_secure.h"

static const char* s_tag = "KEYV";

/*
 * Fail-closed stub-crypto gate (issue #180). The SHA-256 below is real
 * (FIPS 180-4), but the surrounding key vault is a software placeholder that
 * holds symmetric keys in a plain secure-SRAM array with no hardware-backed
 * protection -- a stand-in for a real key store. It is only meant for host
 * simulation or an explicitly-declared insecure dev/eval image. A real
 * production/HIL image (neither flag set) compiles the #else branch, where
 * every entry point hard-errors so the placeholder vault cannot be relied on.
 * scripts/checks/check_stub_crypto_guarded.py enforces the guard.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)

/** @brief SHA-256 dimensions (FIPS 180-4). */
typedef enum : uint16_t {
  k_sha256_msg_bytes   = 32U, /**< Fixed 32-byte (256-bit) vault input. */
  k_sha256_words       = 64U, /**< Schedule words / compression rounds. */
  k_sha256_block_bytes = 64U, /**< One SHA-256 message block in bytes.  */
} sha256_dim_t;

/** @brief SHA-256 padding layout and big-endian packing. */
typedef enum : uint8_t {
  k_sha256_pad_pos        = 32U,   /**< First padding byte (== message length).       */
  k_sha256_pad_marker     = 0x80U, /**< Padding marker 0b1000_0000.                   */
  k_sha256_pad_zero_start = 33U,   /**< First zero-fill byte after the marker.        */
  k_sha256_len_b0         = 56U,   /**< Sha256 length b0.                             */
  k_sha256_len_b1         = 57U,   /**< Sha256 length b1.                             */
  k_sha256_len_b2         = 58U,   /**< Sha256 length b2.                             */
  k_sha256_len_b3         = 59U,   /**< Sha256 length b3.                             */
  k_sha256_len_b4         = 60U,   /**< Sha256 length b4.                             */
  k_sha256_len_b5         = 61U,   /**< Sha256 length b5.                             */
  k_sha256_len_b6         = 62U,   /**< Sha256 length b6.                             */
  k_sha256_len_b7         = 63U,   /**< 64-bit big-endian length.                     */
  k_sha256_len_hi         = 0x01U, /**< 256-bit length high byte (0x0100).            */
  k_sha256_byte_shift     = 24U,   /**< Byte-3 shift for BE word pack (8/16 ignored). */
} sha256_pad_t;

/** @brief FIPS 180-4 4.1.2 sigma rotation/shift amounts and back-refs. */
typedef enum : uint8_t {
  k_sha256_ssig0_r0 = 7U,  /**< Sha256 ssig0 r0. */
  k_sha256_ssig0_r1 = 18U, /**< Sha256 ssig0 r1. */
  k_sha256_ssig0_sh = 3U,  /**< Sha256 ssig0 sh. */
  k_sha256_ssig1_r0 = 17U, /**< Sha256 ssig1 r0. */
  k_sha256_ssig1_r1 = 19U, /**< Sha256 ssig1 r1. */
  k_sha256_ssig1_sh = 10U, /**< Sha256 ssig1 sh. */
  k_sha256_bsig1_r0 = 6U,  /**< Sha256 bsig1 r0. */
  k_sha256_bsig1_r1 = 11U, /**< Sha256 bsig1 r1. */
  k_sha256_bsig1_r2 = 25U, /**< Sha256 bsig1 r2. */
  k_sha256_bsig0_r0 = 2U,  /**< Sha256 bsig0 r0. */
  k_sha256_bsig0_r1 = 13U, /**< Sha256 bsig0 r1. */
  k_sha256_bsig0_r2 = 22U, /**< Sha256 bsig0 r2. */
  k_sha256_back15   = 15U, /**< Sha256 back15.   */
  k_sha256_back7    = 7U,  /**< Sha256 back7.    */
} sha256_rot_t;

/** @brief SHA-256 hash-state word indices (a..h). */
typedef enum : uint8_t {
  k_sha256_st_a = 0U, /**< Sha256 st a. */
  k_sha256_st_b = 1U, /**< Sha256 st b. */
  k_sha256_st_c = 2U, /**< Sha256 st c. */
  k_sha256_st_d = 3U, /**< Sha256 st d. */
  k_sha256_st_e = 4U, /**< Sha256 st e. */
  k_sha256_st_f = 5U, /**< Sha256 st f. */
  k_sha256_st_g = 6U, /**< Sha256 st g. */
  k_sha256_st_h = 7U, /**< Sha256 st h. */
} sha256_state_idx_t;

/**
 * @struct ra8_key_vault_slot_t
 * @brief A single symmetric-key slot.
 */
typedef struct {
  uint8_t key[k_ra8_key_vault_key_bytes]; /**< Key. */
} ra8_key_vault_slot_t;

static ra8_key_vault_slot_t s_vault[k_ra8_key_vault_slots];

/** @brief Accepted key-authentication-key (KAK) lengths (AES-128 / AES-256). */
typedef enum : uint16_t {
  k_kv_mac_key_128 = 16U, /**< AES-128 KAK length. */
  k_kv_mac_key_256 = 32U, /**< AES-256 KAK length. */
} ra8_kv_mac_key_len_t;

/**
 * @var s_mac_key
 * @brief Secret key-authentication key (KAK) that keys the import-blob CMAC.
 *
 * @details Dedicated storage held *outside* the ``s_vault`` slot array, so it
 * is never reachable through the Non-Secure key-import path. Provisioned by
 * ``ra8_key_vault_set_mac_key`` and read only by the secure-side importer via
 * ``ra8_key_vault_load_mac_key``.
 *
 * @note Direct modification from outside this TU is forbidden.
 * @warning Secret material; never expose across the S/NS boundary.
 * @since 0.1.0
 */
static uint8_t s_mac_key[k_ra8_key_vault_mac_key_bytes];

/**
 * @var s_mac_key_len
 * @brief Length in bytes of the provisioned KAK, or 0 when unprovisioned.
 *
 * @note Direct modification from outside this TU is forbidden.
 * @warning A zero value means no KAK is available and imports cannot verify.
 * @since 0.1.0
 */
static uint16_t s_mac_key_len = 0U;

/* =============================================================================
 * Tiny SHA-256 (single-block, 32-byte input)
 * =============================================================================
 *
 * Implements just enough of FIPS 180-4 to compute SHA-256 over a
 * 32-byte input. Used by ra8_key_vault_sha256_xor_challenge to
 * scramble the key in a way that does not reveal the key bytes.
 *
 * The SHA-256 sponge is a transcription of the FIPS 180-4 spec.
 * Every magic number below comes directly from the standard;
 * giving them names would inflate the code without adding clarity.
 */

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

/**
 * @brief 32-bit rotate-right primitive used by the SHA-256 sponge.
 *
 * @details
 * Hot helper for the FIPS 180-4 message-schedule and round-function
 * mixing. Inlined by the compiler under -O2.
 *
 * @param[in] x Source 32-bit word.
 * @param[in] n Rotate amount; only the low 5 bits are meaningful.
 *
 * @return ``x`` rotated right by ``n mod 32`` bits.
 * @retval ``x`` when ``n mod 32 == 0``.
 *
 * @pre Caller treats this as a pure expression.
 * @pre ``n`` may take any uint32_t value.
 * @post No state is mutated.
 * @post Return value depends only on the parameters.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (32U - n));
}

/* CITES-OK: constant-time decomposition. internal_sha256_32 is split into
 * three helpers (build_block, schedule, compress). Each helper iterates a
 * fixed loop count with no data-dependent branches; the only inputs whose
 * values steer control flow are loop indices, which are compile-time
 * constants. Side-channel posture is identical to the monolithic form. */

/**
 * @brief Assemble the SHA-256 single-block padding for a 32-byte message.
 *
 * @details
 * Writes 32 input bytes, the 0x80 terminator, zero-fill, and the fixed
 * 256-bit big-endian length suffix into the caller's 64-byte block.
 * The control flow is input-independent (two fixed-bound loops and a
 * constant tail), preserving the constant-time property required of
 * the secure-side digest.
 *
 * @param[in]  in32  32-byte input message.
 * @param[out] block 64-byte padded block.
 *
 * @pre ``in32`` and ``block`` are non-NULL.
 * @pre ``block`` spans at least 64 bytes.
 * @post ``block`` contains the FIPS 180-4 padded single-block message.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sha256_build_block(const uint8_t* in32, uint8_t* block)
{
  /* Build the 64-byte block: 32 input bytes, 0x80, zeros, then
   * the 64-bit length in bits at the end (SHA-256 uses BIG-endian
   * length). */
  for (uint32_t i = 0U; i < k_sha256_msg_bytes; ++i) {
    block[i] = in32[i];
  }
  block[k_sha256_pad_pos] = k_sha256_pad_marker;
  for (uint32_t i = k_sha256_pad_zero_start; i < k_sha256_len_b0; ++i) {
    block[i] = 0U;
  }
  /* Length = 32 bytes = 256 bits = 0x0100 big-endian. */
  block[k_sha256_len_b0] = 0U;
  block[k_sha256_len_b1] = 0U;
  block[k_sha256_len_b2] = 0U;
  block[k_sha256_len_b3] = 0U;
  block[k_sha256_len_b4] = 0U;
  block[k_sha256_len_b5] = 0U;
  block[k_sha256_len_b6] = k_sha256_len_hi;
  block[k_sha256_len_b7] = 0x00U;
}

/**
 * @brief Expand a padded 64-byte block into the 64-word message schedule.
 *
 * @details
 * Loads the first 16 words big-endian, then derives words 16..63 with
 * the standard sigma0/sigma1 mixing. All loops have constant bounds and
 * the body has no data-dependent branches, so this helper is
 * constant-time in the same sense as the monolithic original.
 *
 * @param[in]  block 64-byte padded message block.
 * @param[out] w     64-word message schedule.
 *
 * @pre ``block`` and ``w`` are non-NULL.
 * @pre ``block`` spans 64 bytes and ``w`` spans 64 words.
 * @post ``w`` holds the SHA-256 schedule for ``block``.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sha256_schedule(const uint8_t* block, uint32_t* w)
{
  for (uint32_t i = 0U; i < 16U; ++i) {
    w[i] = ((uint32_t)block[(i * 4U) + 0U] << k_sha256_byte_shift) |
           ((uint32_t)block[(i * 4U) + 1U] << 16U) | ((uint32_t)block[(i * 4U) + 2U] << 8U) |
           ((uint32_t)block[(i * 4U) + 3U]);
  }
  for (uint32_t i = 16U; i < k_sha256_words; ++i) {
    const uint32_t s0 = internal_rotr(w[i - k_sha256_back15], k_sha256_ssig0_r0) ^
                        internal_rotr(w[i - k_sha256_back15], k_sha256_ssig0_r1) ^
                        (w[i - k_sha256_back15] >> k_sha256_ssig0_sh);
    const uint32_t s1 = internal_rotr(w[i - 2U], k_sha256_ssig1_r0) ^
                        internal_rotr(w[i - 2U], k_sha256_ssig1_r1) ^
                        (w[i - 2U] >> k_sha256_ssig1_sh);
    w[i]              = w[i - 16U] + s0 + w[i - k_sha256_back7] + s1;
  }
}

/**
 * @brief Run the 64-round SHA-256 compression and emit the digest.
 *
 * @details
 * Compresses the message schedule into the initial hash value and
 * serialises the eight 32-bit state words into the caller's 32-byte
 * output buffer big-endian. The round loop iterates a fixed 64 times
 * with no data-dependent branches; the final serialisation is two
 * constant-bound loops. Constant-time posture identical to the
 * monolithic original.
 *
 * @param[in]  w     64-word SHA-256 message schedule.
 * @param[out] out32 32-byte digest output.
 *
 * @pre ``w`` and ``out32`` are non-NULL.
 * @pre ``out32`` spans at least 32 bytes.
 * @post ``out32`` contains the SHA-256 digest.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sha256_compress(const uint32_t* w, uint8_t* out32)
{
  uint32_t a = k_sha256_h0[k_sha256_st_a];
  uint32_t b = k_sha256_h0[k_sha256_st_b];
  uint32_t c = k_sha256_h0[k_sha256_st_c];
  uint32_t d = k_sha256_h0[k_sha256_st_d];
  uint32_t e = k_sha256_h0[k_sha256_st_e];
  uint32_t f = k_sha256_h0[k_sha256_st_f];
  uint32_t g = k_sha256_h0[k_sha256_st_g];
  uint32_t h = k_sha256_h0[k_sha256_st_h];

  for (uint32_t i = 0U; i < k_sha256_words; ++i) {
    const uint32_t s1 = internal_rotr(e, k_sha256_bsig1_r0) ^ internal_rotr(e, k_sha256_bsig1_r1) ^
                        internal_rotr(e, k_sha256_bsig1_r2);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + k_sha256_k[i] + w[i];
    const uint32_t s0  = internal_rotr(a, k_sha256_bsig0_r0) ^ internal_rotr(a, k_sha256_bsig0_r1) ^
                         internal_rotr(a, k_sha256_bsig0_r2);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
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
    a + k_sha256_h0[k_sha256_st_a],
    b + k_sha256_h0[k_sha256_st_b],
    c + k_sha256_h0[k_sha256_st_c],
    d + k_sha256_h0[k_sha256_st_d],
    e + k_sha256_h0[k_sha256_st_e],
    f + k_sha256_h0[k_sha256_st_f],
    g + k_sha256_h0[k_sha256_st_g],
    h + k_sha256_h0[k_sha256_st_h],
  };
  for (uint32_t i = 0U; i < 8U; ++i) {
    out32[(i * 4U) + 0U] = (uint8_t)(hh[i] >> k_sha256_byte_shift);
    out32[(i * 4U) + 1U] = (uint8_t)(hh[i] >> 16U);
    out32[(i * 4U) + 2U] = (uint8_t)(hh[i] >> 8U);
    out32[(i * 4U) + 3U] = (uint8_t)(hh[i]);
  }
}

/**
 * @brief Hash a 32-byte input. Padding produces a single 64-byte block.
 *
 * @details
 * Implements just enough of FIPS 180-4 to digest exactly 32 bytes by
 * orchestrating the three constant-time helpers
 * ::internal_sha256_build_block, ::internal_sha256_schedule and
 * ::internal_sha256_compress. Used by
 * ::ra8_key_vault_sha256_xor_challenge to scramble a per-key XOR
 * result without leaking the underlying key bytes.
 *
 * @param[in]  in32  32-byte input message.
 * @param[out] out32 32-byte SHA-256 digest.
 *
 * @pre ``in32`` and ``out32`` are non-NULL.
 * @pre Both buffers span at least 32 bytes.
 * @post ``out32`` contains the SHA-256 digest of ``in32``.
 * @post No global state is mutated.
 *
 * @note Pure compute function; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sha256_32(const uint8_t* in32, uint8_t* out32)
{
  uint8_t  block[k_sha256_block_bytes];
  uint32_t w[k_sha256_words];
  internal_sha256_build_block(in32, block);
  internal_sha256_schedule(block, w);
  internal_sha256_compress(w, out32);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Zero every slot in the key vault.
 *
 * @details
 * Walks the static slot array and clears each key byte so a freshly
 * booted vault contains no residual material. Called once during
 * secure-side bring-up before any ::ra8_key_vault_store request can
 * arrive from the importer.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init context.
 * @pre No NS-side challenge is in flight (single-thread guarantee).
 * @post Every slot's key bytes are zero.
 * @post No other state is modified.
 *
 * @note Not thread-safe; init/test path only.
 * @since 0.1.0
 */
ra8_err_t ra8_key_vault_init(void)
{
  for (uint16_t s = 0U; s < k_ra8_key_vault_slots; ++s) {
    for (uint16_t i = 0U; i < k_ra8_key_vault_key_bytes; ++i) {
      s_vault[s].key[i] = 0U;
    }
  }
  for (uint16_t i = 0U; i < k_ra8_key_vault_mac_key_bytes; ++i) {
    s_mac_key[i] = 0U;
  }
  s_mac_key_len = 0U;
  return k_ra8_ok;
}

/**
 * @brief Copy a 32-byte key into the requested vault slot.
 *
 * @details
 * Validates the slot index, then byte-copies the supplied key into
 * the secure-side slot array. No allocation, no freeing, no audit
 * log -- the importer holds those responsibilities.
 *
 * @param[in] slot Slot index (0..k_ra8_key_vault_slots-1).
 * @param[in] key  Source 32-byte key material.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Key stored.
 * @retval k_ra8_err_null_ptr       ``key`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``slot`` out of range.
 *
 * @pre ``key`` is non-NULL.
 * @pre Caller is in the secure-side dispatch path.
 * @post On success, ``s_vault[slot].key`` holds the new material.
 * @post On error, no slot bytes are mutated.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @see ra8_key_vault_sha256_xor_challenge
 * @since 0.1.0
 */
ra8_err_t ra8_key_vault_store(uint16_t slot, const uint8_t* key)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "store: key");
  if (slot >= k_ra8_key_vault_slots) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < k_ra8_key_vault_key_bytes; ++i) {
    s_vault[slot].key[i] = key[i];
  }
  return k_ra8_ok;
}

/**
 * @brief Reply to an NS challenge with SHA-256(key XOR challenge).
 *
 * @details
 * Forms the per-call scratch ``key XOR challenge`` on the secure
 * stack, hashes it via ::internal_sha256_32, then wipes the scratch
 * before returning so the XOR result does not linger in stack
 * memory. The NS world only ever sees the digest; the underlying
 * key never leaves the vault.
 *
 * @param[in]  slot      Slot index (0..k_ra8_key_vault_slots-1).
 * @param[in]  challenge 32-byte challenge from NS.
 * @param[out] out       32-byte SHA-256 response.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Response written.
 * @retval k_ra8_err_null_ptr       ``challenge`` or ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg    ``slot`` out of range.
 *
 * @pre ``challenge`` and ``out`` are non-NULL.
 * @pre Caller arrived through the NSC veneer with a resolved slot.
 * @post On success, ``out`` contains the digest.
 * @post Stack scratch ``key XOR challenge`` is wiped before return.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @see ra8_key_vault_store
 * @since 0.1.0
 */
ra8_err_t ra8_key_vault_sha256_xor_challenge(uint16_t slot, const uint8_t* challenge, uint8_t* out)
{
  RA8_CHECK_NULL_PTR(challenge, s_tag, "challenge: challenge");
  RA8_CHECK_NULL_PTR(out, s_tag, "challenge: out");
  if (slot >= k_ra8_key_vault_slots) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t scratch[k_ra8_key_vault_key_bytes] = {};
  for (uint16_t i = 0U; i < k_ra8_key_vault_key_bytes; ++i) {
    scratch[i] = (uint8_t)(s_vault[slot].key[i] ^ challenge[i]);
  }
  internal_sha256_32(scratch, out);
  /* Wipe the key-XOR-challenge result from the secure stack before the frame is
   * reused. ra8_secure_memzero writes through a volatile pointer, so -- unlike the
   * plain zero loop it replaces, which the optimiser was free to elide as a dead
   * store (the old code even suppressed cppcheck's unreadVariable) -- the erase
   * is an observable side effect that survives -O2 (T5-12). */
  ra8_secure_memzero(scratch, (size_t)k_ra8_key_vault_key_bytes);
  return k_ra8_ok;
}

ra8_err_t ra8_key_vault_set_mac_key(const uint8_t* key, uint16_t key_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "set_mac_key: key");
  if ((key_len != (uint16_t)k_kv_mac_key_128) && (key_len != (uint16_t)k_kv_mac_key_256)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < k_ra8_key_vault_mac_key_bytes; ++i) {
    s_mac_key[i] = (i < key_len) ? key[i] : 0U;
  }
  s_mac_key_len = key_len;
  return k_ra8_ok;
}

ra8_err_t ra8_key_vault_load_mac_key(uint8_t* out, uint16_t out_cap, uint16_t* out_len)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "load_mac_key: out");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "load_mac_key: out_len");
  if (s_mac_key_len == 0U) {
    return k_ra8_err_not_found;
  }
  if (out_cap < s_mac_key_len) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i < s_mac_key_len; ++i) {
    out[i] = s_mac_key[i];
  }
  *out_len = s_mac_key_len;
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_SIMULATOR_MODE */

/*
 * Fail-closed production variant. The software key store above is a placeholder
 * for a hardware-backed vault, so every entry point returns a hard error
 * (never k_ra8_ok). A production image that forgot to provide the real vault
 * therefore cannot store keys in unprotected SRAM or answer challenges from it.
 */

ra8_err_t ra8_key_vault_init(void)
{
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_key_vault_store(uint16_t slot, const uint8_t* key)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "store: key");
  (void)slot;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_key_vault_sha256_xor_challenge(uint16_t slot, const uint8_t* challenge, uint8_t* out)
{
  RA8_CHECK_NULL_PTR(challenge, s_tag, "challenge: challenge");
  RA8_CHECK_NULL_PTR(out, s_tag, "challenge: out");
  (void)slot;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_key_vault_set_mac_key(const uint8_t* key, uint16_t key_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "set_mac_key: key");
  (void)key_len;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_key_vault_load_mac_key(uint8_t* out, uint16_t out_cap, uint16_t* out_len)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "load_mac_key: out");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "load_mac_key: out_len");
  (void)out_cap;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_SIMULATOR_MODE */
