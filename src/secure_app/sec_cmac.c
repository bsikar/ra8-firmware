/**
 * @file sec_cmac.c
 * @brief Secure-side AES-CMAC seam implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Two interchangeable backends behind the ``ra8_sec_cmac_*`` API declared in
 * ``sec_cmac_internal.h``:
 *
 * - ``RA8_KEY_IMPORT_PSA_CMAC`` defined -- delegate to the vendored,
 *   silicon-proven TF-PSA-Crypto: ``psa_mac_compute`` for generation and
 *   ``psa_mac_verify`` for the verdict, both with ``PSA_ALG_CMAC`` over a
 *   transiently imported ``PSA_KEY_TYPE_AES`` key. This is the production /
 *   HIL path; the app that links TF-PSA-Crypto sets the flag.
 * - otherwise (the default: host unit tests and every app that does not link
 *   TF-PSA-Crypto) -- a self-contained AES-128 / AES-256 + CMAC reference
 *   (FIPS 197 + NIST SP 800-38B). It is exercised by the host suite against
 *   the published SP 800-38B known-answer vectors, so it is real, not a
 *   forgeable placeholder.
 *
 * Both backends compute the identical standard tag, so the seam is Liskov-
 * substitutable and the wrapped-key blob format is backend-independent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_secure.h"
#include "sec_cmac_internal.h"

/** @brief Logging / error tag prefix for this module. */
static const char* s_tag = "SECMAC";

/**
 * @brief Shared precondition check for both CMAC entry points.
 *
 * @details
 * Validates the key pointer, the key length (AES-128 or AES-256 only), the
 * message pointer/length pairing, and the static message-size cap that keeps
 * every downstream loop provably bounded (NASA Power of 10 Rule 2). Keeping
 * the checks in one helper means the compute and verify paths cannot drift.
 *
 * @param[in] key     Symmetric CMAC key.
 * @param[in] key_len Key length in bytes.
 * @param[in] msg     Message pointer (NULL only when ``msg_len==0``).
 * @param[in] msg_len Message length in bytes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Arguments are well-formed.
 * @retval k_ra8_err_null_ptr     ``key`` NULL, or ``msg`` NULL with a
 *                                non-zero ``msg_len``.
 * @retval k_ra8_err_invalid_arg  ``key_len`` was neither 16 nor 32.
 * @retval k_ra8_err_invalid_size ``msg_len`` exceeded the static cap.
 *
 * @pre ``key`` handling is delegated to ``RA8_CHECK_NULL_PTR``.
 * @pre ``key_len`` is compared against the two accepted lengths.
 * @post No state is mutated.
 * @post Return value depends only on the parameters.
 *
 * @note Pure validation helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cmac_check_args(const uint8_t* key, uint16_t key_len, const uint8_t* msg, uint32_t msg_len)
{
  RA8_CHECK_NULL_PTR(key, s_tag, "cmac: key");
  if ((key_len != (uint16_t)k_ra8_sec_cmac_key_128) &&
      (key_len != (uint16_t)k_ra8_sec_cmac_key_256)) {
    return k_ra8_err_invalid_arg;
  }
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (msg_len > (uint32_t)k_ra8_sec_cmac_max_msg_bytes) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

#ifdef RA8_KEY_IMPORT_PSA_CMAC

/* =============================================================================
 * PSA backend -- production / HIL image (TF-PSA-Crypto)
 * =============================================================================
 */

#include "psa/crypto.h"

