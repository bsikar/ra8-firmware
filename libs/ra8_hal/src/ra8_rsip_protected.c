/**
 * @file ra8_rsip_protected.c
 * @brief RSIP protected-key HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API in
 * ``libs/ra8_hal/inc/ra8_rsip_protected.h``. Each entry point validates
 * the wrapped-key blob via ``ra8_rsip_key_validate``, unwraps the
 * payload into a private scratch buffer, dispatches the underlying
 * unprotected ``ra8_rsip_*`` operation, and scrubs the scratch buffer.
 *
 * @warning Stub backend; NOT cryptographically secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rsip_protected.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rsip.h"
#include "ra8_rsip_key_injection.h"
#include "ra8_rsip_regs.h"
#include "ra8_stack_budget.h"

/**
 * @var s_tag
 * @brief Logger tag for this TU.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP_P";

/** @brief RSA modulus / ECC private-scalar byte counts. */
typedef enum : uint16_t {
  k_rsa_1024_mod_bytes       = 128U, /**< RSA 1024 mod bytes.       */
  k_rsa_2048_mod_bytes       = 256U, /**< RSA 2048 mod bytes.       */
  k_rsa_3072_mod_bytes       = 384U, /**< RSA 3072 mod bytes.       */
  k_rsa_4096_mod_bytes       = 512U, /**< RSA 4096 mod bytes.       */
  k_ecc_secp256_priv_bytes   = 32U,  /**< ECC secp256 priv bytes.   */
  k_ecc_secp384r1_priv_bytes = 48U,  /**< ECC secp384r1 priv bytes. */
  k_ecc_secp521r1_priv_bytes = 66U,  /**< ECC secp521r1 priv bytes. */
} rsip_prot_size_t;

/** @brief Measured worst-case stack frames (bytes), scrubbed on unwind. */
typedef enum : uint16_t {
  k_unwrap_key_stack_bytes   = 1128U, /**< Unwrap key stack bytes.   */
  k_rsa4096_priv_stack_bytes = 1720U, /**< Rsa4096 priv stack bytes. */
  k_ecc_priv_stack_bytes     = 1104U, /**< ECC priv stack bytes.     */
} rsip_prot_stack_t;

/**
 * @enum ra8_rsip_p_layout_t
 * @brief Local copy of the wrapped-key blob layout.
 *
 * @details
 * Mirrors ``ra8_rsip_ki_layout_t`` from ``ra8_rsip_key_injection.c``;
 * duplicated here so this TU does not depend on private internals
 * of the injection driver.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_p_off_payload = 20U, /**< Payload offset inside a wrapped blob. */
} ra8_rsip_p_layout_t;

/**
 * @enum ra8_rsip_p_const_t
 * @brief Sizing constants used by the protected layer.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_p_aes_max_bytes = 32U, /**< AES-256 key length.        */
  k_ra8_rsip_p_iv_bytes      = 16U, /**< AES IV length.             */
  k_ra8_rsip_p_bits_per_byte = 8U,  /**< Width of a byte.           */
  k_ra8_rsip_p_aes128_bytes  = 16U, /**< AES-128 raw-key bytes.     */
  k_ra8_rsip_p_aes192_bytes  = 24U, /**< AES-192 raw-key bytes.     */
  k_ra8_rsip_p_aes256_bytes  = 32U, /**< AES-256 raw-key bytes.     */
  k_ra8_rsip_p_rsa_e_bytes   = 4U,  /**< RSA exponent bytes (stub). */
} ra8_rsip_p_const_t;

