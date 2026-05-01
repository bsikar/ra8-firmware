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

#if !defined(RA_SIMULATOR_MODE)
#include "psa/crypto.h"
#endif

/* =============================================================================
 * Logging tag
 * =============================================================================
 */

/** @brief Logging tag prefix used by every ``ra_psa_crypto`` log line. */
static const char* const k_ra_psa_tag = "ra_psa_crypto";

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
  bool              in_use;                       /**< Slot allocated.            */
  ra_psa_key_attr_t attr;                         /**< Cached caller attributes.  */
  size_t            key_len;                      /**< Bytes valid in ``key``.    */
  uint8_t           key[k_ra_psa_max_key_bytes];  /**< Raw key material (sim).    */
#if !defined(RA_SIMULATOR_MODE)
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
static bool internal_handle_valid(const ra_psa_key_t h)
{
  if (h == NULL) {
    return false;
  }
  const struct ra_psa_key_handle* base = &s_key_pool[0];
  const struct ra_psa_key_handle* end  = &s_key_pool[k_ra_psa_max_keys];
  if ((h < base) || (h >= end)) {
    return false;
  }
  return h->in_use;
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

#if defined(RA_SIMULATOR_MODE)

/* ===========================================================================
 * Simulator-mode crypto primitives
 * ===========================================================================
 *
 * Real SHA-256 + tiny stand-ins for AES-GCM and ECDSA. These are
 * exercised by the host unit tests and never run on the target.
 */

/* -- SHA-256 (FIPS 180-4 reference) ---------------------------------------- */

/** @brief SHA-256 round constants K[0..63] (FIPS 180-4 Sec 4.2.2). */
static const uint32_t k_sha256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (32U - n));
}

/** @brief Process one 64-byte block into the hash state ``H``. */
static void sha256_block(uint32_t H[8], const uint8_t block[64])
{
  uint32_t W[64];
  for (uint32_t i = 0U; i < 16U; ++i) {
    W[i] = ((uint32_t)block[(i * 4U) + 0U] << 24) | ((uint32_t)block[(i * 4U) + 1U] << 16)
           | ((uint32_t)block[(i * 4U) + 2U] << 8) | ((uint32_t)block[(i * 4U) + 3U]);
  }
  for (uint32_t i = 16U; i < 64U; ++i) {
    const uint32_t s0 = sha256_rotr(W[i - 15U], 7U) ^ sha256_rotr(W[i - 15U], 18U) ^ (W[i - 15U] >> 3U);
    const uint32_t s1 = sha256_rotr(W[i - 2U], 17U) ^ sha256_rotr(W[i - 2U], 19U) ^ (W[i - 2U] >> 10U);
    W[i]              = W[i - 16U] + s0 + W[i - 7U] + s1;
  }
  uint32_t a = H[0];
  uint32_t b = H[1];
  uint32_t c = H[2];
  uint32_t d = H[3];
  uint32_t e = H[4];
  uint32_t f = H[5];
  uint32_t g = H[6];
  uint32_t h = H[7];
  for (uint32_t i = 0U; i < 64U; ++i) {
    const uint32_t S1    = sha256_rotr(e, 6U) ^ sha256_rotr(e, 11U) ^ sha256_rotr(e, 25U);
    const uint32_t ch    = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + k_sha256_K[i] + W[i];
    const uint32_t S0    = sha256_rotr(a, 2U) ^ sha256_rotr(a, 13U) ^ sha256_rotr(a, 22U);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
    h                    = g;
    g                    = f;
    f                    = e;
    e                    = d + temp1;
    d                    = c;
    c                    = b;
    b                    = a;
    a                    = temp1 + temp2;
  }
  H[0] += a;
  H[1] += b;
  H[2] += c;
  H[3] += d;
  H[4] += e;
  H[5] += f;
  H[6] += g;
  H[7] += h;
}