/**
 * @brief Import ``key`` as a transient AES-CMAC PSA key.
 *
 * @details
 * Wraps ``psa_import_key`` with the ``PSA_ALG_CMAC`` policy and both
 * sign/verify message-usage flags so the same slot can generate and check a
 * tag. The caller destroys the key on every return path.
 *
 * @param[in]  key     Raw AES key bytes.
 * @param[in]  key_len Key length (16 or 32).
 * @param[out] out_id  Receives the PSA key id on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Key imported.
 * @retval k_ra8_err_hw_error ``psa_import_key`` rejected the material.
 *
 * @pre ``key`` and ``out_id`` are non-NULL (guaranteed by the caller).
 * @pre ``psa_crypto_init`` has run in the image.
 * @post On ``k_ra8_ok``, ``*out_id`` names a live transient key.
 * @post On error, no PSA key is left allocated.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_psa_import_cmac_key(const uint8_t* key, uint16_t key_len, psa_key_id_t* out_id)
{
  psa_key_attributes_t attr = psa_key_attributes_init();
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
  psa_key_id_t       kid = 0;
  const psa_status_t st  = psa_import_key(&attr, key, (size_t)key_len, &kid);
  psa_reset_key_attributes(&attr);
  if (st != PSA_SUCCESS) {
    return k_ra8_err_hw_error;
  }
  *out_id = kid;
  return k_ra8_ok;
}

ra8_err_t ra8_sec_cmac_compute(const uint8_t* key,
                               uint16_t       key_len,
                               const uint8_t* msg,
                               uint32_t       msg_len,
                               uint8_t*       out_mac)
{
  RA8_CHECK_NULL_PTR(out_mac, s_tag, "compute: out_mac");
  const ra8_err_t ck = internal_cmac_check_args(key, key_len, msg, msg_len);
  if (ck != k_ra8_ok) {
    return ck;
  }
  psa_key_id_t    kid  = 0;
  const ra8_err_t ierr = internal_psa_import_cmac_key(key, key_len, &kid);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  size_t             produced = 0U;
  const psa_status_t st       = psa_mac_compute(kid,
                                                PSA_ALG_CMAC,
                                                msg,
                                                (size_t)msg_len,
                                                out_mac,
                                                (size_t)k_ra8_sec_cmac_tag_bytes,
                                                &produced);
  (void)psa_destroy_key(kid);
  if ((st != PSA_SUCCESS) || (produced != (size_t)k_ra8_sec_cmac_tag_bytes)) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sec_cmac_verify(const uint8_t* key,
                              uint16_t       key_len,
                              const uint8_t* msg,
                              uint32_t       msg_len,
                              const uint8_t* mac,
                              uint16_t       mac_len)
{
  RA8_CHECK_NULL_PTR(mac, s_tag, "verify: mac");
  const ra8_err_t ck = internal_cmac_check_args(key, key_len, msg, msg_len);
  if (ck != k_ra8_ok) {
    return ck;
  }
  psa_key_id_t    kid  = 0;
  const ra8_err_t ierr = internal_psa_import_cmac_key(key, key_len, &kid);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  /* psa_mac_verify performs the length check and a constant-time compare
   * internally, returning PSA_ERROR_INVALID_SIGNATURE on any mismatch. */
  const psa_status_t st =
    psa_mac_verify(kid, PSA_ALG_CMAC, msg, (size_t)msg_len, mac, (size_t)mac_len);
  (void)psa_destroy_key(kid);
  if (st == PSA_SUCCESS) {
    return k_ra8_ok;
  }
  if (st == PSA_ERROR_INVALID_SIGNATURE) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_err_hw_error;
}

#else /* !RA8_KEY_IMPORT_PSA_CMAC -- in-tree AES-CMAC reference (host + portable) */

/* =============================================================================
 * Reference backend -- self-contained AES-128/256 + CMAC (FIPS 197 / SP 800-38B)
 * =============================================================================
 *
 * The AES round transform and CMAC subkey/GF(2^128) doubling below are direct
 * transcriptions of FIPS 197 and NIST SP 800-38B. The S-box and the field
 * constants are standard values, not tunable parameters; the loops all have
 * compile-time-fixed bounds. The host suite pins the output to the published
 * SP 800-38B AES-128 and AES-256 known-answer vectors.
 */

