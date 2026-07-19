/**
 * @file ra8_rsip_key_injection.c
 * @brief RSIP key-injection HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API in
 * ``libs/ra8_hal/inc/ra8_rsip_key_injection.h``. Wrapped key blobs are
 * laid out as ``[type | mgmt-info | payload | mac]`` and the MAC is
 * computed with the same xorshift mixer ``ra8_sce_key_injection`` uses
 * so the round-trip property is preserved across the
 * inject -> protected-op pipeline. Round-tripping under unit tests
 * is the only guarantee made; the wrapping is not cryptographically
 * meaningful and a real backend will replace this entirely.
 *
 * @warning Stub backend; NOT cryptographically secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rsip_key_injection.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_secure.h"

/**
 * @var s_tag
 * @brief Logger tag for this TU.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP_KI";

/*
 * Fail-closed stub-crypto gate (issue #180). The key-wrap and MAC below use a
 * NON-cryptographic xorshift mixer (see the @warning in the file banner), so
 * the wrapping is not cryptographically meaningful. It is only safe under host
 * simulation or an explicitly-declared insecure dev/eval image. A real
 * production/HIL image (neither flag set) compiles the #else branch, where
 * every entry point hard-errors so keys are never wrapped or validated with the
 * stub. scripts/utils/check_stub_crypto_guarded.py enforces the guard.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)

/** @brief AES / RSA / ECC key element byte counts. */
typedef enum : uint16_t {
  k_aes_192_key_bytes  = 24U,  /**< AES 192 key bytes.  */
  k_rsa_1024_mod_bytes = 128U, /**< RSA 1024 mod bytes. */
  k_rsa_2048_mod_bytes = 256U, /**< RSA 2048 mod bytes. */
  k_rsa_3072_mod_bytes = 384U, /**< RSA 3072 mod bytes. */
  k_rsa_4096_mod_bytes = 512U, /**< RSA 4096 mod bytes. */
  k_ecc_256_pub_bytes  = 64U,  /**< ECC 256 pub bytes.  */
  k_ecc_384_priv_bytes = 48U,  /**< ECC 384 priv bytes. */
  k_ecc_384_pub_bytes  = 96U,  /**< ECC 384 pub bytes.  */
  k_ecc_521_priv_bytes = 66U,  /**< ECC 521 priv bytes. */
  k_ecc_521_pub_bytes  = 132U, /**< ECC 521 pub bytes.  */
} rsip_ki_size_t;

/**
 * @enum ra8_rsip_ki_internal_t
 * @brief Internal sizing constants reused inside the wrapper.
 *
 * @details
 * The mixer state is seeded with the same golden-ratio constant used
 * elsewhere in the secure HALs. ``k_..._byte_mask`` is the standard
 * 8-bit mask, named so the no-magic-numbers rule is satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_ra8_rsip_ki_seed      = 0x9E3779B97F4A7C15ULL, /**< Mixer seed.      */
  k_ra8_rsip_ki_mul       = 0x2545F4914F6CDD1DULL, /**< xorshift64*.     */
  k_ra8_rsip_ki_xor_a     = 12ULL,                 /**< xorshift step a. */
  k_ra8_rsip_ki_xor_b     = 25ULL,                 /**< xorshift step b. */
  k_ra8_rsip_ki_xor_c     = 27ULL,                 /**< xorshift step c. */
  k_ra8_rsip_ki_byte_mask = 0xFFULL,               /**< Low-byte mask.   */
} ra8_rsip_ki_internal_t;

/**
 * @enum ra8_rsip_ki_layout_t
 * @brief Byte offsets inside a wrapped-key blob.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_ki_off_type      = 0U,  /**< Type tag (4 bytes).   */
  k_ra8_rsip_ki_off_mgmt_info = 4U,  /**< Mgmt-info (16 bytes). */
  k_ra8_rsip_ki_off_payload   = 20U, /**< Payload (variable).   */
} ra8_rsip_ki_layout_t;

