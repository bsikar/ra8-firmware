/**
 * @file ra8_psa_crypto_internal.h
 * @brief Module-private definitions shared across the ``ra8_psa_crypto`` TUs.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The ``ra8_psa_crypto`` facade is split across two translation units:
 * ``ra8_psa_crypto.c`` (public API + key-handle pool) and
 * ``ra8_psa_crypto_fake.c`` (off-target SHA-256 / AEAD stand-ins). This
 * private header holds the definitions both TUs need:
 *
 * - The concrete ``struct ra8_psa_key_handle`` slot layout (the public
 *   header only forward-declares it).
 * - The internal typed-enum numeric constants (no magic numbers) used by
 *   the SHA-256 reference path, the AEAD fake, and the xorshift32
 *   RNG.
 * - ``extern`` declarations for the fake helpers that are defined in
 *   ``ra8_psa_crypto_fake.c`` but called from the public API in
 *   ``ra8_psa_crypto.c``.
 *
 * This header is internal to the library; consumers include the public
 * ``ra8_psa_crypto.h`` only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"

#ifndef RA8_OFF_TARGET
/* The real backend stores a PSA key handle in each slot; pull in the type the
 * struct below references. Skipped under RA8_OFF_TARGET, which keeps its own
 * software key material and never links tf-psa-crypto. */
#include "psa/crypto.h"
#endif

/* =============================================================================
 * Internal typed constants (no magic numbers)
 * =============================================================================
 */

/**
 * @enum ra8_psa_internal_const_t
 * @brief Internal numeric constants used by the crypto facade.
 *
 * @details
 * Centralizes all sizes, byte offsets, and bit counts referenced by the
 * SHA-256 reference implementation and AEAD fake paths so that no
 * magic numbers leak into the source.
 */
typedef enum : uint16_t {
  k_ra8_psa_sha256_block_bytes   = 64U,   /**< SHA-256 message block size.          */
  k_ra8_psa_sha256_state_words   = 8U,    /**< SHA-256 working state words.         */
  k_ra8_psa_sha256_schedule_len  = 64U,   /**< Length of message schedule W[].      */
  k_ra8_psa_sha256_init_words    = 16U,   /**< Initial copy from block to W[].      */
  k_ra8_psa_sha256_pad_buf_len   = 128U,  /**< Two-block padding scratch.           */
  k_ra8_psa_sha256_pad_threshold = 56U,   /**< If remaining < this, one tail block. */
  k_ra8_psa_fake_scratch_bytes   = 256U,  /**< AEAD fake scratch buffer size.       */
  k_ra8_psa_word_bits            = 32U,   /**< Word width in bits.                  */
  k_ra8_psa_byte_bits            = 8U,    /**< Bits per byte.                       */
  k_ra8_psa_bytes_per_word       = 4U,    /**< Bytes packed per 32-bit word.        */
  k_ra8_psa_pad_marker           = 0x80U, /**< SHA-256 padding sentinel byte.       */
  k_ra8_psa_length_field_bytes   = 8U,    /**< 64-bit big-endian length tail.       */
  k_ra8_psa_shift_b3             = 24U,   /**< Shift for big-endian byte 3.         */
  k_ra8_psa_shift_b2             = 16U,   /**< Shift for big-endian byte 2.         */
  k_ra8_psa_shift_b1             = 8U,    /**< Shift for big-endian byte 1.         */
  k_ra8_psa_rot_s0_a             = 7U,    /**< sigma0(W[i-15]) rot a.               */
  k_ra8_psa_rot_s0_b             = 18U,   /**< sigma0(W[i-15]) rot b.               */
  k_ra8_psa_shr_s0               = 3U,    /**< sigma0(W[i-15]) shr.                 */
  k_ra8_psa_rot_s1_a             = 17U,   /**< sigma1(W[i-2]) rot a.                */
  k_ra8_psa_rot_s1_b             = 19U,   /**< sigma1(W[i-2]) rot b.                */
  k_ra8_psa_shr_s1               = 10U,   /**< sigma1(W[i-2]) shr.                  */
  k_ra8_psa_rot_e_a              = 6U,    /**< Sigma1(e) rot a.                     */
  k_ra8_psa_rot_e_b              = 11U,   /**< Sigma1(e) rot b.                     */
  k_ra8_psa_rot_e_c              = 25U,   /**< Sigma1(e) rot c.                     */
  k_ra8_psa_rot_a_a              = 2U,    /**< Sigma0(a) rot a.                     */
  k_ra8_psa_rot_a_b              = 13U,   /**< Sigma0(a) rot b.                     */
  k_ra8_psa_rot_a_c              = 22U,   /**< Sigma0(a) rot c.                     */
  k_ra8_psa_w_back_15            = 15U,   /**< Schedule offset W[i-15].             */
  k_ra8_psa_w_back_2             = 2U,    /**< Schedule offset W[i-2].              */
  k_ra8_psa_w_back_16            = 16U,   /**< Schedule offset W[i-16].             */
  k_ra8_psa_w_back_7             = 7U,    /**< Schedule offset W[i-7].              */
  k_ra8_psa_state_idx_a          = 0U,    /**< SHA-256 working register a slot.     */
  k_ra8_psa_state_idx_b          = 1U,    /**< SHA-256 working register b slot.     */
  k_ra8_psa_state_idx_c          = 2U,    /**< SHA-256 working register c slot.     */
  k_ra8_psa_state_idx_d          = 3U,    /**< SHA-256 working register d slot.     */
  k_ra8_psa_state_idx_e          = 4U,    /**< SHA-256 working register e slot.     */
  k_ra8_psa_state_idx_f          = 5U,    /**< SHA-256 working register f slot.     */
  k_ra8_psa_state_idx_g          = 6U,    /**< SHA-256 working register g slot.     */
  k_ra8_psa_state_idx_h          = 7U,    /**< SHA-256 working register h slot.     */
} ra8_psa_internal_const_t;