/** @brief AES dimensions in bytes (FIPS 197). */
typedef enum : uint16_t {
  k_aes_block_bytes  = 16U,  /**< AES block / CMAC tag size.               */
  k_aes_word_bytes   = 4U,   /**< Bytes per key-schedule word.             */
  k_aes_state_cols   = 4U,   /**< Columns in the AES state.                */
  k_aes_max_rk_bytes = 240U, /**< Round-key bytes for AES-256 (16*(14+1)). */
} aes_dim_t;

/** @brief AES round counts and small byte offsets (FIPS 197). */
typedef enum : uint8_t {
  k_aes_nr_128    = 10U, /**< AES-128 rounds.                     */
  k_aes_nr_256    = 14U, /**< AES-256 rounds.                     */
  k_aes_ext_sub_i = 4U,  /**< AES-256 extra SubWord at i%Nk==4.   */
  k_aes_col_mask  = 3U,  /**< Column index wrap mask (mod 4).     */
  k_aes_msb_shift = 7U,  /**< Byte MSB shift.                     */
  k_aes_last_byte = 15U, /**< Index of the final byte in a block. */
  k_aes_rcon_seed = 1U,  /**< Rcon seed value (round 1).          */
} aes_off_t;

/** @brief GF field constants used by AES / CMAC. */
typedef enum : uint8_t {
  k_aes_field_poly  = 0x1BU, /**< GF(2^8) reduction poly (FIPS 197 4.2).     */
  k_cmac_rb         = 0x87U, /**< GF(2^128) constant (SP 800-38B 5.3).       */
  k_cmac_pad_marker = 0x80U, /**< CMAC final-block pad bit (SP 800-38B 5.5). */
} aes_gf_t;

/** @brief AES S-box (FIPS 197 Fig. 7). Standard substitution table. */
static const uint8_t s_aes_sbox[256] = {
  0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
  0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
  0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
  0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
  0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
  0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
  0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
  0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
  0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
  0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
  0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
  0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
  0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
  0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
  0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
  0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/**
 * @brief Multiply a byte by x in GF(2^8) with the AES reduction poly.
 *
 * @details FIPS 197 Sec 4.2: left-shift, conditionally XOR 0x1B on overflow.
 *
 * @param[in] value Source byte.
 * @return ``value`` doubled in the AES field.
 * @retval "value<<1"        When the high bit was clear.
 * @retval "(value<<1)^0x1B" When the high bit was set.
 *
 * @pre ``value`` is any byte.
 * @pre Caller treats this as a pure expression.
 * @post No state is mutated.
 * @post Return depends only on ``value``.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_aes_xtime(uint8_t value)
{
  const uint8_t hi = (uint8_t)(value >> k_aes_msb_shift);
  return (uint8_t)(((uint8_t)(value << 1U)) ^ (uint8_t)(hi * (uint8_t)k_aes_field_poly));
}

/**
 * @brief Expand an AES key into the full round-key schedule.
 *
 * @details FIPS 197 Sec 5.2 key expansion, parameterised by ``nk`` words
 * (4 or 8) and ``nr`` rounds (10 or 14).
 *
 * @param[in]  key Raw key bytes (``nk*4`` long).
 * @param[in]  nk  Key words (4 for AES-128, 8 for AES-256).
 * @param[in]  nr  Round count (10 or 14).
 * @param[out] rk  Round-key buffer (``16*(nr+1)`` bytes).
 *
 * @pre ``key`` and ``rk`` are non-NULL.
 * @pre ``nk`` is 4 or 8 and ``nr`` matches.
 * @post ``rk`` holds the expanded schedule.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_aes_key_expand(const uint8_t* key, uint8_t nk, uint8_t nr, uint8_t* rk)
{
  const uint16_t total_words = (uint16_t)((uint16_t)k_aes_state_cols * (uint16_t)(nr + 1U));
  for (uint16_t i = 0U; i < ((uint16_t)nk * (uint16_t)k_aes_word_bytes); ++i) {
    rk[i] = key[i];
  }
  uint8_t rcon = (uint8_t)k_aes_rcon_seed;
  for (uint16_t i = nk; i < total_words; ++i) {
    uint8_t t[k_aes_word_bytes];
    for (uint8_t j = 0U; j < (uint8_t)k_aes_word_bytes; ++j) {
      t[j] = rk[((uint16_t)(i - 1U) * (uint16_t)k_aes_word_bytes) + j];
    }
    if ((i % nk) == 0U) {
      const uint8_t tmp = t[0];
      t[0]              = (uint8_t)(s_aes_sbox[t[1]] ^ rcon);
      t[1]              = s_aes_sbox[t[2]];
      t[2]              = s_aes_sbox[t[3]];
      t[3]              = s_aes_sbox[tmp];
      rcon              = internal_aes_xtime(rcon);
    } else if ((nk > 6U) && ((i % nk) == (uint8_t)k_aes_ext_sub_i)) {
      for (uint8_t j = 0U; j < (uint8_t)k_aes_word_bytes; ++j) {
        t[j] = s_aes_sbox[t[j]];
      }
    } else {
      /* No transform: straight copy from t[] below. */
    }
    for (uint8_t j = 0U; j < (uint8_t)k_aes_word_bytes; ++j) {
      rk[((uint16_t)i * (uint16_t)k_aes_word_bytes) + j] =
        (uint8_t)(rk[((uint16_t)(i - nk) * (uint16_t)k_aes_word_bytes) + j] ^ t[j]);
    }
  }
}