/**
 * @enum ra8_rsip_ki_const_t
 * @brief Numeric constants used by the byte-shuffle helpers.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rsip_ki_byte_bits   = 8U,    /**< Bits per byte.                  */
  k_ra8_rsip_ki_word_byte0  = 0U,    /**< LSB index in a 32-bit word.     */
  k_ra8_rsip_ki_word_byte1  = 1U,    /**< Byte 1 of a 32-bit word.        */
  k_ra8_rsip_ki_word_byte2  = 2U,    /**< Byte 2 of a 32-bit word.        */
  k_ra8_rsip_ki_word_byte3  = 3U,    /**< MSB index in a 32-bit word.     */
  k_ra8_rsip_ki_shift_8     = 8U,    /**< 8-bit shift.                    */
  k_ra8_rsip_ki_shift_16    = 16U,   /**< 16-bit shift.                   */
  k_ra8_rsip_ki_shift_24    = 24U,   /**< 24-bit shift.                   */
  k_ra8_rsip_ki_byte_low    = 0xFFU, /**< Low-byte mask (32-bit form).    */
  k_ra8_rsip_ki_rsa_e_bytes = 4U,    /**< RSA exponent byte count (stub). */
} ra8_rsip_ki_const_t;

/**
 * @brief One step of the xorshift64* mixer.
 *
 * @details
 * Mirrors the mixer used in ``ra8_sce_key_injection.c`` so the secure
 * HALs share a deterministic check-pattern across blobs.
 *
 * @param[in] state Previous state; must be non-zero.
 * @return Updated state.
 *
 * @pre ``state != 0``.
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static uint64_t ki_xorshift(uint64_t state)
{
  uint64_t x = state;
  x ^= x >> k_ra8_rsip_ki_xor_a;
  x ^= x << k_ra8_rsip_ki_xor_b;
  x ^= x >> k_ra8_rsip_ki_xor_c;
  return x * k_ra8_rsip_ki_mul;
}

/**
 * @brief Absorb one byte into a mixer state.
 *
 * @param[in] state Previous state; must be non-zero.
 * @param[in] b     Byte to mix.
 * @return Updated state.
 *
 * @pre ``state != 0``.
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static uint64_t ki_mix_byte(uint64_t state, uint8_t b)
{
  return ki_xorshift(state ^ (uint64_t)b);
}

/**
 * @brief Compute a 16-byte MAC over ``buf[0..len-1]`` and write it to
 *        ``mac_out``.
 *
 * @param[in]  buf     Input buffer.
 * @param[in]  len     Length of ``buf``.
 * @param[out] mac_out 16-byte MAC destination.
 *
 * @pre ``buf`` and ``mac_out`` non-NULL.
 * @post ``mac_out[0..15]`` is populated.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static void ki_compute_mac(const uint8_t* buf, uint32_t len, uint8_t* mac_out)
{
  uint64_t state = k_ra8_rsip_ki_seed;
  for (uint32_t i = 0U; i < len; ++i) {
    state = ki_mix_byte(state, buf[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rsip_wrapped_mac_bytes; ++i) {
    state      = ki_xorshift(state);
    mac_out[i] = (uint8_t)(state & k_ra8_rsip_ki_byte_mask);
  }
}

/**
 * @brief Write a 32-bit little-endian type tag at ``buf``.
 *
 * @param[out] buf Destination buffer (>= 4 bytes).
 * @param[in]  tag Tag value.
 *
 * @pre ``buf`` non-NULL.
 * @post First 4 bytes of ``buf`` reflect ``tag`` (LE).
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static void ki_write_type(uint8_t* buf, uint32_t tag)
{
  buf[k_ra8_rsip_ki_word_byte0] = (uint8_t)((tag >> 0U) & (uint32_t)k_ra8_rsip_ki_byte_low);
  buf[k_ra8_rsip_ki_word_byte1] =
    (uint8_t)((tag >> (uint32_t)k_ra8_rsip_ki_shift_8) & (uint32_t)k_ra8_rsip_ki_byte_low);
  buf[k_ra8_rsip_ki_word_byte2] =
    (uint8_t)((tag >> (uint32_t)k_ra8_rsip_ki_shift_16) & (uint32_t)k_ra8_rsip_ki_byte_low);
  buf[k_ra8_rsip_ki_word_byte3] =
    (uint8_t)((tag >> (uint32_t)k_ra8_rsip_ki_shift_24) & (uint32_t)k_ra8_rsip_ki_byte_low);
}

/**
 * @brief Read a 32-bit little-endian tag from ``buf``.
 *
 * @param[in] buf Source buffer (>= 4 bytes).
 * @return The decoded tag value.
 *
 * @pre ``buf`` non-NULL.
 * @post No state changes.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static uint32_t ki_read_type(const uint8_t* buf)
{
  uint32_t v = 0U;
  v |= (uint32_t)buf[k_ra8_rsip_ki_word_byte0] << 0U;
  v |= (uint32_t)buf[k_ra8_rsip_ki_word_byte1] << (uint32_t)k_ra8_rsip_ki_shift_8;
  v |= (uint32_t)buf[k_ra8_rsip_ki_word_byte2] << (uint32_t)k_ra8_rsip_ki_shift_16;
  v |= (uint32_t)buf[k_ra8_rsip_ki_word_byte3] << (uint32_t)k_ra8_rsip_ki_shift_24;
  return v;
}

/**
 * @brief Populate the management-info envelope from a key fingerprint.
 *
 * @param[out] mgmt 16-byte mgmt-info buffer.
 * @param[in]  raw  Raw key bytes used to seed the fingerprint.
 * @param[in]  len  Length of ``raw``.
 *
 * @pre ``mgmt`` and ``raw`` non-NULL.
 * @post ``mgmt[0..15]`` is populated deterministically.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static void ki_fill_mgmt_info(uint8_t* mgmt, const uint8_t* raw, uint32_t len)
{
  uint64_t state = k_ra8_rsip_ki_seed ^ (uint64_t)len;
  for (uint32_t i = 0U; i < len; ++i) {
    state = ki_mix_byte(state, raw[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rsip_wrapped_mgmt_info_bytes; ++i) {
    state   = ki_xorshift(state);
    mgmt[i] = (uint8_t)(state & k_ra8_rsip_ki_byte_mask);
  }
}

/**
 * @brief Map an AES key-bits selector to its byte count.
 *
 * @param[in]  key_bits Selector.
 * @param[out] out      Receives the byte count.
 * @return ``true`` on supported width.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static bool ki_aes_bytes(ra8_rsip_aes_key_bits_t key_bits, uint32_t* out)
{
  bool ok = true;
  switch (key_bits) {
    case k_ra8_rsip_aes_key_bits_128:
      *out = 16U;
      break;
    case k_ra8_rsip_aes_key_bits_192:
      *out = k_aes_192_key_bytes;
      break;
    case k_ra8_rsip_aes_key_bits_256:
      *out = 32U;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Map an RSA size selector to its modulus byte count.
 *
 * @param[in]  size Selector.
 * @param[out] out  Receives the byte count.
 * @return ``true`` on supported width.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static bool ki_rsa_bytes(ra8_rsip_rsa_size_t size, uint32_t* out)
{
  bool ok = true;
  switch (size) {
    case k_ra8_rsip_rsa_1024:
      *out = k_rsa_1024_mod_bytes;
      break;
    case k_ra8_rsip_rsa_2048:
      *out = k_rsa_2048_mod_bytes;
      break;
    case k_ra8_rsip_rsa_3072:
      *out = k_rsa_3072_mod_bytes;
      break;
    case k_ra8_rsip_rsa_4096:
      *out = k_rsa_4096_mod_bytes;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Map an ECC curve selector to its (priv, pub) byte counts.
 *
 * @param[in]  curve Selector.
 * @param[out] priv  Receives the private-scalar byte count.
 * @param[out] pub   Receives the public-point (X || Y) byte count.
 * @return ``true`` on supported curve.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static bool ki_ecc_bytes(ra8_rsip_curve_t curve, uint32_t* priv, uint32_t* pub)
{
  bool ok = true;
  switch (curve) {
    case k_ra8_rsip_curve_secp256r1:
    case k_ra8_rsip_curve_brain256r1:
    case k_ra8_rsip_curve_secp256k1:
      *priv = 32U;
      *pub  = k_ecc_256_pub_bytes;
      break;
    case k_ra8_rsip_curve_secp384r1:
    case k_ra8_rsip_curve_brain384r1:
      *priv = k_ecc_384_priv_bytes;
      *pub  = k_ecc_384_pub_bytes;
      break;
    case k_ra8_rsip_curve_secp521r1:
    case k_ra8_rsip_curve_brain512r1:
      *priv = k_ecc_521_priv_bytes;
      *pub  = k_ecc_521_pub_bytes;
      break;
    case k_ra8_rsip_curve_ed25519:
      *priv = 32U;
      *pub  = 32U;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Common wrapper layout: ``[type | mgmt-info | payload | MAC]``.
 *
 * @param[out] dst         Destination buffer (>= max_total).
 * @param[in]  type        Type tag.
 * @param[in]  payload     Raw payload.
 * @param[in]  payload_len Length of ``payload``.
 *
 * @pre ``dst``, ``payload`` non-NULL.
 * @post First ``k_ra8_rsip_wrapped_max_total`` bytes of ``dst`` populated.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static void ki_pack(uint8_t* dst, uint32_t type, const uint8_t* payload, uint32_t payload_len)
{
  /* Zero the entire blob so unused slack is deterministic. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rsip_wrapped_max_total; ++i) {
    dst[i] = 0U;
  }
  ki_write_type(dst + (uint32_t)k_ra8_rsip_ki_off_type, type);
  ki_fill_mgmt_info(dst + (uint32_t)k_ra8_rsip_ki_off_mgmt_info, payload, payload_len);
  for (uint32_t i = 0U; i < payload_len; ++i) {
    dst[(uint32_t)k_ra8_rsip_ki_off_payload + i] = payload[i];
  }
  const uint32_t mac_off =
    (uint32_t)k_ra8_rsip_wrapped_max_total - (uint32_t)k_ra8_rsip_wrapped_mac_bytes;
  ki_compute_mac(dst, mac_off, dst + mac_off);
}

ra8_err_t ra8_rsip_key_inject_aes(uint8_t*                installed_key_buf,
                                  const uint8_t*          raw_key,
                                  ra8_rsip_aes_key_bits_t key_bits)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_aes: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_key, s_tag, "inject_aes: raw_key");

  uint32_t key_len = 0U;
  if (!ki_aes_bytes(key_bits, &key_len)) {
    return k_ra8_err_invalid_arg;
  }
  ki_pack(installed_key_buf, (uint32_t)k_ra8_rsip_wrapped_type_aes, raw_key, key_len);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_key_inject_rsa(uint8_t*            installed_key_buf,
                                  const uint8_t*      raw_modulus,
                                  const uint8_t*      raw_exponent,
                                  ra8_rsip_rsa_size_t size)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_rsa: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_modulus, s_tag, "inject_rsa: raw_modulus");
  RA8_CHECK_NULL_PTR(raw_exponent, s_tag, "inject_rsa: raw_exponent");

  uint32_t mod_bytes = 0U;
  if (!ki_rsa_bytes(size, &mod_bytes)) {
    return k_ra8_err_invalid_arg;
  }

  /* Layout: modulus first, then 4-byte exponent. */
  uint8_t        payload[k_ra8_rsip_wrapped_max_payload] = {};
  const uint32_t exp_bytes                               = (uint32_t)k_ra8_rsip_ki_rsa_e_bytes;
  if ((mod_bytes + exp_bytes) > (uint32_t)k_ra8_rsip_wrapped_max_payload) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < mod_bytes; ++i) {
    payload[i] = raw_modulus[i];
  }
  for (uint32_t i = 0U; i < exp_bytes; ++i) {
    payload[mod_bytes + i] = raw_exponent[i];
  }
  /* Public blob if the wrapper holds a 4-byte e; the stub uses the
   * public type tag for both because there is no engine to enforce
   * the discrimination here -- the protected layer accepts either. */
  ki_pack(installed_key_buf,
          (uint32_t)k_ra8_rsip_wrapped_type_rsa_pub,
          payload,
          mod_bytes + exp_bytes);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_key_inject_ecc(uint8_t*         installed_key_buf,
                                  ra8_rsip_curve_t curve,
                                  const uint8_t*   raw_priv_or_pub,
                                  bool             is_private)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_ecc: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_priv_or_pub, s_tag, "inject_ecc: raw_priv_or_pub");

  uint32_t priv = 0U;
  uint32_t pub  = 0U;
  if (!ki_ecc_bytes(curve, &priv, &pub)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t len = is_private ? priv : pub;
  const uint32_t tag = is_private ? (uint32_t)k_ra8_rsip_wrapped_type_ecc_priv
                                  : (uint32_t)k_ra8_rsip_wrapped_type_ecc_pub;
  ki_pack(installed_key_buf, tag, raw_priv_or_pub, len);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_key_validate(const uint8_t*              installed_key_buf,
                                ra8_rsip_wrapped_key_type_t expected_type)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "key_validate: installed_key_buf");

  const uint32_t tag = ki_read_type(installed_key_buf);
  if (tag != (uint32_t)expected_type) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t        mac[k_ra8_rsip_wrapped_mac_bytes] = {};
  const uint32_t mac_off =
    (uint32_t)k_ra8_rsip_wrapped_max_total - (uint32_t)k_ra8_rsip_wrapped_mac_bytes;
  ki_compute_mac(installed_key_buf, mac_off, mac);
  /* Constant-time MAC compare: an early-out byte loop would leak, through its
   * timing, how many leading MAC bytes an attacker's forged wrapped key got
   * right -- a byte-at-a-time forgery oracle (T5-12). */
  const bool match =
    ra8_ct_equal(&installed_key_buf[mac_off], mac, (size_t)k_ra8_rsip_wrapped_mac_bytes);
  return match ? k_ra8_ok : k_ra8_err_hw_error;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_SIMULATOR_MODE */

