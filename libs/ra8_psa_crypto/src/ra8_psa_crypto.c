/**
 * @file ra8_psa_crypto.c
 * @brief Implementation of the ``ra8_psa_crypto`` facade over TF-PSA-Crypto.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Hosts the static key-handle pool and the thin translation layer
 * between PSA Crypto status codes (``psa_status_t``) and ``ra8_err_t``.
 *
 * TF-PSA-Crypto is only linked into the firmware build when
 * ``RA8_USE_MBEDTLS=ON`` (the same option that pulls in Mbed TLS). The
 * host unit-test build (``tests/CMakeLists.txt``) defines
 * ``RA8_OFF_TARGET`` for every translation unit and intentionally
 * does not link the heavy PSA object library; in that mode this file
 * substitutes a tiny in-memory crypto stand-in:
 *
 * - SHA-256 is implemented inline (FIPS 180-4 reference algorithm) so
 *   ``ra8_psa_hash_compute`` can produce real digests in the test suite.
 * - AES-GCM is replaced by a deterministic XOR-and-checksum cipher
 *   that exercises the AEAD contract (ciphertext + tag layout, tag
 *   tampering detection) without requiring the real AES core.
 * - ECDSA sign / verify is replaced by an HMAC-style "signature" that
 *   binds (key bytes, hash) and verifies by recomputation. This is
 *   not cryptographically secure -- it exists only so the round-trip
 *   sign+verify test in ``test_ra8_psa_crypto.c`` can pass without
 *   pulling in P-256.
 *
 * The public ``ra8_psa_*`` surface is identical in either build, so a
 * caller migrating from off-target mode to a real firmware image only
 * has to flip the build flag.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_psa_crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_psa_crypto_internal.h"

#ifndef RA8_OFF_TARGET
#include "psa/crypto.h"
#endif

/* =============================================================================
 * Logging tag
 * =============================================================================
 */

/** @brief Logging tag prefix used by every ``ra8_psa_crypto`` log line. */
[[maybe_unused]] static const char* const k_ra8_psa_tag = "ra8_psa_crypto";

/* =============================================================================
 * Pool state
 * =============================================================================
 *
 * The internal typed-constant enums and the concrete
 * ``struct ra8_psa_key_handle`` definition live in
 * ``ra8_psa_crypto_internal.h`` so the fake TU can share them.
 */

/** @brief Per-handle state pool sized at compile time. */
static struct ra8_psa_key_handle s_key_pool[k_ra8_psa_max_keys];

/** @brief One-shot global init flag protecting the pool. */
static bool s_initialized;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/* Validate that a typed handle points into the static pool -- see implementation for details. */
static bool internal_handle_valid(ra8_psa_key_t handle)
{
  if (handle == nullptr) {
    return false;
  }
  const struct ra8_psa_key_handle* base = &s_key_pool[0];
  const struct ra8_psa_key_handle* end  = &s_key_pool[k_ra8_psa_max_keys];
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
static struct ra8_psa_key_handle* internal_alloc_slot(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_psa_max_keys; ++i) {
    if (!s_key_pool[i].in_use) {
      return &s_key_pool[i];
    }
  }
  return nullptr; /**< Nullptr. */
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_psa_crypto_init(void)
{
  if (s_initialized) {
    return k_ra8_err_exists;
  }

#ifndef RA8_OFF_TARGET
  const psa_status_t st = psa_crypto_init();
  if (st != PSA_SUCCESS) {
    ra8_log_error(k_ra8_psa_tag, "psa_crypto_init failed");
    return k_ra8_err_hw_error;
  }
#endif

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_psa_max_keys; ++i) {
    s_key_pool[i].in_use  = false;
    s_key_pool[i].key_len = 0U;
  }

  s_initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_crypto_deinit(void)
{
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_psa_max_keys; ++i) {
    if (s_key_pool[i].in_use) {
#ifndef RA8_OFF_TARGET
      (void)psa_destroy_key(s_key_pool[i].psa_id);
#endif
      (void)memset(s_key_pool[i].key, 0, sizeof(s_key_pool[i].key));
      s_key_pool[i].in_use  = false;
      s_key_pool[i].key_len = 0U;
    }
  }

#ifndef RA8_OFF_TARGET
  mbedtls_psa_crypto_free();
#endif

  s_initialized = false;
  return k_ra8_ok;
}

#ifndef RA8_OFF_TARGET