/**
 * @var s_p_aes_handle
 * @brief Latched AES handle for the protected-AES path.
 *
 * @details
 * Populated by ``ra8_rsip_protected_aes_init`` and consumed by
 * ``ra8_rsip_protected_aes_encrypt`` / ``..._decrypt``.
 *
 * @warning Direct modification is forbidden; the protected entry
 *          points are the only legitimate writers.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra8_rsip_key_handle_t s_p_aes_handle;

/**
 * @var s_p_aes_iv
 * @brief Latched IV used by the protected-AES path.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static uint8_t s_p_aes_iv[k_ra8_rsip_p_iv_bytes];

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
static ra8_rsip_aes_mode_t s_p_aes_mode;

/**
 * @var s_p_aes_active
 * @brief Whether ``ra8_rsip_protected_aes_init`` has run since the
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
RA8_INTERNAL
RA8_INTERNAL static void internal_p_scrub(uint8_t* buf, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    buf[i] = 0U;
  }
}

/**
 * @brief Drive the right ``ra8_rsip_aes*_install_plain`` for the width.
 *
 * @param[in]  raw_key  Raw key bytes.
 * @param[in]  key_bits Width selector.
 * @param[out] out      Wrapped handle.
 * @return ``ra8_err_t``.
 *
 * @pre ``raw_key`` and ``out`` non-NULL.
 * @pre ``raw_key`` is at least ``key_bits / 8`` bytes long.
 * @post On success, ``out->alg`` matches the install opcode.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_p_aes_install(const uint8_t*          raw_key,
                                                     ra8_rsip_aes_key_bits_t key_bits,
                                                     ra8_rsip_key_handle_t*  out)
{
  switch (key_bits) {
    case k_ra8_rsip_aes_key_bits_128:
      return ra8_rsip_aes128_install_plain(raw_key, out);
    case k_ra8_rsip_aes_key_bits_192:
      return ra8_rsip_aes192_install_plain(raw_key, out);
    case k_ra8_rsip_aes_key_bits_256:
      return ra8_rsip_aes256_install_plain(raw_key, out);
    default:
      return k_ra8_err_invalid_arg;
  }
}

ra8_err_t ra8_rsip_protected_aes_init(const uint8_t*          wrapped_key,
                                      ra8_rsip_aes_key_bits_t key_bits,
                                      ra8_rsip_aes_mode_t     mode,
                                      const uint8_t*          iv)
{
  /* cppcheck-suppress unknownMacro -- cppcheck runs without include paths and cannot expand RA8_STACK_BUDGET. */
  RA8_STACK_BUDGET(k_unwrap_key_stack_bytes); /* unwrapped-key scratch; scrubbed on unwind */
  RA8_CHECK_NULL_PTR(wrapped_key, s_tag, "p_aes_init: wrapped_key");

  ra8_err_t rc = ra8_rsip_key_validate(wrapped_key, k_ra8_rsip_wrapped_type_aes);
  if (rc != k_ra8_ok) {
    return rc;
  }

  uint8_t        raw_key[k_ra8_rsip_p_aes_max_bytes] = {};
  const uint32_t n_bytes = (uint32_t)key_bits / (uint32_t)k_ra8_rsip_p_bits_per_byte;
  if (n_bytes > (uint32_t)k_ra8_rsip_p_aes_max_bytes) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < n_bytes; ++i) {
    raw_key[i] = wrapped_key[(uint32_t)k_ra8_rsip_p_off_payload + i];
  }

  ra8_rsip_key_handle_t handle = {};
  rc                           = internal_p_aes_install(raw_key, key_bits, &handle);
  internal_p_scrub(raw_key, n_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }

  s_p_aes_handle = handle;
  s_p_aes_mode   = mode;
  s_p_aes_iv_set = (iv != nullptr);
  if (s_p_aes_iv_set) {
    for (uint32_t i = 0U; i < (uint32_t)k_ra8_rsip_p_iv_bytes; ++i) {
      s_p_aes_iv[i] = iv[i];
    }
  } else {
    internal_p_scrub(s_p_aes_iv, (uint32_t)k_ra8_rsip_p_iv_bytes);
  }
  s_p_aes_active = true;
  return k_ra8_ok;
}

ra8_err_t
ra8_rsip_protected_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len)
{
  if (!s_p_aes_active) {
    return k_ra8_err_invalid_state;
  }
  RA8_CHECK_NULL_PTR(plaintext, s_tag, "p_aes_encrypt: plaintext");
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "p_aes_encrypt: ciphertext");
  return ra8_rsip_aes_cipher(&s_p_aes_handle,
                             s_p_aes_mode,
                             k_ra8_rsip_dir_encrypt,
                             s_p_aes_iv_set ? s_p_aes_iv : nullptr,
                             plaintext,
                             ciphertext,
                             len);
}

ra8_err_t
ra8_rsip_protected_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len)
{
  if (!s_p_aes_active) {
    return k_ra8_err_invalid_state;
  }
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "p_aes_decrypt: ciphertext");
  RA8_CHECK_NULL_PTR(plaintext, s_tag, "p_aes_decrypt: plaintext");
  return ra8_rsip_aes_cipher(&s_p_aes_handle,
                             s_p_aes_mode,
                             k_ra8_rsip_dir_decrypt,
                             s_p_aes_iv_set ? s_p_aes_iv : nullptr,
                             ciphertext,
                             plaintext,
                             len);
}

