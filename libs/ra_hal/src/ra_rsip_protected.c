/**
 * @file ra_rsip_protected.c
 * @brief RSIP protected-key HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API in
 * ``libs/ra_hal/inc/ra_rsip_protected.h``. Each entry point validates
 * the wrapped-key blob via ``ra_rsip_key_validate``, unwraps the
 * payload into a private scratch buffer, dispatches the underlying
 * unprotected ``ra_rsip_*`` operation, and scrubs the scratch buffer.
 *
 * @warning Stub backend; NOT cryptographically secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rsip_protected.h"

#include <stdint.h>

#include "ra8d2_rsip_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_rsip.h"
#include "ra_rsip_key_injection.h"
#include "ra_stack_budget.h"

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)

/**
 * @var s_tag
 * @brief Logger tag for this TU.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP_P";

/**
 * @enum ra_rsip_p_layout_t
 * @brief Local copy of the wrapped-key blob layout.
 *
 * @details
 * Mirrors ``ra_rsip_ki_layout_t`` from ``ra_rsip_key_injection.c``;
 * duplicated here so this TU does not depend on private internals
 * of the injection driver.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_rsip_p_off_payload = 20U, /**< Payload offset inside a wrapped blob. */
} ra_rsip_p_layout_t;

/**
 * @enum ra_rsip_p_const_t
 * @brief Sizing constants used by the protected layer.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_rsip_p_aes_max_bytes = 32U, /**< AES-256 key length.            */
  k_ra_rsip_p_iv_bytes      = 16U, /**< AES IV length.                 */
  k_ra_rsip_p_bits_per_byte = 8U,  /**< Width of a byte.               */
  k_ra_rsip_p_aes128_bytes  = 16U, /**< AES-128 raw-key bytes.         */
  k_ra_rsip_p_aes192_bytes  = 24U, /**< AES-192 raw-key bytes.         */
  k_ra_rsip_p_aes256_bytes  = 32U, /**< AES-256 raw-key bytes.         */
  k_ra_rsip_p_rsa_e_bytes   = 4U,  /**< RSA exponent bytes (stub).     */
} ra_rsip_p_const_t;

