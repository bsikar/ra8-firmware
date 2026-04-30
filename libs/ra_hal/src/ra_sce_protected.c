/**
 * @file ra_sce_protected.c
 * @brief SCE protected-key HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API in
 * ``libs/ra_hal/inc/ra_sce_protected.h``. Each entry point validates
 * the wrapped-key blob via ``ra_sce_key_validate``, unwraps the
 * payload into a private scratch buffer, dispatches the underlying
 * unprotected ``ra_sce_*`` operation, and scrubs the scratch buffer.
 *
 * @warning Stub backend; NOT cryptographically secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sce_protected.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_sce.h"
#include "ra_sce_key_injection.h"

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)

/**
 * @var s_tag
 * @brief Logger tag for this TU.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "SCE_P";

/**
 * @enum ra_sce_p_layout_t
 * @brief Local copy of the wrapped-key blob layout.
 *
 * @details
 * Mirrors ``ra_sce_ki_layout_t`` from ``ra_sce_key_injection.c``;
 * duplicated here so this TU does not depend on private internals
 * of the injection driver.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_p_off_payload = 20U, /**< Payload offset inside a wrapped blob. */
} ra_sce_p_layout_t;

/**
 * @enum ra_sce_p_const_t
 * @brief Sizing constants used by the protected layer.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_p_aes_max_bytes = 32U, /**< AES-256 key length.           */
  k_ra_sce_p_rsa_pub_exp   = 4U,  /**< Public-exponent byte count.   */
} ra_sce_p_const_t;

/**
 * @brief Bytewise scrub of a buffer.
 *
 * @param[out] buf Buffer to zero.
 * @param[in]  n   Length of ``buf``.
 *
 * @pre ``buf`` non-NULL when ``n > 0``.
 * @post ``buf[0..n-1]`` is zero.
 *
 * @since 0.1.0
 */
static void p_scrub(uint8_t* buf, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    buf[i] = 0U;
  }
}

ra_err_t ra_sce_protected_aes_init(const uint8_t*        wrapped_key,
                                   ra_sce_aes_key_bits_t key_bits,
                                   ra_sce_aes_mode_t     mode,
                                   const uint8_t*        iv)
{
  RA_CHECK_NULL_PTR(wrapped_key, s_tag, "p_aes_init: wrapped_key");

  ra_err_t rc = ra_sce_key_validate(wrapped_key, k_ra_sce_wrapped_type_aes);
  if (rc != k_ra_ok) {
    return rc;
  }

  uint8_t        raw_key[k_ra_sce_p_aes_max_bytes] = {};
  const uint32_t n_bytes                           = (uint32_t)key_bits / 8U;
  if (n_bytes > (uint32_t)k_ra_sce_p_aes_max_bytes) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < n_bytes; ++i) {
    raw_key[i] = wrapped_key[(uint32_t)k_ra_sce_p_off_payload + i];
  }
  rc = ra_sce_aes_init(raw_key, key_bits, mode, iv);
  p_scrub(raw_key, n_bytes);
  return rc;
}

ra_err_t ra_sce_protected_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len)
{
  return ra_sce_aes_encrypt(plaintext, ciphertext, len);
}

ra_err_t ra_sce_protected_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len)
{
  return ra_sce_aes_decrypt(ciphertext, plaintext, len);
}

ra_err_t ra_sce_protected_aes_finish(uint8_t* tag_out)
{
  return ra_sce_aes_finish(tag_out);
}

ra_err_t ra_sce_protected_rsa_decrypt(const uint8_t*        wrapped_priv,
                                      ra_sce_rsa_key_bits_t key_bits,
                                      const uint8_t*        ciphertext,
                                      uint32_t              ciphertext_len,
                                      uint8_t*              plaintext_out,
                                      uint32_t*             plaintext_len_out)
{
  RA_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_rsa_decrypt: wrapped_priv");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "p_rsa_decrypt: ciphertext");
  RA_CHECK_NULL_PTR(plaintext_out, s_tag, "p_rsa_decrypt: plaintext_out");
  RA_CHECK_NULL_PTR(plaintext_len_out, s_tag, "p_rsa_decrypt: plaintext_len_out");

  /* The injection layer tags both private and public RSA blobs with
   * the public type tag (single-tag stub); accept either. */
  ra_err_t rc = ra_sce_key_validate(wrapped_priv, k_ra_sce_wrapped_type_rsa_pub);
  if (rc != k_ra_ok) {
    rc = ra_sce_key_validate(wrapped_priv, k_ra_sce_wrapped_type_rsa_priv);
    if (rc != k_ra_ok) {
      return rc;
    }
  }

  /* Unwrap modulus + exponent from the payload. */
  uint32_t mod_bytes = 0U;
  switch (key_bits) {
    case k_ra_sce_rsa_key_bits_2048:
      mod_bytes = 256U;
      break;
    case k_ra_sce_rsa_key_bits_3072:
      mod_bytes = 384U;
      break;
    case k_ra_sce_rsa_key_bits_4096:
      mod_bytes = 512U;
      break;
    default:
      return k_ra_err_invalid_arg;
  }

  ra_sce_rsa_priv_t priv = {.key_bits = key_bits,
                            .modulus  = wrapped_priv + (uint32_t)k_ra_sce_p_off_payload,
                            .private_exp =
                              wrapped_priv + (uint32_t)k_ra_sce_p_off_payload + mod_bytes};
  return ra_sce_rsa_private_decrypt(&priv,
                                    ciphertext,
                                    ciphertext_len,
                                    plaintext_out,
                                    plaintext_len_out);
}

ra_err_t ra_sce_protected_ecdsa_sign(const uint8_t*     wrapped_priv,
                                     ra_sce_ecc_curve_t curve,
                                     const uint8_t*     hash,
                                     uint32_t           hash_len,
                                     uint8_t*           sig_out)
{
  RA_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_ecdsa_sign: wrapped_priv");
  RA_CHECK_NULL_PTR(hash, s_tag, "p_ecdsa_sign: hash");
  RA_CHECK_NULL_PTR(sig_out, s_tag, "p_ecdsa_sign: sig_out");

  ra_err_t rc = ra_sce_key_validate(wrapped_priv, k_ra_sce_wrapped_type_ecc_priv);
  if (rc != k_ra_ok) {
    return rc;
  }

  const uint8_t* priv_payload = wrapped_priv + (uint32_t)k_ra_sce_p_off_payload;
  return ra_sce_ecc_ecdsa_sign(curve, priv_payload, hash, hash_len, sig_out);
}

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)