ra8_err_t ra8_rsip_protected_aes_finish(void)
{
  if (!s_p_aes_active) {
    return k_ra8_err_invalid_state;
  }
  uint8_t* h = (uint8_t*)&s_p_aes_handle;
  internal_p_scrub(h, (uint32_t)sizeof(s_p_aes_handle));
  internal_p_scrub(s_p_aes_iv, (uint32_t)k_ra8_rsip_p_iv_bytes);
  s_p_aes_iv_set = false;
  s_p_aes_active = false;
  return k_ra8_ok;
}

/**
 * @brief Map an ::ra8_rsip_rsa_size_t to its modulus byte count.
 *
 * @details
 * Lookup helper used by the protected RSA path; the engine's
 * modular-exponentiation surface accepts only the four canonical key
 * sizes. Anything else is rejected.
 *
 * @param[in]  size       RSA key-size enum.
 * @param[out] out_bytes  Receives 128/256/384/512 on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Valid size mapped.
 * @retval k_ra8_err_invalid_arg   Unknown enum value.
 *
 * @pre ``out_bytes`` non-NULL.
 * @pre Caller has validated the wrapped key blob.
 * @post On success ``*out_bytes`` holds the modulus length in bytes.
 * @post On error ``*out_bytes`` is unchanged.
 *
 * @note Pure function; no side effects.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rsa_mod_bytes(ra8_rsip_rsa_size_t size, uint32_t* out_bytes)
{
  switch (size) {
    case k_ra8_rsip_rsa_1024:
      *out_bytes = k_rsa_1024_mod_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_rsa_2048:
      *out_bytes = k_rsa_2048_mod_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_rsa_3072:
      *out_bytes = k_rsa_3072_mod_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_rsa_4096:
      *out_bytes = k_rsa_4096_mod_bytes;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Resolve the OEM install opcode for an RSA private key size.
 *
 * @details
 * The stub install table is keyed by ::ra8_rsip_oem_cmd_t; this helper
 * collapses the size-to-opcode mapping so the public entry point does
 * not have to repeat the switch in line.
 *
 * @param[in] size RSA key-size enum.
 *
 * @return ::ra8_rsip_oem_cmd_t value; ``k_ra8_rsip_oem_cmd_invalid`` for
 *         sizes not represented in the OEM install path (RSA-1024).
 * @retval k_ra8_rsip_oem_cmd_rsa2048_priv  size == k_ra8_rsip_rsa_2048.
 * @retval k_ra8_rsip_oem_cmd_rsa3072_priv  size == k_ra8_rsip_rsa_3072.
 * @retval k_ra8_rsip_oem_cmd_rsa4096_priv  size == k_ra8_rsip_rsa_4096.
 * @retval k_ra8_rsip_oem_cmd_invalid       size is RSA-1024 or unknown.
 *
 * @pre Caller has validated ``size`` via internal_rsa_mod_bytes().
 * @pre ``size`` is a value of ::ra8_rsip_rsa_size_t.
 * @post Return value is one of the documented opcodes or _invalid.
 * @post No global or module-private state is mutated.
 *
 * @note Pure function; reentrant and ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_rsip_oem_cmd_t internal_rsa_install_cmd(ra8_rsip_rsa_size_t size)
{
  if (size == k_ra8_rsip_rsa_2048) {
    return k_ra8_rsip_oem_cmd_rsa2048_priv;
  }
  if (size == k_ra8_rsip_rsa_3072) {
    return k_ra8_rsip_oem_cmd_rsa3072_priv;
  }
  if (size == k_ra8_rsip_rsa_4096) {
    return k_ra8_rsip_oem_cmd_rsa4096_priv;
  }
  return k_ra8_rsip_oem_cmd_invalid;
}

/**
 * @brief Accept a wrapped RSA blob tagged with either RSA type tag.
 *
 * @details
 * The injection layer tags both private and public RSA blobs with the
 * public type tag (single-tag stub); this helper accepts either tag so
 * the protected entry points do not repeat the two-step validate.
 *
 * @param[in] wrapped_priv Wrapped RSA key blob.
 *
 * @return ``ra8_err_t`` error code from ::ra8_rsip_key_validate.
 * @retval k_ra8_ok  Blob carries a valid RSA public or private tag.
 *
 * @pre ``wrapped_priv`` is non-NULL (checked by the caller).
 * @pre The injection driver is initialized.
 * @post No state is mutated.
 * @post Return value depends only on the blob contents.
 *
 * @note Pure validation helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rsa_validate_wrapped(const uint8_t* wrapped_priv)
{
  const ra8_err_t rc = ra8_rsip_key_validate(wrapped_priv, k_ra8_rsip_wrapped_type_rsa_pub);
  if (rc == k_ra8_ok) {
    return k_ra8_ok;
  }
  return ra8_rsip_key_validate(wrapped_priv, k_ra8_rsip_wrapped_type_rsa_priv);
}

/**
 * @brief Recover the modulus from a wrapped blob and OEM-install it.
 *
 * @details
 * Re-installs the wrapped key under the RSA OEM opcode so the
 * downstream ``ra8_rsip_rsa_sign`` accepts it (the stub recognises
 * ``k_ra8_rsip_oem_cmd_rsa*`` algorithm tags). The raw modulus is
 * recovered from the payload and pushed through the OEM install path
 * just like the unprotected install entry points would; the stack
 * scratch copy is scrubbed before returning.
 *
 * @param[in]  wrapped_priv Wrapped RSA private-key blob.
 * @param[in]  size         RSA key-size enum (already validated).
 * @param[in]  mod_bytes    Modulus byte count for ``size``.
 * @param[out] out_handle   Receives the installed key handle.
 *
 * @return ``ra8_err_t`` error code from ::ra8_rsip_oem_install.
 * @retval k_ra8_ok  Key installed; ``*out_handle`` is live.
 *
 * @pre ``wrapped_priv`` passed ::internal_rsa_validate_wrapped.
 * @pre ``mod_bytes`` came from ::internal_rsa_mod_bytes for ``size``.
 * @post The modulus stack scratch has been scrubbed.
 * @post On error ``*out_handle`` holds no usable key material.
 *
 * @note Not thread-safe; single secure-dispatch context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rsa_install_priv(const uint8_t*         wrapped_priv,
                                           ra8_rsip_rsa_size_t    size,
                                           uint32_t               mod_bytes,
                                           ra8_rsip_key_handle_t* out_handle)
{
  uint8_t modulus[k_ra8_rsip_wrapped_max_payload] = {};
  for (uint32_t i = 0U; i < mod_bytes; ++i) {
    modulus[i] = wrapped_priv[(uint32_t)k_ra8_rsip_p_off_payload + i];
  }
  /* Use the OEM install path for RSA private keys: the wrapped layout
   * fed in here is already the engine-acceptable blob plus a 16-byte
   * IV that the stub validates trivially. */
  uint8_t                  install_iv[k_ra8_rsip_p_iv_bytes] = {};
  const ra8_rsip_oem_cmd_t install_cmd                       = internal_rsa_install_cmd(size);
  const ra8_err_t          rc =
    ra8_rsip_oem_install(install_cmd, install_iv, modulus, mod_bytes, out_handle);
  internal_p_scrub(modulus, mod_bytes);
  return rc;
}