/**
 * @var s_p_aes_handle
 * @brief Latched AES handle for the protected-AES path.
 *
 * @details
 * Populated by ``ra_rsip_protected_aes_init`` and consumed by
 * ``ra_rsip_protected_aes_encrypt`` / ``..._decrypt``.
 *
 * @warning Direct modification is forbidden; the protected entry
 *          points are the only legitimate writers.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra_rsip_key_handle_t s_p_aes_handle;

/**
 * @var s_p_aes_iv
 * @brief Latched IV used by the protected-AES path.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static uint8_t s_p_aes_iv[k_ra_rsip_p_iv_bytes];

/**
 * @var s_p_aes_iv_set
 * @brief Whether ``s_p_aes_iv`` was populated by the caller.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static bool s_p_aes_iv_set;

/**
 * @var s_p_aes_mode
 * @brief Latched block-cipher mode for the protected-AES path.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra_rsip_aes_mode_t s_p_aes_mode;

/**
 * @var s_p_aes_active
 * @brief Whether ``ra_rsip_protected_aes_init`` has run since the
 *        last finish.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static bool s_p_aes_active;

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
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
static void p_scrub(uint8_t* buf, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    buf[i] = 0U;
  }
}

/**
 * @brief Drive the right ``ra_rsip_aes*_install_plain`` for the width.
 *
 * @param[in]  raw_key  Raw key bytes.
 * @param[in]  key_bits Width selector.
 * @param[out] out      Wrapped handle.
 * @return ``ra_err_t``.
 *
 * @pre ``raw_key`` and ``out`` non-NULL.
 * @pre ``raw_key`` is at least ``key_bits / 8`` bytes long.
 * @post On success, ``out->alg`` matches the install opcode.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
static ra_err_t
p_aes_install(const uint8_t* raw_key, ra_rsip_aes_key_bits_t key_bits, ra_rsip_key_handle_t* out)
{
  switch (key_bits) {
    case k_ra_rsip_aes_key_bits_128:
      return ra_rsip_aes128_install_plain(raw_key, out);
    case k_ra_rsip_aes_key_bits_192:
      return ra_rsip_aes192_install_plain(raw_key, out);
    case k_ra_rsip_aes_key_bits_256:
      return ra_rsip_aes256_install_plain(raw_key, out);
    default:
      return k_ra_err_invalid_arg;
  }
}

/**
 * @brief Implementation of ra_rsip_protected_aes_init (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] wrapped_key See implementation.
 * @param[in] key_bits See implementation.
 * @param[in] mode See implementation.
 * @param[in] iv See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @par Stack-budget deviation:
 *      Holds a 32-byte raw-key scratch + a full ra_rsip_key_handle_t on
 *      the stack (~1128 bytes total per the .su file) so the unwrapped
 *      key material is scrubbed via `p_scrub` when the frame unwinds
 *      and never lives in .bss. Considered alternatives (a static
 *      single-use scratch struct in .bss, a heap allocation) both
 *      defeat the scrub-on-return security property. See
 *      docs/STACK_USAGE.md and the per-app STACK_USAGE_BYTES override.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_aes_init(const uint8_t*         wrapped_key,
                                    ra_rsip_aes_key_bits_t key_bits,
                                    ra_rsip_aes_mode_t     mode,
                                    const uint8_t*         iv)
{
  /* cppcheck-suppress unknownMacro */
  RA_STACK_BUDGET(1128); /* unwrapped-key scratch; scrubbed on unwind */
  RA_CHECK_NULL_PTR(wrapped_key, s_tag, "p_aes_init: wrapped_key");

  ra_err_t rc = ra_rsip_key_validate(wrapped_key, k_ra_rsip_wrapped_type_aes);
  if (rc != k_ra_ok) {
    return rc;
  }

  uint8_t        raw_key[k_ra_rsip_p_aes_max_bytes] = {};
  const uint32_t n_bytes = (uint32_t)key_bits / (uint32_t)k_ra_rsip_p_bits_per_byte;
  if (n_bytes > (uint32_t)k_ra_rsip_p_aes_max_bytes) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < n_bytes; ++i) {
    raw_key[i] = wrapped_key[(uint32_t)k_ra_rsip_p_off_payload + i];
  }

  ra_rsip_key_handle_t handle = {};
  rc                          = p_aes_install(raw_key, key_bits, &handle);
  p_scrub(raw_key, n_bytes);
  if (rc != k_ra_ok) {
    return rc;
  }

  s_p_aes_handle = handle;
  s_p_aes_mode   = mode;
  s_p_aes_iv_set = (iv != nullptr);
  if (s_p_aes_iv_set) {
    for (uint32_t i = 0U; i < (uint32_t)k_ra_rsip_p_iv_bytes; ++i) {
      s_p_aes_iv[i] = iv[i];
    }
  } else {
    p_scrub(s_p_aes_iv, (uint32_t)k_ra_rsip_p_iv_bytes);
  }
  s_p_aes_active = true;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_rsip_protected_aes_encrypt (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] plaintext See implementation.
 * @param[in] ciphertext See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len)
{
  if (!s_p_aes_active) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(plaintext, s_tag, "p_aes_encrypt: plaintext");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "p_aes_encrypt: ciphertext");
  return ra_rsip_aes_cipher(&s_p_aes_handle,
                            s_p_aes_mode,
                            k_ra_rsip_dir_encrypt,
                            s_p_aes_iv_set ? s_p_aes_iv : nullptr,
                            plaintext,
                            ciphertext,
                            len);
}