/**
 * @brief Apply SubBytes + ShiftRows to the AES state in place.
 *
 * @details FIPS 197 Sec 5.1.1/5.1.2: substitute every state byte
 * through the S-box, then rotate row ``r`` left by ``r`` columns. The
 * state is column-major (byte ``4*c+r`` is row ``r`` of column ``c``).
 *
 * @param[in,out] s 16-byte AES state.
 *
 * @pre ``s`` is a non-NULL 16-byte state block.
 * @pre The S-box table ``s_aes_sbox`` is the FIPS 197 constant.
 * @post ``s`` holds ShiftRows(SubBytes(s)).
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_aes_sub_shift(uint8_t* s)
{
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    s[i] = s_aes_sbox[s[i]];
  }
  uint8_t shifted[k_aes_block_bytes];
  for (uint8_t r = 0U; r < (uint8_t)k_aes_state_cols; ++r) {
    for (uint8_t c = 0U; c < (uint8_t)k_aes_state_cols; ++c) {
      shifted[((uint8_t)(c * (uint8_t)k_aes_state_cols)) + r] =
        s[((uint8_t)(((c + r) & (uint8_t)k_aes_col_mask) * (uint8_t)k_aes_state_cols)) + r];
    }
  }
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    s[i] = shifted[i];
  }
}

/**
 * @brief Apply MixColumns to the AES state in place.
 *
 * @details FIPS 197 Sec 5.1.3: multiply every state column by the
 * fixed polynomial ``{03}x^3 + {01}x^2 + {01}x + {02}`` in GF(2^8),
 * expressed through ::internal_aes_xtime doublings.
 *
 * @param[in,out] s 16-byte AES state.
 *
 * @pre ``s`` is a non-NULL 16-byte state block.
 * @pre Caller skips this step on the final round per FIPS 197.
 * @post Every column of ``s`` has been mixed.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_aes_mix_columns(uint8_t* s)
{
  for (uint8_t c = 0U; c < (uint8_t)k_aes_state_cols; ++c) {
    uint8_t*      col = &s[(uint8_t)(c * (uint8_t)k_aes_state_cols)];
    const uint8_t a0  = col[0];
    const uint8_t a1  = col[1];
    const uint8_t a2  = col[2];
    const uint8_t a3  = col[3];
    col[0]            = (uint8_t)(internal_aes_xtime(a0) ^ internal_aes_xtime(a1) ^ a1 ^ a2 ^ a3);
    col[1]            = (uint8_t)(a0 ^ internal_aes_xtime(a1) ^ internal_aes_xtime(a2) ^ a2 ^ a3);
    col[2]            = (uint8_t)(a0 ^ a1 ^ internal_aes_xtime(a2) ^ internal_aes_xtime(a3) ^ a3);
    col[3]            = (uint8_t)(internal_aes_xtime(a0) ^ a0 ^ a1 ^ a2 ^ internal_aes_xtime(a3));
  }
}

/**
 * @brief Encrypt one 16-byte block with AES-``nr``.
 *
 * @details FIPS 197 Sec 5.1 cipher: initial AddRoundKey, ``nr-1`` full
 * rounds (SubBytes/ShiftRows/MixColumns/AddRoundKey), final partial
 * round -- the per-step transforms live in ::internal_aes_sub_shift
 * and ::internal_aes_mix_columns. The state is column-major (byte
 * ``4*c+r`` is row ``r`` of column ``c``).
 *
 * @param[in]  rk  Round-key schedule.
 * @param[in]  nr  Round count (10 or 14).
 * @param[in]  in  16-byte plaintext block.
 * @param[out] out 16-byte ciphertext block.
 *
 * @pre ``rk``, ``in`` and ``out`` are non-NULL.
 * @pre ``rk`` was produced by ::internal_aes_key_expand for ``nr``.
 * @post ``out`` holds the AES ciphertext of ``in``.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_aes_encrypt(const uint8_t* rk, uint8_t nr, const uint8_t* in, uint8_t* out)
{
  uint8_t s[k_aes_block_bytes];
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    s[i] = (uint8_t)(in[i] ^ rk[i]);
  }
  for (uint8_t round = 1U; round <= nr; ++round) {
    internal_aes_sub_shift(s);
    if (round != nr) {
      internal_aes_mix_columns(s);
    }
    const uint8_t* round_key = &rk[(size_t)round * (size_t)k_aes_block_bytes];
    for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
      s[i] = (uint8_t)(s[i] ^ round_key[i]);
    }
  }
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    out[i] = s[i];
  }
}

/**
 * @brief Double a 128-bit value in GF(2^128) (CMAC subkey step).
 *
 * @details NIST SP 800-38B Sec 6.1: left-shift the 16-byte value by one bit,
 * XOR-ing the constant ``Rb`` (0x87) into the last byte when the top bit was
 * set.
 *
 * @param[in]  in  Source 16-byte value.
 * @param[out] out Doubled 16-byte value.
 *
 * @pre ``in`` and ``out`` are non-NULL 16-byte buffers.
 * @pre Caller uses the result as a CMAC subkey.
 * @post ``out`` holds ``in`` doubled in the field.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cmac_double(const uint8_t* in, uint8_t* out)
{
  const uint8_t msb = (uint8_t)(in[0] >> k_aes_msb_shift);
  for (uint8_t i = 0U; i < (uint8_t)k_aes_last_byte; ++i) {
    out[i] = (uint8_t)((uint8_t)(in[i] << 1U) | (uint8_t)(in[i + 1U] >> k_aes_msb_shift));
  }
  out[k_aes_last_byte] = (uint8_t)(in[k_aes_last_byte] << 1U);
  if (msb != 0U) {
    out[k_aes_last_byte] = (uint8_t)(out[k_aes_last_byte] ^ (uint8_t)k_cmac_rb);
  }
}

/**
 * @brief Derive the CMAC subkeys K1 / K2 from the expanded key.
 *
 * @details SP 800-38B Sec 6.1: ``L = AES(key, 0^128)``, ``K1 = 2L``
 * and ``K2 = 2 * K1`` in GF(2^128). The intermediate ``L`` value is
 * wiped before returning so only the subkeys leave this frame.
 *
 * @param[in]  rk Round-key schedule from ::internal_aes_key_expand.
 * @param[in]  nr Round count (10 or 14).
 * @param[out] k1 Receives the full-block subkey K1 (16 bytes).
 * @param[out] k2 Receives the padded-block subkey K2 (16 bytes).
 *
 * @pre ``rk`` was expanded for ``nr`` rounds.
 * @pre ``k1`` and ``k2`` are non-NULL 16-byte buffers.
 * @post ``k1`` / ``k2`` hold the SP 800-38B subkeys.
 * @post The intermediate ``L`` stack value has been wiped.
 *
 * @note Pure compute helper; uses only stack.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_cmac_subkeys(const uint8_t* rk, uint8_t nr, uint8_t* k1, uint8_t* k2)
{
  const uint8_t zero[k_aes_block_bytes] = {};
  uint8_t       l_val[k_aes_block_bytes];
  internal_aes_encrypt(rk, nr, zero, l_val);
  internal_cmac_double(l_val, k1);
  internal_cmac_double(k1, k2);
  ra8_secure_memzero(l_val, sizeof(l_val));
}

/**
 * @brief Fold the (possibly padded) final CMAC block with its subkey.
 *
 * @details SP 800-38B Sec 6.2 step 4: a complete final block is
 * XOR-ed with K1; an incomplete (or empty-message) block is padded
 * with ``0x80 0x00...`` and XOR-ed with K2.
 *
 * @param[in]  msg      Message bytes (NULL only when ``msg_len==0``).
 * @param[in]  msg_len  Message length in bytes.
 * @param[in]  last_off Byte offset of the final block within ``msg``.
 * @param[in]  complete True when the final block is a whole 16 bytes.
 * @param[in]  k1       Subkey for complete final blocks.
 * @param[in]  k2       Subkey for padded final blocks.
 * @param[out] last     Receives the folded 16-byte final block.
 *
 * @pre ``last_off`` and ``complete`` describe ``msg`` per the caller.
 * @pre ``k1`` / ``k2`` came from ::internal_cmac_subkeys.
 * @post ``last`` holds the subkey-folded final block.
 * @post No global state is mutated.
 *
 * @note Pure compute helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cmac_build_last(const uint8_t* msg,
                                                  uint32_t       msg_len,
                                                  uint32_t       last_off,
                                                  bool           complete,
                                                  const uint8_t* k1,
                                                  const uint8_t* k2,
                                                  uint8_t*       last)
{
  if (complete) {
    for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
      last[i] = (uint8_t)(msg[last_off + i] ^ k1[i]);
    }
    return;
  }
  const uint32_t rem = msg_len - last_off;
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    uint8_t byte = 0U;
    if ((uint32_t)i < rem) {
      byte = msg[last_off + (uint32_t)i];
    } else if ((uint32_t)i == rem) {
      byte = (uint8_t)k_cmac_pad_marker;
    } else {
      byte = 0U;
    }
    last[i] = (uint8_t)(byte ^ k2[i]);
  }
}

/**
 * @brief Compute the AES-CMAC tag of a message (SP 800-38B).
 *
 * @details
 * Derives subkeys K1/K2 via ::internal_cmac_subkeys, chains the full
 * blocks, and folds the (possibly padded) final block built by
 * ::internal_cmac_build_last before the last AES call. Handles the
 * empty message as a single padded block per the standard.
 *
 * @param[in]  key     AES key bytes.
 * @param[in]  key_len Key length (16 or 32).
 * @param[in]  msg     Message (NULL only when ``msg_len==0``).
 * @param[in]  msg_len Message length (``<= k_ra8_sec_cmac_max_msg_bytes``).
 * @param[out] out_mac 16-byte tag output.
 *
 * @pre All pointers valid per the caller's validation.
 * @pre ``msg_len`` is within the static cap.
 * @post ``out_mac`` holds the CMAC tag.
 * @post Local subkey material is wiped before return.
 *
 * @note Pure compute helper; not thread-safe (uses only stack).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cmac_tag(const uint8_t* key,
                                           uint16_t       key_len,
                                           const uint8_t* msg,
                                           uint32_t       msg_len,
                                           uint8_t*       out_mac)
{
  const uint8_t nk = (uint8_t)(key_len / (uint16_t)k_aes_word_bytes);
  const uint8_t nr =
    (key_len == (uint16_t)k_ra8_sec_cmac_key_128) ? (uint8_t)k_aes_nr_128 : (uint8_t)k_aes_nr_256;
  uint8_t rk[k_aes_max_rk_bytes];
  internal_aes_key_expand(key, nk, nr, rk);

  uint8_t k1[k_aes_block_bytes];
  uint8_t k2[k_aes_block_bytes];
  internal_cmac_subkeys(rk, nr, k1, k2);

  /* Block count and whether the last block is a whole 16 bytes. */
  const uint32_t n_blocks =
    (msg_len == 0U) ? 1U : ((msg_len + (uint32_t)k_aes_last_byte) / (uint32_t)k_aes_block_bytes);
  const bool     complete = (msg_len != 0U) && ((msg_len % (uint32_t)k_aes_block_bytes) == 0U);
  const uint32_t last_off = (n_blocks - 1U) * (uint32_t)k_aes_block_bytes;

  uint8_t last[k_aes_block_bytes];
  internal_cmac_build_last(msg, msg_len, last_off, complete, k1, k2, last);

  uint8_t x[k_aes_block_bytes] = {};
  uint8_t y[k_aes_block_bytes];
  for (uint32_t b = 0U; (b + 1U) < n_blocks; ++b) {
    for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
      y[i] = (uint8_t)(x[i] ^ msg[((uint32_t)b * (uint32_t)k_aes_block_bytes) + (uint32_t)i]);
    }
    internal_aes_encrypt(rk, nr, y, x);
  }
  for (uint8_t i = 0U; i < (uint8_t)k_aes_block_bytes; ++i) {
    y[i] = (uint8_t)(x[i] ^ last[i]);
  }
  internal_aes_encrypt(rk, nr, y, out_mac);

  ra8_secure_memzero(rk, sizeof(rk));
  ra8_secure_memzero(k1, sizeof(k1));
  ra8_secure_memzero(k2, sizeof(k2));
}