ra8_err_t ra8_rsip_protected_rsa_decrypt(const uint8_t*      wrapped_priv,
                                         ra8_rsip_rsa_size_t size,
                                         const uint8_t*      ciphertext,
                                         uint32_t            ciphertext_len,
                                         uint8_t*            plaintext_out,
                                         uint32_t            plaintext_cap)
{
  /* cppcheck-suppress unknownMacro -- cppcheck runs without include paths and cannot expand RA8_STACK_BUDGET. */
  RA8_STACK_BUDGET(k_rsa4096_priv_stack_bytes); /* RSA-4096 modulus scratch; scrubbed on unwind */
  RA8_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_rsa_decrypt: wrapped_priv");
  RA8_CHECK_NULL_PTR(ciphertext, s_tag, "p_rsa_decrypt: ciphertext");
  RA8_CHECK_NULL_PTR(plaintext_out, s_tag, "p_rsa_decrypt: plaintext_out");

  ra8_err_t rc = internal_rsa_validate_wrapped(wrapped_priv);
  if (rc != k_ra8_ok) {
    return rc;
  }

  uint32_t mod_bytes = 0U;
  rc                 = internal_rsa_mod_bytes(size, &mod_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (plaintext_cap < mod_bytes) {
    return k_ra8_err_invalid_arg;
  }
  if (ciphertext_len > mod_bytes) {
    return k_ra8_err_invalid_arg;
  }

  ra8_rsip_key_handle_t handle = {};
  rc                           = internal_rsa_install_priv(wrapped_priv, size, mod_bytes, &handle);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* RSA "private decrypt" maps onto ra8_rsip_rsa_sign in the stub --
   * both drive the engine's modular-exponentiation path. */
  return ra8_rsip_rsa_sign(&handle, size, ciphertext, ciphertext_len, plaintext_out);
}

/**
 * @brief Map an ::ra8_rsip_curve_t to its OEM opcode and scalar size.
 *
 * @details
 * Lookup helper for the protected ECDSA path: resolves the engine
 * algorithm tag (``k_ra8_rsip_oem_cmd_ecc_*_priv``) and the private
 * scalar byte count for each supported curve. Unknown curve values
 * are rejected so the caller never builds a handle from them.
 *
 * @param[in]  curve          ECC curve selector.
 * @param[out] out_alg        Receives the OEM install opcode value.
 * @param[out] out_priv_bytes Receives the private scalar byte count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Curve mapped; both outputs written.
 * @retval k_ra8_err_invalid_arg  Unknown curve enum value.
 *
 * @pre ``out_alg`` and ``out_priv_bytes`` are non-NULL.
 * @pre ``curve`` is a value of ::ra8_rsip_curve_t.
 * @post On success both outputs describe the selected curve.
 * @post On error neither output has been written.
 *
 * @note Pure function; reentrant and ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_ecc_priv_params(ra8_rsip_curve_t curve, uint32_t* out_alg, uint32_t* out_priv_bytes)
{
  switch (curve) {
    case k_ra8_rsip_curve_secp256r1:
      *out_alg        = (uint32_t)k_ra8_rsip_oem_cmd_ecc_secp256r1_priv;
      *out_priv_bytes = (uint32_t)k_ecc_secp256_priv_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_curve_secp384r1:
      *out_alg        = (uint32_t)k_ra8_rsip_oem_cmd_ecc_secp384r1_priv;
      *out_priv_bytes = (uint32_t)k_ecc_secp384r1_priv_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_curve_secp521r1:
      *out_alg        = (uint32_t)k_ra8_rsip_oem_cmd_ecc_secp521r1_priv;
      *out_priv_bytes = (uint32_t)k_ecc_secp521r1_priv_bytes;
      return k_ra8_ok;
    case k_ra8_rsip_curve_secp256k1:
      *out_alg        = (uint32_t)k_ra8_rsip_oem_cmd_ecc_secp256k1_priv;
      *out_priv_bytes = (uint32_t)k_ecc_secp256_priv_bytes;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

ra8_err_t ra8_rsip_protected_ecdsa_sign(const uint8_t*   wrapped_priv,
                                        ra8_rsip_curve_t curve,
                                        const uint8_t*   hash,
                                        uint32_t         hash_len,
                                        uint8_t*         sig_out)
{
  /* cppcheck-suppress unknownMacro -- cppcheck runs without include paths and cannot expand RA8_STACK_BUDGET. */
  RA8_STACK_BUDGET(k_ecc_priv_stack_bytes); /* ECC private scalar scratch; scrubbed on unwind */
  RA8_CHECK_NULL_PTR(wrapped_priv, s_tag, "p_ecdsa_sign: wrapped_priv");
  RA8_CHECK_NULL_PTR(hash, s_tag, "p_ecdsa_sign: hash");
  RA8_CHECK_NULL_PTR(sig_out, s_tag, "p_ecdsa_sign: sig_out");

  ra8_err_t rc = ra8_rsip_key_validate(wrapped_priv, k_ra8_rsip_wrapped_type_ecc_priv);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* Build a minimal ECC private handle for the engine. The stub's
   * ``ra8_rsip_ecdsa_sign`` routes through ``internal_asym_run`` which
   * just needs a non-NULL handle whose ``alg`` carries an ECC opcode;
   * the wrapped body holds the private scalar bytes copied from the
   * payload. */
  ra8_rsip_key_handle_t handle     = {};
  uint32_t              priv_bytes = 0U;
  rc                               = internal_ecc_priv_params(curve, &handle.alg, &priv_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  handle.body_words = priv_bytes / sizeof(uint32_t);

  /* Copy the private scalar into the handle body word-aligned. */
  for (uint32_t i = 0U; i < priv_bytes; ++i) {
    ((uint8_t*)handle.body)[i] = wrapped_priv[(uint32_t)k_ra8_rsip_p_off_payload + i];
  }

  rc = ra8_rsip_ecdsa_sign(&handle, curve, hash, hash_len, sig_out);
  internal_p_scrub((uint8_t*)handle.body, priv_bytes);
  return rc;
}