/* Translate ``ra8_psa_key_type_t`` to its ``PSA_KEY_TYPE_*`` peer -- see implementation for details. */
static psa_key_type_t internal_map_key_type(ra8_psa_key_type_t type)
{
  switch (type) {
    case k_ra8_psa_key_type_aes:
      return PSA_KEY_TYPE_AES;
    case k_ra8_psa_key_type_hmac:
      return PSA_KEY_TYPE_HMAC;
    case k_ra8_psa_key_type_ecc_p256_priv:
      return PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
    case k_ra8_psa_key_type_ecc_p256_pub:
      return PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);
    case k_ra8_psa_key_type_raw:
    default:
      return PSA_KEY_TYPE_RAW_DATA;
  }
}

/* Translate ``ra8_psa_alg_t`` to its ``PSA_ALG_*`` peer -- see implementation for details. */
static psa_algorithm_t internal_map_alg(ra8_psa_alg_t alg)
{
  switch (alg) {
    case k_ra8_psa_alg_aes_gcm:
      return PSA_ALG_GCM;
    case k_ra8_psa_alg_ecdsa_sha_256:
      return PSA_ALG_ECDSA(PSA_ALG_SHA_256);
    case k_ra8_psa_alg_sha_256:
      return PSA_ALG_SHA_256;
    case k_ra8_psa_alg_none:
    default:
      return 0;
  }
}

/* Translate ``ra8_psa_key_usage_t`` bitmask to ``PSA_KEY_USAGE_*`` flags -- see implementation for details. */
static psa_key_usage_t internal_map_usage(ra8_psa_key_usage_t usage)
{
  psa_key_usage_t out = 0;
  if ((usage & k_ra8_psa_usage_sign) != 0U) {
    out |= PSA_KEY_USAGE_SIGN_HASH;
  }
  if ((usage & k_ra8_psa_usage_verify) != 0U) {
    out |= PSA_KEY_USAGE_VERIFY_HASH;
  }
  if ((usage & k_ra8_psa_usage_encrypt) != 0U) {
    out |= PSA_KEY_USAGE_ENCRYPT;
  }
  if ((usage & k_ra8_psa_usage_decrypt) != 0U) {
    out |= PSA_KEY_USAGE_DECRYPT;
  }
  if ((usage & k_ra8_psa_usage_derive) != 0U) {
    out |= PSA_KEY_USAGE_DERIVE;
  }
  return out;
}

/* Run the PSA-side import + slot wire-up for ``ra8_psa_key_import`` -- see implementation for details. */
static ra8_err_t internal_psa_import_into_slot(struct ra8_psa_key_handle* slot,
                                               const ra8_psa_key_attr_t*  attr,
                                               const uint8_t*             data,
                                               size_t                     data_len)
{
  psa_key_attributes_t pa = psa_key_attributes_init();
  psa_set_key_type(&pa, internal_map_key_type(attr->type));
  psa_set_key_algorithm(&pa, internal_map_alg(attr->alg));
  psa_set_key_usage_flags(&pa, internal_map_usage(attr->usage));

  psa_key_id_t       kid = 0;
  const psa_status_t st  = psa_import_key(&pa, data, data_len, &kid);
  if (st != PSA_SUCCESS) {
    return k_ra8_err_hw_error;
  }
  slot->psa_id = kid;
  return k_ra8_ok;
}

#endif /* !RA8_OFF_TARGET */