ra8_err_t ra8_sec_cmac_compute(const uint8_t* key,
                               uint16_t       key_len,
                               const uint8_t* msg,
                               uint32_t       msg_len,
                               uint8_t*       out_mac)
{
  RA8_CHECK_NULL_PTR(out_mac, s_tag, "compute: out_mac");
  const ra8_err_t ck = internal_cmac_check_args(key, key_len, msg, msg_len);
  if (ck != k_ra8_ok) {
    return ck;
  }
  internal_cmac_tag(key, key_len, msg, msg_len, out_mac);
  return k_ra8_ok;
}

ra8_err_t ra8_sec_cmac_verify(const uint8_t* key,
                              uint16_t       key_len,
                              const uint8_t* msg,
                              uint32_t       msg_len,
                              const uint8_t* mac,
                              uint16_t       mac_len)
{
  RA8_CHECK_NULL_PTR(mac, s_tag, "verify: mac");
  const ra8_err_t ck = internal_cmac_check_args(key, key_len, msg, msg_len);
  if (ck != k_ra8_ok) {
    return ck;
  }
  uint8_t computed[k_ra8_sec_cmac_tag_bytes];
  internal_cmac_tag(key, key_len, msg, msg_len, computed);
  /* Security verdict: reject a wrong-length tag OR a tag that does not match
   * in constant time. ra8_ct_equal has no data-dependent early-out, so the
   * timing does not leak how many leading bytes matched (ra8_secure.h). */
  const bool len_bad = (mac_len != (uint16_t)k_ra8_sec_cmac_tag_bytes);
  const bool tag_bad = !ra8_ct_equal(computed, mac, (size_t)k_ra8_sec_cmac_tag_bytes);
  ra8_secure_memzero(computed, sizeof(computed));
  if (len_bad || tag_bad) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

#endif /* RA8_KEY_IMPORT_PSA_CMAC */