/** @brief One-shot SHA-256 over ``(in, in_len)`` -> 32-byte ``out``. */
static void sha256_oneshot(const uint8_t* in, size_t in_len, uint8_t out[32])
{
  uint32_t H[8] = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };

  /* Process complete 64-byte blocks. */
  size_t       remaining = in_len;
  const uint8_t* p       = in;
  while (remaining >= 64U) {
    sha256_block(H, p);
    p += 64U;
    remaining -= 64U;
  }

  /* Final block(s) with padding. */
  uint8_t tail[128] = {0};
  for (size_t i = 0U; i < remaining; ++i) {
    tail[i] = p[i];
  }
  tail[remaining]      = 0x80U;
  const size_t tail_blocks = (remaining < 56U) ? 1U : 2U;
  const uint64_t bitlen    = (uint64_t)in_len * 8U;
  const size_t   tail_len  = tail_blocks * 64U;
  for (uint32_t i = 0U; i < 8U; ++i) {
    tail[tail_len - 1U - i] = (uint8_t)(bitlen >> (i * 8U));
  }
  for (size_t b = 0U; b < tail_blocks; ++b) {
    sha256_block(H, &tail[b * 64U]);
  }

  for (uint32_t i = 0U; i < 8U; ++i) {
    out[(i * 4U) + 0U] = (uint8_t)(H[i] >> 24);
    out[(i * 4U) + 1U] = (uint8_t)(H[i] >> 16);
    out[(i * 4U) + 2U] = (uint8_t)(H[i] >> 8);
    out[(i * 4U) + 3U] = (uint8_t)(H[i]);
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
static void sim_aead_tag(const uint8_t* key, size_t key_len, const uint8_t* nonce, size_t nonce_len, const uint8_t* aad,
                         size_t aad_len, const uint8_t* cipher, size_t cipher_len, uint8_t out_tag[16])
{
  uint8_t buf[256];
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
  uint8_t digest[32];
  sha256_oneshot(buf, off, digest);
  for (uint32_t i = 0U; i < 16U; ++i) {
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
static void sim_keystream(const uint8_t* key, size_t key_len, const uint8_t* nonce, size_t nonce_len, uint8_t* dst,
                          size_t len)
{
  uint8_t seed[256];
  size_t  off = 0U;
  for (size_t i = 0U; (i < key_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = key[i];
  }
  for (size_t i = 0U; (i < nonce_len) && (off < sizeof(seed)); ++i) {
    seed[off++] = nonce[i];
  }
  uint8_t  block[32];
  uint32_t counter = 0U;
  size_t   produced = 0U;
  while (produced < len) {
    seed[off]      = (uint8_t)(counter >> 24);
    seed[off + 1U] = (uint8_t)(counter >> 16);
    seed[off + 2U] = (uint8_t)(counter >> 8);
    seed[off + 3U] = (uint8_t)(counter);
    sha256_oneshot(seed, off + 4U, block);
    const size_t take = ((len - produced) < 32U) ? (len - produced) : 32U;
    for (size_t i = 0U; i < take; ++i) {
      dst[produced + i] = block[i];
    }
    produced += take;
    ++counter;
  }
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

#if !defined(RA_SIMULATOR_MODE)
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
#if !defined(RA_SIMULATOR_MODE)
      (void)psa_destroy_key(s_key_pool[i].psa_id);
#endif
      (void)memset(s_key_pool[i].key, 0, sizeof(s_key_pool[i].key));
      s_key_pool[i].in_use  = false;
      s_key_pool[i].key_len = 0U;
    }
  }

#if !defined(RA_SIMULATOR_MODE)
  mbedtls_psa_crypto_free();
#endif

  s_initialised = false;
  return k_ra_ok;
}

ra_err_t ra_psa_key_import(ra_psa_key_t* out_handle, const ra_psa_key_attr_t* attr, const uint8_t* data, size_t data_len)
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

#if !defined(RA_SIMULATOR_MODE)
  psa_key_attributes_t pa = psa_key_attributes_init();
  /* Map the abridged enum back to PSA constants. Only the algorithms we
   * actually exercise are wired up; anything else falls through to the
   * not-supported branch below. */
  psa_algorithm_t       psa_alg     = 0;
  psa_key_type_t        psa_type    = 0;
  psa_key_usage_t       psa_usage   = 0;
  switch (attr->type) {
    case k_ra_psa_key_type_aes:
      psa_type = PSA_KEY_TYPE_AES;
      break;
    case k_ra_psa_key_type_hmac:
      psa_type = PSA_KEY_TYPE_HMAC;
      break;
    case k_ra_psa_key_type_ecc_p256_priv:
      psa_type = PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
      break;
    case k_ra_psa_key_type_ecc_p256_pub:
      psa_type = PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);
      break;
    case k_ra_psa_key_type_raw:
    default:
      psa_type = PSA_KEY_TYPE_RAW_DATA;
      break;
  }
  switch (attr->alg) {
    case k_ra_psa_alg_aes_gcm:
      psa_alg = PSA_ALG_GCM;
      break;
    case k_ra_psa_alg_ecdsa_sha_256:
      psa_alg = PSA_ALG_ECDSA(PSA_ALG_SHA_256);
      break;
    case k_ra_psa_alg_sha_256:
      psa_alg = PSA_ALG_SHA_256;
      break;
    case k_ra_psa_alg_none:
    default:
      psa_alg = 0;
      break;
  }
  if ((attr->usage & k_ra_psa_usage_sign) != 0U) {
    psa_usage |= PSA_KEY_USAGE_SIGN_HASH;
  }
  if ((attr->usage & k_ra_psa_usage_verify) != 0U) {
    psa_usage |= PSA_KEY_USAGE_VERIFY_HASH;
  }
  if ((attr->usage & k_ra_psa_usage_encrypt) != 0U) {
    psa_usage |= PSA_KEY_USAGE_ENCRYPT;
  }
  if ((attr->usage & k_ra_psa_usage_decrypt) != 0U) {
    psa_usage |= PSA_KEY_USAGE_DECRYPT;
  }
  if ((attr->usage & k_ra_psa_usage_derive) != 0U) {
    psa_usage |= PSA_KEY_USAGE_DERIVE;
  }
  psa_set_key_type(&pa, psa_type);
  psa_set_key_algorithm(&pa, psa_alg);
  psa_set_key_usage_flags(&pa, psa_usage);

  psa_key_id_t      kid = 0;
  const psa_status_t st = psa_import_key(&pa, data, data_len, &kid);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  slot->psa_id = kid;
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

#if !defined(RA_SIMULATOR_MODE)
  (void)psa_destroy_key(handle->psa_id);
#endif
  (void)memset(handle->key, 0, sizeof(handle->key));
  handle->key_len = 0U;
  handle->in_use  = false;
  return k_ra_ok;
}

