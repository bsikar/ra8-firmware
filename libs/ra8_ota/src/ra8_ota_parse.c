/**
 * @file ra8_ota_parse.c
 * @brief Phase-5 OTA configuration + manifest parsing helpers.
 *
 * @details
 * Companion translation unit to ``ra8_ota.c``. Holds the pure,
 * state-free parsing and validation surface so the orchestration TU
 * stays under the per-file line budget:
 *
 *   - The two MC/DC predicates (``ra8_ota_internal_char_in_range`` and
 *     ``ra8_ota_internal_download_state_invalid``).
 *   - The JSON / hex scanning helpers (``priv_json_str``,
 *     ``ra8_ota_internal_json_u32``, ``priv_hex_nibble``,
 *     ``priv_hex_decode``).
 *   - The manifest decoders (``priv_manifest_decode_crypto`` and
 *     ``priv_manifest_decode``).
 *   - The configuration validators rooted at
 *     ``ra8_ota_internal_validate_cfg``.
 *
 * Every function here is pure with respect to module state -- none of
 * them read or write the mutable statics owned by ``ra8_ota.c``. The
 * read-only log tag is duplicated locally (cheap, correct for an
 * immutable literal). No malloc anywhere (NASA Rule 3); every loop has
 * a static upper bound (NASA Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"

/** @brief Module log tag (private copy; immutable literal). */
static const char* const s_tag = "ra8_ota";

/**
 * @enum ra8_ota_internal_const_t
 * @brief Internal numeric constants used by JSON / hex helpers.
 */
typedef enum : uint32_t {
  k_ra8_ota_json_skip_max      = 8U,    /**< Max JSON whitespace/quote skip.     */
  k_ra8_ota_u32_decimal_digits = 12U,   /**< Max decimal digits in a uint32.     */
  k_ra8_ota_u32_decimal_base   = 10U,   /**< Base for decimal parsing.           */
  k_ra8_ota_hex_alpha_offset   = 10U,   /**< Offset added for 'a'..'f'/'A'..'F'. */
  k_ra8_ota_hex_invalid_nibble = 0xFFU, /**< Sentinel for invalid hex nibble.    */
  k_ra8_ota_hex_chars_per_byte = 2U,    /**< Two hex chars per encoded byte.     */
  k_ra8_ota_hex_nibble_shift   = 4U,    /**< Shift for high nibble in a byte.    */
  k_ra8_ota_hex_buf_bytes      = 257U,  /**< Capacity of stack hex buffer.       */
} ra8_ota_internal_const_t;

/**
 * @brief Pure char-in-range predicate -- see header for full contract.
 * @details Promoted helper so the line-455 AND can be driven under MC/DC.
 * @param[in] c  Character under test.
 * @param[in] lo Inclusive lower bound.
 * @param[in] hi Inclusive upper bound.
 * @return Boolean predicate.
 * @retval true  c is in [lo, hi].
 * @retval false Outside.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_ota_internal_char_in_range(char c, char lo, char hi)
{
  return (c >= lo) && (c <= hi);
}

/**
 * @brief Pure download-state-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-990 AND can be driven under MC/DC.
 * @param[in] state_idle_val        Numeric value of @c k_ra8_ota_state_idle.
 * @param[in] state_downloading_val Numeric value of @c k_ra8_ota_state_downloading.
 * @param[in] state                 Candidate state value.
 * @return Boolean reject predicate.
 * @retval true  Caller returns invalid-state.
 * @retval false State permits operation.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra8_ota_internal_download_state_invalid(uint32_t state_idle_val,
                                             uint32_t state_downloading_val,
                                             uint32_t state)
{
  return (state != state_idle_val) && (state != state_downloading_val);
}

/* =============================================================================
 * Configuration validation
 * ============================================================================= */