/**
 * @enum ra8_psa_xs32_const_t
 * @brief xorshift32 off-target RNG constants (Marsaglia 2003).
 */
typedef enum : uint32_t {
  k_xs32_seed   = 0xACE1ACE1U, /**< Default seed.                         */
  k_xs32_byte_m = 0xFFU,       /**< Mask isolating the low byte of state. */
} ra8_psa_xs32_const_t;

/**
 * @enum ra8_psa_xs32_shift_t
 * @brief xorshift32 shift amounts (Marsaglia 2003 Table 1 row "y[13,17,5]").
 */
typedef enum : uint8_t {
  k_xs32_shl_a = 13U, /**< First left shift.  */
  k_xs32_shr_b = 17U, /**< Right shift.       */
  k_xs32_shl_c = 5U,  /**< Second left shift. */
} ra8_psa_xs32_shift_t;

/**
 * @enum ra8_psa_sha256_iv_t
 * @brief SHA-256 initial hash values H0..H7 (FIPS 180-4 Sec 5.3.3).
 */
typedef enum : uint32_t {
  k_ra8_psa_sha256_h0 = 0x6a09e667U, /**< RA8 PSA sha256 h0. */
  k_ra8_psa_sha256_h1 = 0xbb67ae85U, /**< RA8 PSA sha256 h1. */
  k_ra8_psa_sha256_h2 = 0x3c6ef372U, /**< RA8 PSA sha256 h2. */
  k_ra8_psa_sha256_h3 = 0xa54ff53aU, /**< RA8 PSA sha256 h3. */
  k_ra8_psa_sha256_h4 = 0x510e527fU, /**< RA8 PSA sha256 h4. */
  k_ra8_psa_sha256_h5 = 0x9b05688cU, /**< RA8 PSA sha256 h5. */
  k_ra8_psa_sha256_h6 = 0x1f83d9abU, /**< RA8 PSA sha256 h6. */
  k_ra8_psa_sha256_h7 = 0x5be0cd19U, /**< RA8 PSA sha256 h7. */
} ra8_psa_sha256_iv_t;

/* =============================================================================
 * Pool slot definition
 * =============================================================================
 */

/**
 * @struct ra8_psa_key_handle
 * @brief Concrete pool slot backing one ``ra8_psa_key_t``.
 *
 * @details
 * One slot per imported key. ``in_use`` doubles as the bitmap bit; the
 * array index is the slot index ``[0, k_ra8_psa_max_keys)``.
 *
 * @invariant ``in_use`` is true if and only if ``key_len > 0``.
 */