ra_err_t ra_psa_hash_compute(ra_psa_alg_t alg, const uint8_t* input, size_t input_len, uint8_t* out, size_t out_cap,
                             size_t* out_len)
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

#if defined(RA_SIMULATOR_MODE)
  sha256_oneshot(input, input_len, out);
#else
  size_t             produced = 0U;
  const psa_status_t st       = psa_hash_compute(PSA_ALG_SHA_256, input, input_len, out, out_cap, &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  (void)produced;
#endif

  *out_len = (size_t)k_ra_psa_sha256_len;
  return k_ra_ok;
}

ra_err_t ra_psa_sign_hash(ra_psa_key_t handle, ra_psa_alg_t alg, const uint8_t* hash, size_t hash_len, uint8_t* sig,
                          size_t sig_cap, size_t* sig_len)
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

#if defined(RA_SIMULATOR_MODE)
  /* Sim "signature" = SHA-256(key || hash). 32 bytes. */
  uint8_t buf[k_ra_psa_max_key_bytes + k_ra_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  sha256_oneshot(buf, handle->key_len + hash_len, digest);
  (void)memcpy(sig, digest, sizeof(digest));
  *sig_len = sizeof(digest);
#else
  size_t             produced = 0U;
  const psa_status_t st = psa_sign_hash(handle->psa_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len, sig, sig_cap,
                                        &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  *sig_len = produced;
#endif

  return k_ra_ok;
}

ra_err_t ra_psa_verify_hash(ra_psa_key_t handle, ra_psa_alg_t alg, const uint8_t* hash, size_t hash_len,
                            const uint8_t* sig, size_t sig_len)
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

#if defined(RA_SIMULATOR_MODE)
  if (sig_len != (size_t)k_ra_psa_sha256_len) {
    return k_ra_err_crc_mismatch;
  }
  uint8_t buf[k_ra_psa_max_key_bytes + k_ra_psa_sha256_len];
  (void)memcpy(buf, handle->key, handle->key_len);
  (void)memcpy(&buf[handle->key_len], hash, hash_len);
  uint8_t digest[32];
  sha256_oneshot(buf, handle->key_len + hash_len, digest);
  /* Constant-time compare. */
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_sha256_len; ++i) {
    diff |= (uint8_t)(digest[i] ^ sig[i]);
  }
  return (diff == 0U) ? k_ra_ok : k_ra_err_crc_mismatch;
#else
  const psa_status_t st = psa_verify_hash(handle->psa_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len, sig, sig_len);
  if (st == PSA_SUCCESS) {
    return k_ra_ok;
  }
  if (st == PSA_ERROR_INVALID_SIGNATURE) {
    return k_ra_err_crc_mismatch;
  }
  return k_ra_err_hw_error;
#endif
}