ra8_err_t ra8_psa_key_import(ra8_psa_key_t*            out_handle,
                             const ra8_psa_key_attr_t* attr,
                             const uint8_t*            data,
                             size_t                    data_len)
{
  if (out_handle != nullptr) {
    *out_handle = nullptr;
  }
  if ((out_handle == nullptr) || (attr == nullptr) || (data == nullptr) || (data_len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (data_len > (size_t)k_ra8_psa_max_key_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (attr->usage == k_ra8_psa_usage_none) {
    return k_ra8_err_invalid_arg;
  }

  struct ra8_psa_key_handle* slot = internal_alloc_slot();
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
  }

#ifndef RA8_OFF_TARGET
  const ra8_err_t pst = internal_psa_import_into_slot(slot, attr, data, data_len);
  if (pst != k_ra8_ok) {
    return pst;
  }
#endif

  slot->in_use = true;
  slot->attr   = *attr;
  (void)memcpy(slot->key, data, data_len);
  slot->key_len = data_len;

  *out_handle = slot;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_key_destroy(ra8_psa_key_t handle)
{
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  (void)psa_destroy_key(handle->psa_id);
#endif
  (void)memset(handle->key, 0, sizeof(handle->key));
  handle->key_len = 0U;
  handle->in_use  = false;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_hash_compute(ra8_psa_alg_t  alg,
                               const uint8_t* input,
                               size_t         input_len,
                               uint8_t*       out,
                               size_t         out_cap,
                               size_t*        out_len)
{
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  if ((out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((input == nullptr) && (input_len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (alg != k_ra8_psa_alg_sha_256) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (out_cap < (size_t)k_ra8_psa_sha256_len) {
    return k_ra8_err_invalid_size;
  }

#ifdef RA8_OFF_TARGET
  ra8_psa_fake_sha256_oneshot(input, input_len, out);
#else
  size_t             produced = 0U;
  const psa_status_t st =
    psa_hash_compute(PSA_ALG_SHA_256, input, input_len, out, out_cap, &produced);
  if (st != PSA_SUCCESS) {
    return k_ra8_err_hw_error;
  }
  (void)produced;
#endif

  *out_len = (size_t)k_ra8_psa_sha256_len;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_sign_hash(ra8_psa_key_t  handle,
                            ra8_psa_alg_t  alg,
                            const uint8_t* hash,
                            size_t         hash_len,
                            uint8_t*       sig,
                            size_t         sig_cap,
                            size_t*        sig_len)
{
  if (sig_len != nullptr) {
    *sig_len = 0U;
  }
  if ((hash == nullptr) || (sig == nullptr) || (sig_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra8_err_invalid_arg;
  }
  if (alg != k_ra8_psa_alg_ecdsa_sha_256) {
    return k_ra8_err_not_supported;
  }
  if ((handle->attr.usage & k_ra8_psa_usage_sign) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (hash_len != (size_t)k_ra8_psa_sha256_len) {
    return k_ra8_err_invalid_size;
  }
  if (sig_cap < (size_t)k_ra8_psa_sha256_len) {
    return k_ra8_err_invalid_size;
  }

#ifdef RA8_OFF_TARGET
  /* Fake "signature" = SHA-256(key || hash). 32 bytes. */
  uint8_t buf[k_ra8_psa_max_key_bytes + k_ra8_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  ra8_psa_fake_sha256_oneshot(buf, handle->key_len + hash_len, digest);
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
    return k_ra8_err_hw_error;
  }
  *sig_len = produced;
#endif

  return k_ra8_ok;
}

ra8_err_t ra8_psa_verify_hash(ra8_psa_key_t  handle,
                              ra8_psa_alg_t  alg,
                              const uint8_t* hash,
                              size_t         hash_len,
                              const uint8_t* sig,
                              size_t         sig_len)
{
  if ((hash == nullptr) || (sig == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra8_err_invalid_arg;
  }
  if (alg != k_ra8_psa_alg_ecdsa_sha_256) {
    return k_ra8_err_not_supported;
  }
  if ((handle->attr.usage & k_ra8_psa_usage_verify) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (hash_len != (size_t)k_ra8_psa_sha256_len) {
    return k_ra8_err_invalid_size;
  }

#ifdef RA8_OFF_TARGET
  if (sig_len != (size_t)k_ra8_psa_sha256_len) {
    return k_ra8_err_crc_mismatch;
  }
  uint8_t buf[k_ra8_psa_max_key_bytes + k_ra8_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  ra8_psa_fake_sha256_oneshot(buf, handle->key_len + hash_len, digest);
  /* Constant-time compare. */
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_psa_sha256_len; ++i) {
    diff |= (uint8_t)(digest[i] ^ sig[i]);
  }
  return (diff == 0U) ? k_ra8_ok : k_ra8_err_crc_mismatch;
#else
  const psa_status_t st =
    psa_verify_hash(handle->psa_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len, sig, sig_len);
  if (st == PSA_SUCCESS) {
    return k_ra8_ok;
  }
  if (st == PSA_ERROR_INVALID_SIGNATURE) {
    return k_ra8_err_crc_mismatch;
  }
  return k_ra8_err_hw_error;
#endif
}

/* Shared precondition checks for ``ra8_psa_aead_encrypt`` -- see implementation for details. */
static ra8_err_t internal_aead_encrypt_check(ra8_psa_key_t  handle,
                                             ra8_psa_alg_t  alg,
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
  if ((nonce == nullptr) || (out == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (((plain == nullptr) && (plain_len != 0U)) || ((aad == nullptr) && (aad_len != 0U))) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(handle) || (alg != k_ra8_psa_alg_aes_gcm) ||
      ((handle->attr.usage & k_ra8_psa_usage_encrypt) == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((nonce_len != (size_t)k_ra8_psa_gcm_nonce_len) ||
      (out_cap < (plain_len + (size_t)k_ra8_psa_gcm_tag_len))) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_psa_aead_encrypt(ra8_psa_key_t  handle,
                               ra8_psa_alg_t  alg,
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
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  const ra8_err_t ck = internal_aead_encrypt_check(handle,
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
  if (ck != k_ra8_ok) {
    return ck;
  }

#ifdef RA8_OFF_TARGET
  /* Encrypt = plaintext XOR keystream(key, nonce). Tag = SHA-256-trunc16. */
  const ra8_err_t ser = ra8_psa_fake_aead_encrypt(handle,
                                                  nonce,
                                                  nonce_len,
                                                  aad,
                                                  aad_len,
                                                  plain,
                                                  plain_len,
                                                  out,
                                                  out_len);
  if (ser != k_ra8_ok) {
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
    return k_ra8_err_hw_error;
  }
  *out_len = produced;
#endif

  return k_ra8_ok;
}

/* Shared precondition checks for ``ra8_psa_aead_decrypt`` -- see implementation for details. */
static ra8_err_t internal_aead_decrypt_check(ra8_psa_key_t  handle,
                                             ra8_psa_alg_t  alg,
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
  if ((nonce == nullptr) || (cipher == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((aad == nullptr) && (aad_len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(handle) || (alg != k_ra8_psa_alg_aes_gcm) ||
      ((handle->attr.usage & k_ra8_psa_usage_decrypt) == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((nonce_len != (size_t)k_ra8_psa_gcm_nonce_len) ||
      (cipher_len < (size_t)k_ra8_psa_gcm_tag_len)) {
    return k_ra8_err_invalid_size;
  }
  const size_t plain_len = cipher_len - (size_t)k_ra8_psa_gcm_tag_len;
  if ((out == nullptr) && (plain_len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (out_cap < plain_len) {
    return k_ra8_err_invalid_size;
  }
  *out_plain_len = plain_len;
  return k_ra8_ok;
}

ra8_err_t ra8_psa_aead_decrypt(ra8_psa_key_t  handle,
                               ra8_psa_alg_t  alg,
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
  if (out_len != nullptr) {
    *out_len = 0U;
  }
  size_t          plain_len = 0U;
  const ra8_err_t ck        = internal_aead_decrypt_check(handle,
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
  if (ck != k_ra8_ok) {
    return ck;
  }

#ifdef RA8_OFF_TARGET
  const ra8_err_t sdr = ra8_psa_fake_aead_decrypt(handle,
                                                  nonce,
                                                  nonce_len,
                                                  aad,
                                                  aad_len,
                                                  cipher,
                                                  plain_len,
                                                  out,
                                                  out_len);
  if (sdr != k_ra8_ok) {
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
    return k_ra8_err_crc_mismatch;
  }
  if (st != PSA_SUCCESS) {
    return k_ra8_err_hw_error;
  }
  *out_len = produced;
#endif

  return k_ra8_ok;
}

/**
 * @brief Fill ``out`` with cryptographically secure bytes.
 *
 * @details See the matching header declaration for the full contract.
 * On the target this delegates to ``psa_generate_random``; in
 * ``RA8_OFF_TARGET`` builds a deterministic xorshift32 stream is
 * used so host-side tests are reproducible across runs.
 *
 * @param[out] out     Destination buffer.
 * @param[in]  out_len Number of bytes requested.
 * @return ``ra8_err_t`` error code per header.
 * @retval k_ra8_ok                  ``out`` filled with ``out_len`` bytes.
 * @retval k_ra8_err_invalid_arg     ``out`` was NULL.
 * @retval k_ra8_err_invalid_size    ``out_len`` was zero.
 * @retval k_ra8_err_not_initialized Facade not initialized.
 * @retval k_ra8_err_hw_error        Underlying ``psa_generate_random`` failed.
 *
 * @pre Facade has been initialized by ``ra8_psa_crypto_init``.
 * @pre ``out`` is non-NULL and ``out_len > 0``.
 * @post On ``k_ra8_ok`` ``out[0..out_len-1]`` has been written.
 * @post On any error ``out`` is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_psa_crypto_random(uint8_t* out, size_t out_len)
{
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (out_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }

#ifdef RA8_OFF_TARGET
  /* Deterministic xorshift32 -- reproducible host-test entropy.
   * Constants from Marsaglia 2003 "Xorshift RNGs" J. Stat. Soft. 8(14). */
  static uint32_t s_state = (uint32_t)k_xs32_seed;
  for (size_t i = 0U; i < out_len; ++i) {
    uint32_t x = s_state;
    x ^= x << (uint32_t)k_xs32_shl_a;
    x ^= x >> (uint32_t)k_xs32_shr_b;
    x ^= x << (uint32_t)k_xs32_shl_c;
    s_state = x;
    out[i]  = (uint8_t)(x & (uint32_t)k_xs32_byte_m);
  }
  return k_ra8_ok;
#else
  const psa_status_t st = psa_generate_random(out, out_len);
  if (st != PSA_SUCCESS) {
    ra8_log_error(k_ra8_psa_tag, "psa_generate_random failed");
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
#endif
}