/**
 * @brief Implementation of ra_rsip_protected_aes_decrypt (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] ciphertext See implementation.
 * @param[in] plaintext See implementation.
 * @param[in] len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len)
{
  if (!s_p_aes_active) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "p_aes_decrypt: ciphertext");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "p_aes_decrypt: plaintext");
  return ra_rsip_aes_cipher(&s_p_aes_handle,
                            s_p_aes_mode,
                            k_ra_rsip_dir_decrypt,
                            s_p_aes_iv_set ? s_p_aes_iv : nullptr,
                            ciphertext,
                            plaintext,
                            len);
}

/**
 * @brief Implementation of ra_rsip_protected_aes_finish (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_aes_finish(void)
{
  if (!s_p_aes_active) {
    return k_ra_err_invalid_state;
  }
  uint8_t* h = (uint8_t*)&s_p_aes_handle;
  p_scrub(h, (uint32_t)sizeof(s_p_aes_handle));
  p_scrub(s_p_aes_iv, (uint32_t)k_ra_rsip_p_iv_bytes);
  s_p_aes_iv_set = false;
  s_p_aes_active = false;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_rsip_protected_rsa_decrypt (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] wrapped_priv See implementation.
 * @param[in] size See implementation.
 * @param[in] ciphertext See implementation.
 * @param[in] ciphertext_len See implementation.
 * @param[in] plaintext_out See implementation.
 * @param[in] plaintext_cap See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @par Stack-budget deviation:
 *      Holds a 512-byte RSA-4096 modulus scratch + ra_rsip_key_handle_t
 *      + a 16-byte install IV (~1720 bytes total per the .su file) on
 *      the stack so the unwrapped private modulus is scrubbed via
 *      `p_scrub` when the frame unwinds. Moving the modulus into .bss
 *      would either persist the secret across calls or require an
 *      explicit clear-on-exit path that doubles the attack surface.
 *      See docs/STACK_USAGE.md.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_rsa_decrypt(const uint8_t*     wrapped_priv,
                                       ra_rsip_rsa_size_t size,
                                       const uint8_t*     ciphertext,
                                       uint32_t           ciphertext_len,
                                       uint8_t*           plaintext_out,
                                       uint32_t           plaintext_cap)
{
  /* cppcheck-suppress unknownMacro */
  RA_STACK_BUDGET(1720); /* RSA-4096 modulus scratch; scrubbed on unwind */
  RA_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_rsa_decrypt: wrapped_priv");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "p_rsa_decrypt: ciphertext");
  RA_CHECK_NULL_PTR(plaintext_out, s_tag, "p_rsa_decrypt: plaintext_out");

  /* The injection layer tags both private and public RSA blobs with
   * the public type tag (single-tag stub); accept either. */
  ra_err_t rc = ra_rsip_key_validate(wrapped_priv, k_ra_rsip_wrapped_type_rsa_pub);
  if (rc != k_ra_ok) {
    rc = ra_rsip_key_validate(wrapped_priv, k_ra_rsip_wrapped_type_rsa_priv);
    if (rc != k_ra_ok) {
      return rc;
    }
  }

  uint32_t mod_bytes = 0U;
  switch (size) {
    case k_ra_rsip_rsa_1024:
      mod_bytes = 128U;
      break;
    case k_ra_rsip_rsa_2048:
      mod_bytes = 256U;
      break;
    case k_ra_rsip_rsa_3072:
      mod_bytes = 384U;
      break;
    case k_ra_rsip_rsa_4096:
      mod_bytes = 512U;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if (plaintext_cap < mod_bytes) {
    return k_ra_err_invalid_arg;
  }
  if (ciphertext_len > mod_bytes) {
    return k_ra_err_invalid_arg;
  }

  /* Re-install the wrapped key under the public-RSA opcode so the
   * downstream ``ra_rsip_rsa_sign`` accepts it (the stub recognises
   * ``k_ra_rsip_oem_cmd_rsa*`` algorithm tags). The raw modulus is
   * recovered from the payload and pushed through the install path
   * just like the unprotected install entry points would. */
  uint8_t              modulus[k_ra_rsip_wrapped_max_payload] = {};
  ra_rsip_key_handle_t handle                                 = {};
  for (uint32_t i = 0U; i < mod_bytes; ++i) {
    modulus[i] = wrapped_priv[(uint32_t)k_ra_rsip_p_off_payload + i];
  }
  /* Use the OEM install path for RSA private keys: the wrapped layout
   * fed in here is already the engine-acceptable blob plus a 16-byte
   * IV that the stub validates trivially. */
  uint8_t           install_iv[k_ra_rsip_p_iv_bytes] = {};
  ra_rsip_oem_cmd_t install_cmd                      = k_ra_rsip_oem_cmd_invalid;
  if (size == k_ra_rsip_rsa_2048) {
    install_cmd = k_ra_rsip_oem_cmd_rsa2048_priv;
  } else if (size == k_ra_rsip_rsa_3072) {
    install_cmd = k_ra_rsip_oem_cmd_rsa3072_priv;
  } else if (size == k_ra_rsip_rsa_4096) {
    install_cmd = k_ra_rsip_oem_cmd_rsa4096_priv;
  }
  rc = ra_rsip_oem_install(install_cmd, install_iv, modulus, mod_bytes, &handle);
  p_scrub(modulus, mod_bytes);
  if (rc != k_ra_ok) {
    return rc;
  }

  /* RSA "private decrypt" maps onto ra_rsip_rsa_sign in the stub --
   * both drive the engine's modular-exponentiation path. */
  return ra_rsip_rsa_sign(&handle, size, ciphertext, ciphertext_len, plaintext_out);
}

