/**
 * @file ra_sce_key_injection.c
 * @brief SCE key-injection HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API in
 * ``libs/ra_hal/inc/ra_sce_key_injection.h``. Wrapped key blobs are
 * laid out as ``[type | mgmt-info | payload | mac]`` and the MAC is
 * computed with the same xorshift mixer the rest of ``ra_sce`` uses
 * so the round-trip property is preserved across the
 * inject -> protected-op pipeline.
 *
 * @warning Stub backend; NOT cryptographically secure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sce_key_injection.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_sce.h"

// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)

/**
 * @var s_tag
 * @brief Logger tag for this TU.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "SCE_KI";

/**
 * @enum ra_sce_ki_internal_t
 * @brief Internal sizing constants reused inside the wrapper.
 *
 * @details
 * The mixer state is seeded with the same golden-ratio constant used
 * elsewhere in ``ra_sce``. ``k_..._byte_mask`` is the standard 8-bit
 * mask, named so the no-magic-numbers rule is satisfied.
 *
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_ra_sce_ki_seed      = 0x9E3779B97F4A7C15ULL, /**< Mixer seed.       */
  k_ra_sce_ki_mul       = 0x2545F4914F6CDD1DULL, /**< xorshift64*.     */
  k_ra_sce_ki_xor_a     = 12ULL,                 /**< xorshift step a. */
  k_ra_sce_ki_xor_b     = 25ULL,                 /**< xorshift step b. */
  k_ra_sce_ki_xor_c     = 27ULL,                 /**< xorshift step c. */
  k_ra_sce_ki_byte_mask = 0xFFULL,               /**< Low-byte mask.   */
} ra_sce_ki_internal_t;

/**
 * @enum ra_sce_ki_layout_t
 * @brief Byte offsets inside a wrapped-key blob.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_ki_off_type      = 0U,  /**< Type tag (4 bytes).        */
  k_ra_sce_ki_off_mgmt_info = 4U,  /**< Mgmt-info (16 bytes).      */
  k_ra_sce_ki_off_payload   = 20U, /**< Payload (variable).        */
} ra_sce_ki_layout_t;

/**
 * @brief One step of the xorshift64* mixer (mirrors ra_sce.c).
 *
 * @param[in] state Previous state.
 * @return Updated state.
 *
 * @pre ``state`` is non-zero.
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 */
static uint64_t ki_xorshift(uint64_t state)
{
  uint64_t x = state;
  x ^= x >> k_ra_sce_ki_xor_a;
  x ^= x << k_ra_sce_ki_xor_b;
  x ^= x >> k_ra_sce_ki_xor_c;
  return x * k_ra_sce_ki_mul;
}

/**
 * @brief Absorb one byte into a mixer state.
 *
 * @param[in] state Previous state.
 * @param[in] b     Byte to mix.
 * @return Updated state.
 *
 * @pre ``state`` is non-zero.
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 */
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
 */