/*
 * Fail-closed production variant. Without a real RSIP/SCE key-injection backend
 * the xorshift key-wrap above must never be used, so every entry point returns
 * a hard error (never k_ra8_ok). A production image that forgot to provide the
 * real backend therefore cannot wrap or validate keys with the insecure stub.
 */

ra8_err_t ra8_rsip_key_inject_aes(uint8_t*                installed_key_buf,
                                  const uint8_t*          raw_key,
                                  ra8_rsip_aes_key_bits_t key_bits)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_aes: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_key, s_tag, "inject_aes: raw_key");
  (void)key_bits;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_key_inject_rsa(uint8_t*            installed_key_buf,
                                  const uint8_t*      raw_modulus,
                                  const uint8_t*      raw_exponent,
                                  ra8_rsip_rsa_size_t size)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_rsa: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_modulus, s_tag, "inject_rsa: raw_modulus");
  RA8_CHECK_NULL_PTR(raw_exponent, s_tag, "inject_rsa: raw_exponent");
  (void)size;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_key_inject_ecc(uint8_t*         installed_key_buf,
                                  ra8_rsip_curve_t curve,
                                  const uint8_t*   raw_priv_or_pub,
                                  bool             is_private)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_ecc: installed_key_buf");
  RA8_CHECK_NULL_PTR(raw_priv_or_pub, s_tag, "inject_ecc: raw_priv_or_pub");
  (void)curve;
  (void)is_private;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_key_validate(const uint8_t*              installed_key_buf,
                                ra8_rsip_wrapped_key_type_t expected_type)
{
  RA8_CHECK_NULL_PTR(installed_key_buf, s_tag, "key_validate: installed_key_buf");
  (void)expected_type;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_SIMULATOR_MODE */