ra_err_t ra_psa_aead_encrypt(ra_psa_key_t handle, ra_psa_alg_t alg, const uint8_t* nonce, size_t nonce_len,
                             const uint8_t* aad, size_t aad_len, const uint8_t* plain, size_t plain_len, uint8_t* out,
                             size_t out_cap, size_t* out_len)
{
  if (out_len != NULL) {
    *out_len = 0U;
  }
  if ((nonce == NULL) || (out == NULL) || (out_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if ((plain == NULL) && (plain_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if ((aad == NULL) && (aad_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra_err_invalid_arg;
  }
  if (alg != k_ra_psa_alg_aes_gcm) {
    return k_ra_err_invalid_arg;
  }
  if ((handle->attr.usage & k_ra_psa_usage_encrypt) == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (nonce_len != (size_t)k_ra_psa_gcm_nonce_len) {
    return k_ra_err_invalid_size;
  }
  if (out_cap < (plain_len + (size_t)k_ra_psa_gcm_tag_len)) {
    return k_ra_err_invalid_size;
  }

#if defined(RA_SIMULATOR_MODE)
  /* Encrypt = plaintext XOR keystream(key, nonce). Tag = SHA-256-trunc16. */
  if (plain_len > 0U) {
    uint8_t ks[256];
    if (plain_len > sizeof(ks)) {
      return k_ra_err_invalid_size;
    }
    sim_keystream(handle->key, handle->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(plain[i] ^ ks[i]);
    }
  }
  sim_aead_tag(handle->key, handle->key_len, nonce, nonce_len, aad, aad_len, out, plain_len, &out[plain_len]);
  *out_len = plain_len + (size_t)k_ra_psa_gcm_tag_len;
#else
  size_t             produced = 0U;
  const psa_status_t st = psa_aead_encrypt(handle->psa_id, PSA_ALG_GCM, nonce, nonce_len, aad, aad_len, plain,
                                           plain_len, out, out_cap, &produced);
  if (st != PSA_SUCCESS) {
    return k_ra_err_hw_error;
  }
  *out_len = produced;
#endif

  return k_ra_ok;
}

ra_err_t ra_psa_aead_decrypt(ra_psa_key_t handle, ra_psa_alg_t alg, const uint8_t* nonce, size_t nonce_len,
                             const uint8_t* aad, size_t aad_len, const uint8_t* cipher, size_t cipher_len,
                             uint8_t* out, size_t out_cap, size_t* out_len)
{
  if (out_len != NULL) {
    *out_len = 0U;
  }
  if ((nonce == NULL) || (cipher == NULL) || (out_len == NULL)) {
    return k_ra_err_invalid_arg;
  }
  if ((aad == NULL) && (aad_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(handle)) {
    return k_ra_err_invalid_arg;
  }
  if (alg != k_ra_psa_alg_aes_gcm) {
    return k_ra_err_invalid_arg;
  }
  if ((handle->attr.usage & k_ra_psa_usage_decrypt) == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (nonce_len != (size_t)k_ra_psa_gcm_nonce_len) {
    return k_ra_err_invalid_size;
  }
  if (cipher_len < (size_t)k_ra_psa_gcm_tag_len) {
    return k_ra_err_invalid_size;
  }

  const size_t plain_len = cipher_len - (size_t)k_ra_psa_gcm_tag_len;
  if ((out == NULL) && (plain_len != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (out_cap < plain_len) {
    return k_ra_err_invalid_size;
  }

#if defined(RA_SIMULATOR_MODE)
  uint8_t expected_tag[16];
  sim_aead_tag(handle->key, handle->key_len, nonce, nonce_len, aad, aad_len, cipher, plain_len, expected_tag);
  uint8_t diff = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_psa_gcm_tag_len; ++i) {
    diff |= (uint8_t)(expected_tag[i] ^ cipher[plain_len + i]);
  }
  if (diff != 0U) {
    return k_ra_err_crc_mismatch;
  }
  if (plain_len > 0U) {
    uint8_t ks[256];
    if (plain_len > sizeof(ks)) {
      return k_ra_err_invalid_size;
    }
    sim_keystream(handle->key, handle->key_len, nonce, nonce_len, ks, plain_len);
    for (size_t i = 0U; i < plain_len; ++i) {
      out[i] = (uint8_t)(cipher[i] ^ ks[i]);
    }
  }
  *out_len = plain_len;
#else
  size_t             produced = 0U;
  const psa_status_t st = psa_aead_decrypt(handle->psa_id, PSA_ALG_GCM, nonce, nonce_len, aad, aad_len, cipher,
                                           cipher_len, out, out_cap, &produced);
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