struct ra8_psa_key_handle {
  bool               in_use;                       /**< Slot allocated.           */
  ra8_psa_key_attr_t attr;                         /**< Cached caller attributes. */
  size_t             key_len;                      /**< Bytes valid in ``key``.   */
  uint8_t            key[k_ra8_psa_max_key_bytes]; /**< Raw key material (fake).  */
#ifndef RA8_OFF_TARGET
  psa_key_id_t psa_id; /**< Underlying PSA key identifier. */
#endif                 /**< Endif.                         */
};

#ifdef RA8_OFF_TARGET

/* =============================================================================
 * Off-target crypto helpers (defined in ra8_psa_crypto_fake.c)
 * =============================================================================
 */

/**
 * @brief One-shot SHA-256 over ``(in, in_len)`` -> 32-byte ``out``.
 *
 * @details
 * FIPS 180-4 reference implementation used by the host unit tests. Never
 * runs on the target; the firmware build delegates to TF-PSA-Crypto.
 *
 * @param[in]  in     Input buffer (may be NULL only when ``in_len`` is 0).
 * @param[in]  in_len Number of input bytes.
 * @param[out] out    Destination digest, ``k_ra8_psa_sha256_len`` bytes.
 *
 * @pre ``out`` is non-NULL and at least ``k_ra8_psa_sha256_len`` bytes.
 * @pre ``in`` is non-NULL whenever ``in_len`` is non-zero.
 * @post ``out`` holds the SHA-256 digest of the input.
 * @post Input buffers are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_psa_fake_sha256_oneshot(const uint8_t* in,
                                 size_t         in_len,
                                 uint8_t        out[k_ra8_psa_sha256_len]);

/**
 * @brief Fake AEAD encrypt body shared by ``ra8_psa_aead_encrypt``.
 *
 * @details
 * Encrypts ``plain`` into ``out`` with the fake XOR keystream and
 * appends a 16-byte SHA-256 tag at ``out[plain_len..plain_len+16)``.
 *
 * @param[in]  slot      Validated key slot.
 * @param[in]  nonce     AEAD nonce.
 * @param[in]  nonce_len Nonce length in bytes.
 * @param[in]  aad       Additional authenticated data (may be NULL if empty).
 * @param[in]  aad_len   AAD length in bytes.
 * @param[in]  plain     Plaintext (may be NULL if empty).
 * @param[in]  plain_len Plaintext length in bytes.
 * @param[out] out       Ciphertext + tag destination.
 * @param[out] out_len   Bytes written to ``out``.
 *
 * @return Result code or value; see implementation.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_invalid_size Plaintext exceeds the scratch budget.
 *
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_psa_fake_aead_encrypt(const struct ra8_psa_key_handle* slot,
                                    const uint8_t*                   nonce,
                                    size_t                           nonce_len,
                                    const uint8_t*                   aad,
                                    size_t                           aad_len,
                                    const uint8_t*                   plain,
                                    size_t                           plain_len,
                                    uint8_t*                         out,
                                    size_t*                          out_len);

/**
 * @brief Fake AEAD decrypt body shared by ``ra8_psa_aead_decrypt``.
 *
 * @details
 * Verifies the 16-byte tag at the tail of ``cipher`` and, on success,
 * writes ``plain_len`` bytes of plaintext into ``out``.
 *
 * @param[in]  slot      Validated key slot.
 * @param[in]  nonce     AEAD nonce.
 * @param[in]  nonce_len Nonce length in bytes.
 * @param[in]  aad       Additional authenticated data (may be NULL if empty).
 * @param[in]  aad_len   AAD length in bytes.
 * @param[in]  cipher    Ciphertext followed by the 16-byte tag.
 * @param[in]  plain_len Plaintext length in bytes (cipher minus tag).
 * @param[out] out       Plaintext destination (may be NULL if empty).
 * @param[out] out_len   Bytes written to ``out``.
 *
 * @return Result code or value; see implementation.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_crc_mismatch Tag verification failed.
 * @retval k_ra8_err_invalid_size Plaintext exceeds the scratch budget.
 *
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t ra8_psa_fake_aead_decrypt(const struct ra8_psa_key_handle* slot,
                                    const uint8_t*                   nonce,
                                    size_t                           nonce_len,
                                    const uint8_t*                   aad,
                                    size_t                           aad_len,
                                    const uint8_t*                   cipher,
                                    size_t                           plain_len,
                                    uint8_t*                         out,
                                    size_t*                          out_len);

#endif /* RA8_OFF_TARGET */