/**
 * @brief Validate the network function-pointer block of @p cfg.
 *
 * @details
 * Confirms ``cfg->net.open``, ``cfg->net.read`` and ``cfg->net.close``
 * are all non-NULL. Required for the OTA module to fetch manifests
 * and image chunks.
 *
 * @param[in] cfg Caller configuration (already verified non-NULL by
 *                ``ra8_ota_internal_validate_cfg``).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           All net function pointers set.
 * @retval k_ra8_err_null_ptr A required net function pointer is NULL.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialized.
 * @post Returns k_ra8_ok iff every net pointer is non-NULL.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_validate_cfg_net(const ra8_ota_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->net.open, s_tag, "net.open");
  RA8_CHECK_NULL_PTR(cfg->net.read, s_tag, "net.read");
  RA8_CHECK_NULL_PTR(cfg->net.close, s_tag, "net.close");
  return k_ra8_ok;
}

/**
 * @brief Validate the crypto function-pointer block of @p cfg.
 *
 * @details
 * Confirms the four crypto callbacks (sha256_init/update/final and
 * ecdsa_verify) are all wired up. Required to hash and authenticate
 * downloaded firmware images.
 *
 * @param[in] cfg Caller configuration (already verified non-NULL).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           All crypto function pointers set.
 * @retval k_ra8_err_null_ptr A required crypto function pointer is NULL.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialized.
 * @post Returns k_ra8_ok iff every crypto pointer is non-NULL.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_validate_cfg_crypto(const ra8_ota_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->crypto.sha256_init, s_tag, "crypto.sha256_init");
  RA8_CHECK_NULL_PTR(cfg->crypto.sha256_update, s_tag, "crypto.sha256_update");
  RA8_CHECK_NULL_PTR(cfg->crypto.sha256_final, s_tag, "crypto.sha256_final");
  RA8_CHECK_NULL_PTR(cfg->crypto.ecdsa_verify, s_tag, "crypto.ecdsa_verify");
  return k_ra8_ok;
}

/**
 * @brief Validate the flash function-pointer block of @p cfg.
 *
 * @details
 * Confirms erase/program/set_startup/readback callbacks are wired up
 * and the configured ``bank_size_bytes`` is non-zero and below the
 * firmware-wide cap.
 *
 * @param[in] cfg Caller configuration (already verified non-NULL).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              All flash callbacks set, bank size sane.
 * @retval k_ra8_err_null_ptr    A required flash callback is NULL.
 * @retval k_ra8_err_invalid_arg ``bank_size_bytes`` is zero or above the cap.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialized.
 * @post Returns k_ra8_ok iff all callbacks are present and the bank size is sane.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_validate_cfg_flash(const ra8_ota_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->flash.erase, s_tag, "flash.erase");
  RA8_CHECK_NULL_PTR(cfg->flash.program, s_tag, "flash.program");
  RA8_CHECK_NULL_PTR(cfg->flash.set_startup, s_tag, "flash.set_startup");
  RA8_CHECK_NULL_PTR(cfg->flash.readback, s_tag, "flash.readback");
  if (cfg->flash.bank_size_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->flash.bank_size_bytes > k_ra8_ota_max_image_bytes) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_ota_internal_validate_cfg(const ra8_ota_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  ra8_err_t e = priv_validate_cfg_net(cfg);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_validate_cfg_crypto(cfg);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_validate_cfg_flash(cfg);
  if (e != k_ra8_ok) {
    return e;
  }
  if (cfg->manifest_url[0] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * JSON / hex scanning helpers
 * ============================================================================= */