static void ki_compute_mac(const uint8_t* buf, uint32_t len, uint8_t* mac_out)
{
  uint64_t state = k_ra_sce_ki_seed;
  for (uint32_t i = 0U; i < len; ++i) {
    state = ki_mix_byte(state, buf[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_wrapped_mac_bytes; ++i) {
    state      = ki_xorshift(state);
    mac_out[i] = (uint8_t)(state & k_ra_sce_ki_byte_mask);
  }
}

/**
 * @brief Write a 32-bit little-endian type tag at ``buf``.
 *
 * @param[out] buf  Destination buffer (>= 4 bytes).
 * @param[in]  tag  Tag value.
 *
 * @pre ``buf`` non-NULL.
 * @post First 4 bytes of ``buf`` reflect ``tag`` (LE).
 *
 * @since 0.1.0
 */
static void ki_write_type(uint8_t* buf, uint32_t tag)
{
  buf[0] = (uint8_t)((tag >> 0U) & 0xFFU);
  buf[1] = (uint8_t)((tag >> 8U) & 0xFFU);
  buf[2] = (uint8_t)((tag >> 16U) & 0xFFU);
  buf[3] = (uint8_t)((tag >> 24U) & 0xFFU);
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
 */
static uint32_t ki_read_type(const uint8_t* buf)
{
  uint32_t v = 0U;
  v |= (uint32_t)buf[0] << 0U;
  v |= (uint32_t)buf[1] << 8U;
  v |= (uint32_t)buf[2] << 16U;
  v |= (uint32_t)buf[3] << 24U;
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
 */
static void ki_fill_mgmt_info(uint8_t* mgmt, const uint8_t* raw, uint32_t len)
{
  uint64_t state = k_ra_sce_ki_seed ^ (uint64_t)len;
  for (uint32_t i = 0U; i < len; ++i) {
    state = ki_mix_byte(state, raw[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_wrapped_mgmt_info_bytes; ++i) {
    state   = ki_xorshift(state);
    mgmt[i] = (uint8_t)(state & k_ra_sce_ki_byte_mask);
  }
}

/**
 * @brief Map an AES key-bits selector to its byte count.
 *
 * @param[in]  key_bits Selector.
 * @param[out] out      Receives the byte count.
 * @return true on supported width.
 *
 * @since 0.1.0
 */
static bool ki_aes_bytes(ra_sce_aes_key_bits_t key_bits, uint32_t* out)
{
  bool ok = true;
  switch (key_bits) {
    case k_ra_sce_aes_key_bits_128:
      *out = 16U;
      break;
    case k_ra_sce_aes_key_bits_192:
      *out = 24U;
      break;
    case k_ra_sce_aes_key_bits_256:
      *out = 32U;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Map an RSA key-bits selector to its modulus byte count.
 *
 * @since 0.1.0
 */
static bool ki_rsa_bytes(ra_sce_rsa_key_bits_t key_bits, uint32_t* out)
{
  bool ok = true;
  switch (key_bits) {
    case k_ra_sce_rsa_key_bits_2048:
      *out = 256U;
      break;
    case k_ra_sce_rsa_key_bits_3072:
      *out = 384U;
      break;
    case k_ra_sce_rsa_key_bits_4096:
      *out = 512U;
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
 * @since 0.1.0
 */
static bool ki_ecc_bytes(ra_sce_ecc_curve_t curve, uint32_t* priv, uint32_t* pub)
{
  bool ok = true;
  switch (curve) {
    case k_ra_sce_ecc_curve_p256:
      *priv = 32U;
      *pub  = 64U;
      break;
    case k_ra_sce_ecc_curve_p384:
      *priv = 48U;
      *pub  = 96U;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Common wrapper layout: type + mgmt-info + payload + MAC.
 *
 * @param[out] dst         Destination buffer (>= max_total).
 * @param[in]  type        Type tag.
 * @param[in]  payload     Raw payload.
 * @param[in]  payload_len Length of ``payload``.
 *
 * @pre ``dst``, ``payload`` non-NULL.
 * @post First ``k_ra_sce_wrapped_max_total`` bytes of ``dst`` populated.
 *
 * @since 0.1.0
 */
static void ki_pack(uint8_t* dst, uint32_t type, const uint8_t* payload, uint32_t payload_len)
{
  /* Zero the entire blob so unused slack is deterministic. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_wrapped_max_total; ++i) {
    dst[i] = 0U;
  }
  ki_write_type(dst + (uint32_t)k_ra_sce_ki_off_type, type);
  ki_fill_mgmt_info(dst + (uint32_t)k_ra_sce_ki_off_mgmt_info, payload, payload_len);
  for (uint32_t i = 0U; i < payload_len; ++i) {
    dst[(uint32_t)k_ra_sce_ki_off_payload + i] = payload[i];
  }
  const uint32_t mac_off =
    (uint32_t)k_ra_sce_wrapped_max_total - (uint32_t)k_ra_sce_wrapped_mac_bytes;
  ki_compute_mac(dst, mac_off, dst + mac_off);
}

ra_err_t ra_sce_key_inject_aes(uint8_t*              installed_key_buf,
                               const uint8_t*        raw_key,
                               ra_sce_aes_key_bits_t key_bits)
{
  RA_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_aes: installed_key_buf");
  RA_CHECK_NULL_PTR(raw_key, s_tag, "inject_aes: raw_key");

  uint32_t key_len = 0U;
  if (!ki_aes_bytes(key_bits, &key_len)) {
    return k_ra_err_invalid_arg;
  }
  ki_pack(installed_key_buf, (uint32_t)k_ra_sce_wrapped_type_aes, raw_key, key_len);
  return k_ra_ok;
}

ra_err_t ra_sce_key_inject_rsa(uint8_t*              installed_key_buf,
                               const uint8_t*        raw_modulus,
                               const uint8_t*        raw_exponent,
                               ra_sce_rsa_key_bits_t key_bits)
{
  RA_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_rsa: installed_key_buf");
  RA_CHECK_NULL_PTR(raw_modulus, s_tag, "inject_rsa: raw_modulus");
  RA_CHECK_NULL_PTR(raw_exponent, s_tag, "inject_rsa: raw_exponent");

  uint32_t mod_bytes = 0U;
  if (!ki_rsa_bytes(key_bits, &mod_bytes)) {
    return k_ra_err_invalid_arg;
  }

  /* Layout: modulus first, then 4-byte exponent. */
  uint8_t        payload[k_ra_sce_wrapped_max_payload] = {};
  const uint32_t exp_bytes                             = 4U;
  if ((mod_bytes + exp_bytes) > (uint32_t)k_ra_sce_wrapped_max_payload) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < mod_bytes; ++i) {
    payload[i] = raw_modulus[i];
  }
  for (uint32_t i = 0U; i < exp_bytes; ++i) {
    payload[mod_bytes + i] = raw_exponent[i];
  }
  /* Public blob if exponent fits into 4 bytes; private blob otherwise.
   * The stub flags both via the same code path so the type tag is
   * the only differentiator. */
  ki_pack(installed_key_buf,
          (uint32_t)k_ra_sce_wrapped_type_rsa_pub,
          payload,
          mod_bytes + exp_bytes);
  return k_ra_ok;
}

ra_err_t ra_sce_key_inject_ecc(uint8_t*           installed_key_buf,
                               ra_sce_ecc_curve_t curve,
                               const uint8_t*     raw_priv_or_pub,
                               bool               is_private)
{
  RA_CHECK_NULL_PTR(installed_key_buf, s_tag, "inject_ecc: installed_key_buf");
  RA_CHECK_NULL_PTR(raw_priv_or_pub, s_tag, "inject_ecc: raw_priv_or_pub");

  uint32_t priv = 0U;
  uint32_t pub  = 0U;
  if (!ki_ecc_bytes(curve, &priv, &pub)) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t len = is_private ? priv : pub;
  const uint32_t tag =
    is_private ? (uint32_t)k_ra_sce_wrapped_type_ecc_priv : (uint32_t)k_ra_sce_wrapped_type_ecc_pub;
  ki_pack(installed_key_buf, tag, raw_priv_or_pub, len);
  return k_ra_ok;
}

ra_err_t ra_sce_key_validate(const uint8_t*            installed_key_buf,
                             ra_sce_wrapped_key_type_t expected_type)
{
  RA_CHECK_NULL_PTR(installed_key_buf, s_tag, "key_validate: installed_key_buf");

  const uint32_t tag = ki_read_type(installed_key_buf);
  if (tag != (uint32_t)expected_type) {
    return k_ra_err_invalid_arg;
  }
  uint8_t        mac[k_ra_sce_wrapped_mac_bytes] = {};
  const uint32_t mac_off =
    (uint32_t)k_ra_sce_wrapped_max_total - (uint32_t)k_ra_sce_wrapped_mac_bytes;
  ki_compute_mac(installed_key_buf, mac_off, mac);
  bool match = true;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sce_wrapped_mac_bytes; ++i) {
    if (installed_key_buf[mac_off + i] != mac[i]) {
      match = false;
      break;
    }
  }
  return match ? k_ra_ok : k_ra_err_hw_error;
}

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)