/**
 * @brief Implementation of ra_rsip_protected_ecdsa_sign (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] wrapped_priv See implementation.
 * @param[in] curve See implementation.
 * @param[in] hash See implementation.
 * @param[in] hash_len See implementation.
 * @param[in] sig_out See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @par Stack-budget deviation:
 *      Holds an ECC private-key scratch + a full ra_rsip_key_handle_t
 *      on the stack (~1104 bytes total per the .su file) so the
 *      unwrapped private scalar is scrubbed via `p_scrub` when the
 *      frame unwinds. The same scrub-on-return security property
 *      forbids moving the buffer into .bss. See docs/STACK_USAGE.md.
 * @since 0.1.0
 */
ra_err_t ra_rsip_protected_ecdsa_sign(const uint8_t*  wrapped_priv,
                                      ra_rsip_curve_t curve,
                                      const uint8_t*  hash,
                                      uint32_t        hash_len,
                                      uint8_t*        sig_out)
{
  /* cppcheck-suppress unknownMacro */
  RA_STACK_BUDGET(1104); /* ECC private scalar scratch; scrubbed on unwind */
  RA_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_ecdsa_sign: wrapped_priv");
  RA_CHECK_NULL_PTR(hash, s_tag, "p_ecdsa_sign: hash");
  RA_CHECK_NULL_PTR(sig_out, s_tag, "p_ecdsa_sign: sig_out");

  ra_err_t rc = ra_rsip_key_validate(wrapped_priv, k_ra_rsip_wrapped_type_ecc_priv);
  if (rc != k_ra_ok) {
    return rc;
  }

  /* Build a minimal ECC private handle for the engine. The stub's
   * ``ra_rsip_ecdsa_sign`` routes through ``internal_asym_run`` which
   * just needs a non-NULL handle whose ``alg`` carries an ECC opcode;
   * the wrapped body holds the private scalar bytes copied from the
   * payload. */
  ra_rsip_key_handle_t handle = {};
  uint32_t             priv_bytes;
  switch (curve) {
    case k_ra_rsip_curve_secp256r1:
      handle.alg = (uint32_t)k_ra_rsip_oem_cmd_ecc_secp256r1_priv;
      priv_bytes = 32U;
      break;
    case k_ra_rsip_curve_secp384r1:
      handle.alg = (uint32_t)k_ra_rsip_oem_cmd_ecc_secp384r1_priv;
      priv_bytes = 48U;
      break;
    case k_ra_rsip_curve_secp521r1:
      handle.alg = (uint32_t)k_ra_rsip_oem_cmd_ecc_secp521r1_priv;
      priv_bytes = 66U;
      break;
    case k_ra_rsip_curve_secp256k1:
      handle.alg = (uint32_t)k_ra_rsip_oem_cmd_ecc_secp256k1_priv;
      priv_bytes = 32U;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  handle.body_words = priv_bytes / sizeof(uint32_t);

  /* Copy the private scalar into the handle body word-aligned. */
  for (uint32_t i = 0U; i < priv_bytes; ++i) {
    ((uint8_t*)handle.body)[i] = wrapped_priv[(uint32_t)k_ra_rsip_p_off_payload + i];
  }

  rc = ra_rsip_ecdsa_sign(&handle, curve, hash, hash_len, sig_out);
  p_scrub((uint8_t*)handle.body, priv_bytes);
  return rc;
}

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity)