/**
 * @brief Locate ``"key"`` inside a JSON-ish buffer and copy its
 *        string value (assumes minimal, well-formed manifest).
 *
 * @details
 * Uses ``strstr`` to find ``key``, walks past the next ``"``, captures
 * everything up to the matching close-quote and copies it into ``dst``
 * with a trailing NUL. Not a general JSON parser -- the manifest
 * format is intentionally minimal.
 *
 * @param[in]  json  Source bytes (NUL-terminated).
 * @param[in]  key   Key name to look for, e.g. ``"\"version\""``.
 * @param[out] dst   Destination string buffer.
 * @param[in]  cap   Capacity of ``dst``.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               String value copied into ``dst``.
 * @retval k_ra8_err_invalid_arg  Key not found or quotes missing.
 * @retval k_ra8_err_invalid_size Value would not fit in ``dst``.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``dst`` is a NUL-terminated copy of the value.
 * @post On failure ``dst`` content is undefined.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_json_str(const char* json, const char* key, char* dst, uint32_t cap)
{
  const char* p = strstr(json, key);
  if (p == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  p = strchr(p + strlen(key), '"');
  if (p == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ++p;
  const char* q = strchr(p, '"');
  if (q == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t n = (uint32_t)(q - p);
  if (n + 1U > cap) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(dst, p, n);
  dst[n] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Parse a decimal ``"key": NNN`` field out of a JSON-ish buffer.
 *
 * @details
 * Locates ``key`` via ``strstr``, skips past colon/quote/whitespace
 * (bounded by ``k_ra8_ota_json_skip_max``) then accumulates a base-10
 * value out of up to ``k_ra8_ota_u32_decimal_digits`` digit characters.
 * Both inner loops are statically bounded (NASA Rule 2).
 *
 * @param[in]  json  Source JSON bytes (NUL-terminated).
 * @param[in]  key   Key string including its quotes, e.g. ``"\"size\""``.
 * @param[out] out_v Receives the parsed value on success.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok              Value parsed into ``*out_v``.
 * @retval k_ra8_err_invalid_arg Key not found or no digits after the colon.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``*out_v`` reflects the parsed unsigned value.
 * @post On failure ``*out_v`` is unchanged.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
ra8_err_t ra8_ota_internal_json_u32(const char* json, const char* key, uint32_t* out_v)
{
  const char* p = strstr(json, key);
  if (p == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  p += strlen(key);
  /* Skip past quote/colon/whitespace. */
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_ota_json_skip_max; ++guard) {
    if (*p == ':' || *p == ' ' || *p == '"') {
      ++p;
    } else {
      break;
    }
  }
  uint32_t v = 0U;
  uint32_t i = 0U;
  for (; i < (uint32_t)k_ra8_ota_u32_decimal_digits; ++i) {
    const char c = p[i];
    if ((c < '0') || (c > '9')) {
      break;
    }
    v = (v * (uint32_t)k_ra8_ota_u32_decimal_base) + (uint32_t)(c - '0');
  }
  if (i == 0U) {
    return k_ra8_err_invalid_arg;
  }
  *out_v = v;
  return k_ra8_ok;
}

/**
 * @brief Decode a single hex nibble. Returns 0xFFU on invalid input.
 *
 * @details Maps ``'0'..'9'`` to 0..9 and ``'a'..'f'`` / ``'A'..'F'`` to
 *   10..15 via ``k_ra8_ota_hex_alpha_offset``. Any other character returns
 *   ``k_ra8_ota_hex_invalid_nibble`` (0xFFU).
 *
 * @param[in] c Candidate hex character.
 *
 * @return Nibble value 0..15.
 * @retval k_ra8_ota_hex_invalid_nibble Character is not a hex digit.
 *
 * @pre None.
 * @post No state mutated.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL
static uint8_t priv_hex_nibble(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  if ((c >= 'a') && (c <= 'f')) {
    return (uint8_t)((uint8_t)k_ra8_ota_hex_alpha_offset + (c - 'a'));
  }
  if (ra8_ota_internal_char_in_range(c, 'A', 'F')) {
    return (uint8_t)((uint8_t)k_ra8_ota_hex_alpha_offset + (c - 'A'));
  }
  return (uint8_t)k_ra8_ota_hex_invalid_nibble;
}

/**
 * @brief Decode a hex string into bytes. Returns the number of bytes
 *        decoded, or 0 on a malformed input.
 *
 * @details Walks the input two characters at a time, calling
 *   ``priv_hex_nibble`` on each. Rejects odd-length input or any
 *   non-hex character by returning 0.
 *
 * @param[in]  in      NUL-terminated hex string.
 * @param[out] out     Destination byte buffer.
 * @param[in]  out_cap Capacity of ``out`` in bytes.
 *
 * @return Number of bytes written into ``out``.
 * @retval 0 Malformed input or capacity exceeded.
 *
 * @pre ``in`` and ``out`` non-NULL.
 * @pre ``in`` is NUL-terminated.
 * @post On success ``out[0..return-1]`` holds the decoded bytes.
 * @post On failure ``out`` content is unspecified.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_hex_decode(const char* in, uint8_t* out, uint32_t out_cap)
{
  const uint32_t in_len = (uint32_t)strlen(in);
  if ((in_len % (uint32_t)k_ra8_ota_hex_chars_per_byte) != 0U) {
    return 0U;
  }
  const uint32_t bytes = in_len / (uint32_t)k_ra8_ota_hex_chars_per_byte;
  if (bytes > out_cap) {
    return 0U;
  }
  for (uint32_t i = 0U; i < bytes; ++i) {
    const size_t  base_idx = (size_t)i * (size_t)k_ra8_ota_hex_chars_per_byte;
    const uint8_t hi       = priv_hex_nibble(in[base_idx]);
    const uint8_t lo       = priv_hex_nibble(in[base_idx + 1U]);
    if ((hi == (uint8_t)k_ra8_ota_hex_invalid_nibble) ||
        (lo == (uint8_t)k_ra8_ota_hex_invalid_nibble)) {
      return 0U;
    }
    out[i] = (uint8_t)((hi << (uint8_t)k_ra8_ota_hex_nibble_shift) | lo);
  }
  return bytes;
}

/* =============================================================================
 * Manifest decode
 * ============================================================================= */

/**
 * @brief Pull the sha256 + signature hex blobs out of a JSON manifest.
 *
 * @details Locates the ``"sha256"`` and ``"signature"`` string fields
 *   via ``priv_json_str``, hex-decodes them with ``priv_hex_decode``
 *   into the manifest struct, and validates lengths.
 *
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Manifest struct to populate.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok               Both fields decoded.
 * @retval k_ra8_err_invalid_arg  Field missing or wrong byte length.
 *
 * @pre Both pointers non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``out->image_sha256`` and ``out->signature`` populated.
 * @post On failure ``out`` content is undefined.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_manifest_decode_crypto(const char* json, ra8_ota_manifest_t* out)
{
  char      hex[k_ra8_ota_hex_buf_bytes];
  ra8_err_t e = priv_json_str(json, "\"sha256\"", hex, sizeof hex);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t n_d = priv_hex_decode(hex, out->image_sha256, k_ra8_ota_sha256_bytes);
  if (n_d != k_ra8_ota_sha256_bytes) {
    return k_ra8_err_invalid_arg;
  }

  e = priv_json_str(json, "\"signature\"", hex, sizeof hex);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t n_s = priv_hex_decode(hex, out->signature, k_ra8_ota_signature_max_bytes);
  if (n_s == 0U) {
    return k_ra8_err_invalid_arg;
  }
  out->signature_len = (uint16_t)n_s;
  return k_ra8_ok;
}

/**
 * @brief Decode every field of a JSON manifest into an ``ra8_ota_manifest_t``.
 *
 * @details
 * Zeroes ``*out`` then pulls ``version``, ``url``, ``size`` and finally
 * the cryptographic fields (via ``priv_manifest_decode_crypto``). The
 * size is bounded by ``k_ra8_ota_max_image_bytes``.
 *
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Destination struct (filled even on partial errors).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                Manifest fully decoded.
 * @retval k_ra8_err_invalid_arg   Required field missing or zero size.
 * @retval k_ra8_err_invalid_size  Image size above firmware-wide cap.
 *
 * @pre Both pointers non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``*out`` is fully populated.
 * @post On failure ``*out`` may hold a partial decode.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
ra8_err_t ra8_ota_internal_manifest_decode(const char* json, ra8_ota_manifest_t* out)
{
  (void)memset(out, 0, sizeof *out);
  ra8_err_t e = priv_json_str(json, "\"version\"", out->version, k_ra8_ota_version_str_bytes);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_json_str(json, "\"url\"", out->image_url, k_ra8_ota_url_max_bytes);
  if (e != k_ra8_ok) {
    return e;
  }
  e = ra8_ota_internal_json_u32(json, "\"size\"", &out->image_size_bytes);
  if (e != k_ra8_ok) {
    return e;
  }
  if (out->image_size_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (out->image_size_bytes > k_ra8_ota_max_image_bytes) {
    return k_ra8_err_invalid_size;
  }
  return priv_manifest_decode_crypto(json, out);
}
