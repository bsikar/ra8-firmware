/**
 * @file ra_ota.c
 * @brief Phase-5 OTA firmware-update orchestration -- implementation.
 *
 * @details
 * Plain-C implementation of the state machine declared in
 * ``ra_ota.h``. The module owns three statics:
 *
 *   - ``s_state``  -- single-byte state machine value.
 *   - ``s_cfg``    -- copy of the caller's configuration (function
 *                     pointers + URLs + bank metadata).
 *   - ``s_buf``    -- 4 KiB streaming chunk buffer used both for
 *                     the manifest fetch and the firmware download.
 *   - ``s_manifest`` -- cached decoded manifest from the most
 *                     recent ``ra_ota_check_for_update`` call.
 *
 * No malloc anywhere (NASA Rule 3). Every loop has a static upper
 * bound (NASA Rule 2). Every public entry point has at least two
 * preconditions (NASA Rule 5).
 *
 * The ThreadX worker thread is wired in through weakly-bound
 * functions ``ra_ota_threadx_spawn`` / ``ra_ota_threadx_join`` --
 * the host test build provides empty stubs in this same TU so the
 * module links cleanly on Linux x86_64. A target-side adapter
 * lives outside this module.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ota.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_ota_internal.h"

/* =============================================================================
 * Module-static storage
 * ============================================================================= */

/** @brief Module log tag. */
static const char* const s_tag = "ra_ota";

/** @brief Current state-machine value. */
static ra_ota_state_t s_state = k_ra_ota_state_idle;

/** @brief Configuration captured at init time. */
static ra_ota_cfg_t s_cfg;

/** @brief True once ``ra_ota_init`` has succeeded. */
static bool s_initialised = false;

/** @brief Cached decoded manifest from the most recent check. */
static ra_ota_manifest_t s_manifest;

/** @brief Whether ``s_manifest`` holds a valid payload. */
static bool s_manifest_valid = false;

/** @brief Bytes already programmed into the inactive bank. */
static uint32_t s_bytes_done = 0U;

/** @brief Last error observed by the state machine. */
static ra_err_t s_last_err = k_ra_ok;

/** @brief Streaming buffer reused by manifest + download paths. */
static uint8_t s_buf[k_ra_ota_chunk_bytes];

/**
 * @enum ra_ota_internal_const_t
 * @brief Internal numeric constants used by JSON / hex helpers.
 */
typedef enum : uint32_t {
  k_ra_ota_json_skip_max      = 8U,    /**< Max JSON whitespace/quote skip. */
  k_ra_ota_u32_decimal_digits = 12U,   /**< Max decimal digits in a uint32. */
  k_ra_ota_u32_decimal_base   = 10U,   /**< Base for decimal parsing. */
  k_ra_ota_hex_alpha_offset   = 10U,   /**< Offset added for 'a'..'f'/'A'..'F'. */
  k_ra_ota_hex_invalid_nibble = 0xFFU, /**< Sentinel for invalid hex nibble. */
  k_ra_ota_hex_chars_per_byte = 2U,    /**< Two hex chars per encoded byte. */
  k_ra_ota_hex_nibble_shift   = 4U,    /**< Shift for high nibble in a byte. */
  k_ra_ota_hex_buf_bytes      = 257U,  /**< Capacity of stack hex buffer. */
} ra_ota_internal_const_t;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

/**
 * @brief Set state and (if registered) call the progress callback.
 *
 * @details
 * Updates ``s_state`` and ``s_last_err``, then synthesises a
 * ``ra_ota_progress_t`` snapshot and forwards it to the user-provided
 * ``on_progress`` callback when one was registered at init time.
 *
 * @param[in] new_state New SM state.
 * @param[in] err       Error to surface (k_ra_ok on healthy paths).
 *
 * @pre Module is initialised.
 * @pre Caller is the single OTA worker (no concurrent callers).
 * @post ``s_state`` == new_state.
 * @post ``s_last_err`` == err.
 *
 * @note Static helper; not thread-safe -- the OTA module assumes a
 *       single owning context.
 * @since 0.1.0
 */
static void priv_set_state(ra_ota_state_t new_state, ra_err_t err)
{
  s_state    = new_state;
  s_last_err = err;
  if (s_cfg.on_progress != NULL) {
    const ra_ota_progress_t snap = {
      .state       = new_state,
      .bytes_done  = s_bytes_done,
      .bytes_total = s_manifest_valid ? s_manifest.image_size_bytes : 0U,
      .last_err    = err,
    };
    s_cfg.on_progress(&snap);
  }
}

/**
 * @brief Validate the network function-pointer block of @p cfg.
 *
 * @details
 * Confirms ``cfg->net.open``, ``cfg->net.read`` and ``cfg->net.close``
 * are all non-NULL. Required for the OTA module to fetch manifests
 * and image chunks.
 *
 * @param[in] cfg Caller configuration (already verified non-NULL by
 *                ``priv_validate_cfg``).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok           All net function pointers set.
 * @retval k_ra_err_null_ptr A required net function pointer is NULL.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialised.
 * @post Returns k_ra_ok iff every net pointer is non-NULL.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
static ra_err_t priv_validate_cfg_net(const ra_ota_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg->net.open, s_tag, "net.open");
  RA_CHECK_NULL_PTR(cfg->net.read, s_tag, "net.read");
  RA_CHECK_NULL_PTR(cfg->net.close, s_tag, "net.close");
  return k_ra_ok;
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
 * @return ra_err_t outcome.
 * @retval k_ra_ok           All crypto function pointers set.
 * @retval k_ra_err_null_ptr A required crypto function pointer is NULL.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialised.
 * @post Returns k_ra_ok iff every crypto pointer is non-NULL.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
static ra_err_t priv_validate_cfg_crypto(const ra_ota_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_init, s_tag, "crypto.sha256_init");
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_update, s_tag, "crypto.sha256_update");
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_final, s_tag, "crypto.sha256_final");
  RA_CHECK_NULL_PTR(cfg->crypto.ecdsa_verify, s_tag, "crypto.ecdsa_verify");
  return k_ra_ok;
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
 * @return ra_err_t outcome.
 * @retval k_ra_ok              All flash callbacks set, bank size sane.
 * @retval k_ra_err_null_ptr    A required flash callback is NULL.
 * @retval k_ra_err_invalid_arg ``bank_size_bytes`` is zero or above the cap.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre Module is in the process of being initialised.
 * @post Returns k_ra_ok iff all callbacks are present and the bank size is sane.
 * @post No state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
static ra_err_t priv_validate_cfg_flash(const ra_ota_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg->flash.erase, s_tag, "flash.erase");
  RA_CHECK_NULL_PTR(cfg->flash.program, s_tag, "flash.program");
  RA_CHECK_NULL_PTR(cfg->flash.set_startup, s_tag, "flash.set_startup");
  RA_CHECK_NULL_PTR(cfg->flash.readback, s_tag, "flash.readback");
  if (cfg->flash.bank_size_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->flash.bank_size_bytes > k_ra_ota_max_image_bytes) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Validate the entire OTA configuration descriptor.
 *
 * @details
 * Composes the net/crypto/flash sub-validators and verifies the
 * manifest URL is non-empty. This is the single gate every public
 * ``ra_ota_init`` call must pass before the module captures the
 * config into ``s_cfg``.
 *
 * @param[in] cfg Caller configuration (may be NULL -- checked here).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok              Configuration is valid.
 * @retval k_ra_err_null_ptr    ``cfg`` or a sub-pointer is NULL.
 * @retval k_ra_err_invalid_arg Bank size out of range or empty URL.
 *
 * @pre Module init is in progress (no concurrent OTA operation).
 * @pre Caller has not yet committed ``cfg`` to ``s_cfg``.
 * @post Returns k_ra_ok iff every required field is populated.
 * @post No module state mutated.
 *
 * @note Static helper; pure validation function.
 * @since 0.1.0
 */
static ra_err_t priv_validate_cfg(const ra_ota_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  ra_err_t e = priv_validate_cfg_net(cfg);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_validate_cfg_crypto(cfg);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_validate_cfg_flash(cfg);
  if (e != k_ra_ok) {
    return e;
  }
  if (cfg->manifest_url[0] == '\0') {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Drain the network stream and accumulate up to ``cap`` bytes.
 *
 * @details
 * Loops calling the user-supplied ``s_cfg.net.read`` callback until
 * either ``cap`` bytes have been collected or the backend reports EOF
 * (``got == 0``). The loop is bounded by ``cap + 1`` iterations
 * (NASA Rule 2). Returns the byte count via ``out_n``.
 *
 * @param[in,out] dst   Destination buffer.
 * @param[in]     cap   Capacity in bytes.
 * @param[out]    out_n Bytes actually received.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok        Drain completed (possibly short on EOF).
 * @retval other          Whatever the network backend returned.
 *
 * @pre Module is initialised and ``s_cfg.net.read`` is set.
 * @pre ``dst`` and ``out_n`` are non-NULL.
 * @post On success ``*out_n`` reflects bytes written into ``dst``.
 * @post On failure ``*out_n`` is unspecified.
 *
 * @note Static helper; not thread-safe -- shares the OTA worker context.
 * @since 0.1.0
 */
static ra_err_t priv_drain(uint8_t* dst, uint32_t cap, uint32_t* out_n)
{
  uint32_t total = 0U;
  /* Bounded loop: each iteration must consume >= 1 byte or hit EOF. */
  for (uint32_t guard = 0U; guard < cap + 1U; ++guard) {
    if (total >= cap) {
      break;
    }
    uint32_t       got = 0U;
    const ra_err_t e   = s_cfg.net.read(s_cfg.net.ctx, dst + total, cap - total, &got);
    if (e != k_ra_ok) {
      return e;
    }
    if (got == 0U) {
      break; /* EOF */
    }
    total += got;
  }
  *out_n = total;
  return k_ra_ok;
}

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
 * @return ra_err_t outcome.
 * @retval k_ra_ok               String value copied into ``dst``.
 * @retval k_ra_err_invalid_arg  Key not found or quotes missing.
 * @retval k_ra_err_invalid_size Value would not fit in ``dst``.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``dst`` is a NUL-terminated copy of the value.
 * @post On failure ``dst`` content is undefined.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
static ra_err_t priv_json_str(const char* json, const char* key, char* dst, uint32_t cap)
{
  const char* p = strstr(json, key);
  if (p == NULL) {
    return k_ra_err_invalid_arg;
  }
  p = strchr(p + strlen(key), '"');
  if (p == NULL) {
    return k_ra_err_invalid_arg;
  }
  ++p;
  const char* q = strchr(p, '"');
  if (q == NULL) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t n = (uint32_t)(q - p);
  if (n + 1U > cap) {
    return k_ra_err_invalid_size;
  }
  (void)memcpy(dst, p, n);
  dst[n] = '\0';
  return k_ra_ok;
}

/**
 * @brief Parse a decimal ``"key": NNN`` field out of a JSON-ish buffer.
 *
 * @details
 * Locates ``key`` via ``strstr``, skips past colon/quote/whitespace
 * (bounded by ``k_ra_ota_json_skip_max``) then accumulates a base-10
 * value out of up to ``k_ra_ota_u32_decimal_digits`` digit characters.
 * Both inner loops are statically bounded (NASA Rule 2).
 *
 * @param[in]  json  Source JSON bytes (NUL-terminated).
 * @param[in]  key   Key string including its quotes, e.g. ``"\"size\""``.
 * @param[out] out_v Receives the parsed value on success.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok              Value parsed into ``*out_v``.
 * @retval k_ra_err_invalid_arg Key not found or no digits after the colon.
 *
 * @pre All pointer arguments are non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``*out_v`` reflects the parsed unsigned value.
 * @post On failure ``*out_v`` is unchanged.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
ra_err_t ra_ota_internal_json_u32(const char* json, const char* key, uint32_t* out_v)
{
  const char* p = strstr(json, key);
  if (p == NULL) {
    return k_ra_err_invalid_arg;
  }
  p += strlen(key);
  /* Skip past quote/colon/whitespace. */
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra_ota_json_skip_max; ++guard) {
    if (*p == ':' || *p == ' ' || *p == '"') {
      ++p;
    } else {
      break;
    }
  }
  uint32_t v = 0U;
  uint32_t i = 0U;
  for (; i < (uint32_t)k_ra_ota_u32_decimal_digits; ++i) {
    const char c = p[i];
    if ((c < '0') || (c > '9')) {
      break;
    }
    v = (v * (uint32_t)k_ra_ota_u32_decimal_base) + (uint32_t)(c - '0');
  }
  if (i == 0U) {
    return k_ra_err_invalid_arg;
  }
  *out_v = v;
  return k_ra_ok;
}

/**
 * @brief Decode a single hex nibble. Returns 0xFFU on invalid input.
 *
 * @details Maps ``'0'..'9'`` to 0..9 and ``'a'..'f'`` / ``'A'..'F'`` to
 *   10..15 via ``k_ra_ota_hex_alpha_offset``. Any other character returns
 *   ``k_ra_ota_hex_invalid_nibble`` (0xFFU).
 *
 * @param[in] c Candidate hex character.
 *
 * @return Nibble value 0..15.
 * @retval k_ra_ota_hex_invalid_nibble Character is not a hex digit.
 *
 * @pre None.
 * @post No state mutated.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static uint8_t priv_hex_nibble(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  if ((c >= 'a') && (c <= 'f')) {
    return (uint8_t)((uint8_t)k_ra_ota_hex_alpha_offset + (c - 'a'));
  }
  if ((c >= 'A') && (c <= 'F')) {
    return (uint8_t)((uint8_t)k_ra_ota_hex_alpha_offset + (c - 'A'));
  }
  return (uint8_t)k_ra_ota_hex_invalid_nibble;
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
static uint32_t priv_hex_decode(const char* in, uint8_t* out, uint32_t out_cap)
{
  const uint32_t in_len = (uint32_t)strlen(in);
  if ((in_len % (uint32_t)k_ra_ota_hex_chars_per_byte) != 0U) {
    return 0U;
  }
  const uint32_t bytes = in_len / (uint32_t)k_ra_ota_hex_chars_per_byte;
  if (bytes > out_cap) {
    return 0U;
  }
  for (uint32_t i = 0U; i < bytes; ++i) {
    const size_t  base_idx = (size_t)i * (size_t)k_ra_ota_hex_chars_per_byte;
    const uint8_t hi       = priv_hex_nibble(in[base_idx]);
    const uint8_t lo       = priv_hex_nibble(in[base_idx + 1U]);
    if ((hi == (uint8_t)k_ra_ota_hex_invalid_nibble) ||
        (lo == (uint8_t)k_ra_ota_hex_invalid_nibble)) {
      return 0U;
    }
    out[i] = (uint8_t)((hi << (uint8_t)k_ra_ota_hex_nibble_shift) | lo);
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
 * @return ra_err_t outcome.
 * @retval k_ra_ok               Both fields decoded.
 * @retval k_ra_err_invalid_arg  Field missing or wrong byte length.
 *
 * @pre Both pointers non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``out->image_sha256`` and ``out->signature`` populated.
 * @post On failure ``out`` content is undefined.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
static ra_err_t priv_manifest_decode_crypto(const char* json, ra_ota_manifest_t* out)
{
  char     hex[k_ra_ota_hex_buf_bytes];
  ra_err_t e = priv_json_str(json, "\"sha256\"", hex, sizeof hex);
  if (e != k_ra_ok) {
    return e;
  }
  const uint32_t n_d = priv_hex_decode(hex, out->image_sha256, k_ra_ota_sha256_bytes);
  if (n_d != k_ra_ota_sha256_bytes) {
    return k_ra_err_invalid_arg;
  }

  e = priv_json_str(json, "\"signature\"", hex, sizeof hex);
  if (e != k_ra_ok) {
    return e;
  }
  const uint32_t n_s = priv_hex_decode(hex, out->signature, k_ra_ota_signature_max_bytes);
  if (n_s == 0U) {
    return k_ra_err_invalid_arg;
  }
  out->signature_len = (uint16_t)n_s;
  return k_ra_ok;
}

/**
 * @brief Decode every field of a JSON manifest into an ``ra_ota_manifest_t``.
 *
 * @details
 * Zeroes ``*out`` then pulls ``version``, ``url``, ``size`` and finally
 * the cryptographic fields (via ``priv_manifest_decode_crypto``). The
 * size is bounded by ``k_ra_ota_max_image_bytes``.
 *
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Destination struct (filled even on partial errors).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Manifest fully decoded.
 * @retval k_ra_err_invalid_arg   Required field missing or zero size.
 * @retval k_ra_err_invalid_size  Image size above firmware-wide cap.
 *
 * @pre Both pointers non-NULL.
 * @pre ``json`` is NUL-terminated.
 * @post On success ``*out`` is fully populated.
 * @post On failure ``*out`` may hold a partial decode.
 *
 * @note Static helper; pure function.
 * @since 0.1.0
 */
static ra_err_t priv_manifest_decode(const char* json, ra_ota_manifest_t* out)
{
  (void)memset(out, 0, sizeof *out);
  ra_err_t e = priv_json_str(json, "\"version\"", out->version, k_ra_ota_version_str_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_json_str(json, "\"url\"", out->image_url, k_ra_ota_url_max_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  e = ra_ota_internal_json_u32(json, "\"size\"", &out->image_size_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  if (out->image_size_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (out->image_size_bytes > k_ra_ota_max_image_bytes) {
    return k_ra_err_invalid_size;
  }
  return priv_manifest_decode_crypto(json, out);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Initialise the OTA module from a caller-supplied configuration.
 *
 * @details
 * Verifies the module is in the un-initialised state, runs the full
 * ``priv_validate_cfg`` check on ``cfg``, then captures the descriptor
 * by-value into ``s_cfg`` and resets the state machine to
 * ``k_ra_ota_state_idle``.
 *
 * @param[in] cfg Configuration descriptor (function pointers + URLs).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Module initialised.
 * @retval k_ra_err_invalid_state Module already initialised.
 * @retval k_ra_err_null_ptr      ``cfg`` (or sub-pointer) was NULL.
 * @retval k_ra_err_invalid_arg   Configuration field out of range.
 *
 * @pre Module is uninitialised (or ``ra_ota_deinit`` was called).
 * @pre All function pointers in ``cfg`` are wired up.
 * @post On success the module is in ``k_ra_ota_state_idle``.
 * @post On failure no module state was mutated.
 *
 * @par Example:
 * @code
 * ra_ota_cfg_t cfg = { .net = { ... }, .crypto = { ... } };
 * RA_RETURN_ON_ERROR(ra_ota_init(&cfg), s_tag, "ota init");
 * @endcode
 *
 * @see ra_ota_deinit()
 * @see ra_ota_run_full_update()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_init(const ra_ota_cfg_t* cfg)
{
  if (s_initialised) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t e = priv_validate_cfg(cfg);
  if (e != k_ra_ok) {
    return e;
  }
  (void)memcpy(&s_cfg, cfg, sizeof s_cfg);
  s_state          = k_ra_ota_state_idle;
  s_manifest_valid = false;
  s_bytes_done     = 0U;
  s_last_err       = k_ra_ok;
  s_initialised    = true;
  /* run_as_thread is honoured by an external adapter -- on host the
   * caller drives ra_ota_run_step() directly. */
  return k_ra_ok;
}

/**
 * @brief Reset the OTA module to its un-initialised state.
 *
 * @details
 * Clears the cached configuration, manifest, byte-counter and last
 * error so a future ``ra_ota_init`` starts from a clean slate.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre None (safe to call before init).
 * @post ``s_initialised`` is false.
 * @post ``s_cfg`` is zeroed.
 *
 * @see ra_ota_init()
 *
 * @note Thread-safe: no -- intended to be called when no OTA worker
 *       is running.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 */
ra_err_t ra_ota_deinit(void)
{
  s_initialised    = false;
  s_state          = k_ra_ota_state_idle;
  s_manifest_valid = false;
  s_bytes_done     = 0U;
  s_last_err       = k_ra_ok;
  (void)memset(&s_cfg, 0, sizeof s_cfg);
  return k_ra_ok;
}

/**
 * @brief Return the current OTA state-machine value.
 *
 * @details
 * Reads the latched ``s_state`` directly. ``s_state`` is a single byte,
 * so a torn read is impossible on the target.
 *
 * @return ra_ota_state_t outcome.
 * @retval k_ra_ota_state_idle Module not initialised, or genuinely idle.
 * @retval other               Whatever state the worker last latched.
 *
 * @pre None (safe to call before init -- returns ``idle``).
 * @post No state mutated.
 *
 * @see ra_ota_run_step()
 *
 * @note Thread-safe: yes -- single-byte read of a static.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
ra_ota_state_t ra_ota_get_state(void)
{
  return s_state;
}

/**
 * @brief Open the manifest URL and drain its payload into ``s_buf``.
 *
 * @details
 * Calls ``s_cfg.net.open`` on the configured manifest URL, validates
 * the advertised content length is below ``k_ra_ota_manifest_max_bytes``,
 * then drains via ``priv_drain`` and appends a NUL byte so the JSON
 * helpers may use ``strstr``.
 *
 * @param[out] out_got Bytes received (NUL terminator added at ``s_buf[got]``).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Payload fetched into ``s_buf``.
 * @retval k_ra_err_invalid_size  Server advertised too large a body.
 * @retval other                  Whatever the network backend returned.
 *
 * @pre Module is initialised.
 * @pre ``out_got`` non-NULL.
 * @post On success ``s_buf[0..*out_got]`` holds the payload + trailing NUL.
 * @post On failure the network connection has been closed.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_fetch_manifest_payload(uint32_t* out_got)
{
  uint32_t content_len = 0U;
  ra_err_t e           = s_cfg.net.open(s_cfg.net.ctx, s_cfg.manifest_url, &content_len);
  if (e != k_ra_ok) {
    return e;
  }
  if (content_len > k_ra_ota_manifest_max_bytes) {
    (void)s_cfg.net.close(s_cfg.net.ctx);
    return k_ra_err_invalid_size;
  }
  uint32_t got = 0U;
  e            = priv_drain(s_buf, k_ra_ota_manifest_max_bytes - 1U, &got);
  (void)s_cfg.net.close(s_cfg.net.ctx);
  if (e != k_ra_ok) {
    return e;
  }
  s_buf[got] = 0U; /* NUL terminate so JSON helpers can use strstr. */
  *out_got   = got;
  return k_ra_ok;
}

/**
 * @brief Fetch and decode the upstream manifest, leaving it cached.
 *
 * @details
 * Transitions the state machine ``idle -> checking -> idle`` (success)
 * or ``idle -> checking -> error`` (failure). On success the manifest
 * is mirrored both into ``*out_manifest`` and into the module-private
 * ``s_manifest`` so subsequent steps can refer to it.
 *
 * @param[out] out_manifest Caller-owned manifest buffer.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Manifest cached and returned.
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval k_ra_err_null_ptr        ``out_manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``idle``.
 * @retval other                    Network or decode error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @pre Module is in ``k_ra_ota_state_idle``.
 * @post On success state == ``idle``, manifest is cached.
 * @post On failure state == ``error`` with ``s_last_err`` set.
 *
 * @see ra_ota_download_to_inactive_bank()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_check_for_update(ra_ota_manifest_t* out_manifest)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(out_manifest, s_tag, "out_manifest");
  if (s_state != k_ra_ota_state_idle) {
    return k_ra_err_invalid_state;
  }
  priv_set_state(k_ra_ota_state_checking, k_ra_ok);

  uint32_t got = 0U;
  ra_err_t e   = priv_fetch_manifest_payload(&got);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }

  e = priv_manifest_decode((const char*)s_buf, out_manifest);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  (void)memcpy(&s_manifest, out_manifest, sizeof s_manifest);
  s_manifest_valid = true;
  priv_set_state(k_ra_ota_state_idle, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Stream one chunk: drain network -> hash -> flash program.
 *
 * @details
 * Computes a chunk size capped at ``k_ra_ota_chunk_bytes``, drains it
 * via ``priv_drain``, updates the SHA-256 accumulator, programs it
 * into flash at ``addr_base + *in_out_done`` and bumps the running
 * counter.
 *
 * @param[in]     addr_base   Bank base address inside flash.
 * @param[in,out] in_out_done Bytes already programmed; bumped on success.
 * @param[in]     total       Image size in bytes.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok            Chunk programmed and accumulator updated.
 * @retval k_ra_err_hw_error  Backend EOF before image complete.
 * @retval other              Network / crypto / flash error.
 *
 * @pre Module is in ``downloading`` (or about to enter it).
 * @pre Pointers non-NULL.
 * @post On success ``*in_out_done`` increased by the chunk byte count.
 * @post On failure no flash bytes were programmed in this call.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_download_chunk(uint32_t addr_base, uint32_t* in_out_done, uint32_t total)
{
  const uint32_t remaining = total - *in_out_done;
  const uint32_t want      = (remaining < k_ra_ota_chunk_bytes) ? remaining : k_ra_ota_chunk_bytes;
  uint32_t       got       = 0U;
  ra_err_t       e         = priv_drain(s_buf, want, &got);
  if (e != k_ra_ok) {
    return e;
  }
  if (got == 0U) {
    return k_ra_err_hw_error;
  }
  e = s_cfg.crypto.sha256_update(s_cfg.crypto.ctx, s_buf, got);
  if (e != k_ra_ok) {
    return e;
  }
  e = s_cfg.flash.program(s_cfg.flash.ctx, addr_base + *in_out_done, s_buf, got);
  if (e != k_ra_ok) {
    return e;
  }
  *in_out_done += got;
  priv_set_state(k_ra_ota_state_downloading, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Erase the inactive bank and prime the SHA accumulator.
 *
 * @details
 * Only invoked on a fresh download start (``s_bytes_done == 0``).
 * Erases ``manifest->image_size_bytes`` worth of inactive-bank flash
 * then re-initialises the SHA-256 accumulator so the new download is
 * hashed from byte 0.
 *
 * @param[in] manifest Currently active manifest.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Bank erased and SHA primed.
 * @retval other   Whatever the flash erase / sha init returned.
 *
 * @pre ``manifest`` non-NULL.
 * @pre Module owns the inactive bank.
 * @post On success the inactive bank is fully erased and SHA is primed.
 * @post On failure the bank may be partially erased.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_prepare_bank(const ra_ota_manifest_t* manifest)
{
  ra_err_t e =
    s_cfg.flash.erase(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_addr, manifest->image_size_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  return s_cfg.crypto.sha256_init(s_cfg.crypto.ctx);
}

/**
 * @brief Drain chunks until the entire image is downloaded or an error fires.
 *
 * @details
 * Loops calling ``priv_download_chunk`` until ``s_bytes_done`` reaches
 * ``manifest->image_size_bytes``. Bounded by
 * ``(k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1`` iterations
 * (NASA Rule 2).
 *
 * @param[in] manifest Manifest describing the in-flight image.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok           Download completed.
 * @retval k_ra_err_hw_error Chunk count exceeded the static cap.
 * @retval other             Whatever ``priv_download_chunk`` returned.
 *
 * @pre Module is in ``downloading``.
 * @pre ``manifest`` non-NULL.
 * @post On success ``s_bytes_done == manifest->image_size_bytes``.
 * @post On failure ``s_bytes_done`` reflects the partial progress.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_download_loop(const ra_ota_manifest_t* manifest)
{
  const uint32_t max_chunks = (k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1U;
  uint32_t       chunks     = 0U;
  ra_err_t       e          = k_ra_ok;
  while (s_bytes_done < manifest->image_size_bytes) {
    if (chunks >= max_chunks) {
      e = k_ra_err_hw_error;
      break;
    }
    e = priv_download_chunk(s_cfg.flash.inactive_bank_addr,
                            &s_bytes_done,
                            manifest->image_size_bytes);
    if (e != k_ra_ok) {
      break;
    }
    ++chunks;
  }
  return e;
}

/**
 * @brief Download an image into the inactive bank, hashing as it goes.
 *
 * @details
 * On a fresh start (``s_bytes_done == 0``) erases the bank and primes
 * the SHA accumulator via ``priv_prepare_bank``. Then opens the image
 * URL and runs ``priv_download_loop``. On success the state machine
 * lands in ``verifying``; on failure it lands in ``error``.
 *
 * @param[in] manifest Manifest describing the image to fetch.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Download complete; ready to verify.
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval k_ra_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``idle`` or ``downloading``.
 * @retval k_ra_err_invalid_size    Image larger than the configured bank.
 * @retval other                    Network / crypto / flash error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @pre ``manifest`` non-NULL.
 * @post On success state == ``verifying``.
 * @post On failure state == ``error`` with ``s_last_err`` set.
 *
 * @see ra_ota_verify_signature()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_download_to_inactive_bank(const ra_ota_manifest_t* manifest)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(manifest, s_tag, "manifest");
  if ((s_state != k_ra_ota_state_idle) && (s_state != k_ra_ota_state_downloading)) {
    return k_ra_err_invalid_state;
  }
  if (manifest->image_size_bytes > s_cfg.flash.bank_size_bytes) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_invalid_size);
    return k_ra_err_invalid_size;
  }

  if (s_bytes_done == 0U) {
    const ra_err_t e = priv_prepare_bank(manifest);
    if (e != k_ra_ok) {
      priv_set_state(k_ra_ota_state_error, e);
      return e;
    }
  }

  uint32_t content_len = 0U;
  ra_err_t e           = s_cfg.net.open(s_cfg.net.ctx, manifest->image_url, &content_len);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_downloading, k_ra_ok);

  e = priv_download_loop(manifest);
  (void)s_cfg.net.close(s_cfg.net.ctx);

  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_verifying, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Re-hash the inactive bank to re-derive the digest after program.
 *
 * @details
 * Re-initialises the SHA accumulator then walks the inactive bank in
 * ``k_ra_ota_chunk_bytes`` chunks via ``s_cfg.flash.readback``,
 * feeding each one to ``s_cfg.crypto.sha256_update``. Finalises into
 * ``out_digest``. Loop is bounded by
 * ``(k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1``.
 *
 * @param[in]  m          Manifest (provides ``image_size_bytes``).
 * @param[out] out_digest 32-byte SHA-256 destination.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Digest derived.
 * @retval other   Whatever the readback / crypto callbacks returned.
 *
 * @pre Module is in ``verifying``.
 * @pre Pointers non-NULL.
 * @post On success ``out_digest`` holds SHA-256 of the bank contents.
 * @post On failure ``out_digest`` content is unspecified.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_rehash_bank(const ra_ota_manifest_t* m, uint8_t out_digest[32])
{
  ra_err_t e = s_cfg.crypto.sha256_init(s_cfg.crypto.ctx);
  if (e != k_ra_ok) {
    return e;
  }
  uint32_t       offset     = 0U;
  const uint32_t max_chunks = (k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1U;
  for (uint32_t i = 0U; i < max_chunks; ++i) {
    if (offset >= m->image_size_bytes) {
      break;
    }
    const uint32_t remaining = m->image_size_bytes - offset;
    const uint32_t want = (remaining < k_ra_ota_chunk_bytes) ? remaining : k_ra_ota_chunk_bytes;
    e = s_cfg.flash.readback(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_addr + offset, s_buf, want);
    if (e != k_ra_ok) {
      return e;
    }
    e = s_cfg.crypto.sha256_update(s_cfg.crypto.ctx, s_buf, want);
    if (e != k_ra_ok) {
      return e;
    }
    offset += want;
  }
  return s_cfg.crypto.sha256_final(s_cfg.crypto.ctx, out_digest);
}

/**
 * @brief Verify the freshly-programmed bank against the manifest signature.
 *
 * @details
 * Re-hashes the inactive bank via ``priv_rehash_bank``, compares the
 * digest against ``manifest->image_sha256``, and on a match invokes
 * the configured ECDSA verifier with the public-key handle and the
 * manifest signature. On success the state machine lands in
 * ``committing``.
 *
 * @param[in] manifest Manifest used for the download.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Image authenticated.
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval k_ra_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``verifying``.
 * @retval k_ra_err_crc_mismatch    SHA-256 mismatch (image corrupt).
 * @retval k_ra_err_hw_error        ECDSA verify rejected the signature.
 * @retval other                    Crypto / flash backend error.
 *
 * @pre ``ra_ota_download_to_inactive_bank`` succeeded.
 * @pre ``manifest`` non-NULL.
 * @post On success state == ``committing``.
 * @post On failure state == ``error``.
 *
 * @see ra_ota_commit_and_reboot()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_verify_signature(const ra_ota_manifest_t* manifest)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(manifest, s_tag, "manifest");
  if (s_state != k_ra_ota_state_verifying) {
    return k_ra_err_invalid_state;
  }

  uint8_t  digest[k_ra_ota_sha256_bytes] = {};
  ra_err_t e                             = priv_rehash_bank(manifest, digest);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  if (memcmp(digest, manifest->image_sha256, k_ra_ota_sha256_bytes) != 0) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_crc_mismatch);
    return k_ra_err_crc_mismatch;
  }
  e = s_cfg.crypto.ecdsa_verify(s_cfg.crypto.ctx,
                                s_cfg.pubkey_handle,
                                digest,
                                manifest->signature,
                                manifest->signature_len);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_hw_error);
    return k_ra_err_hw_error;
  }
  priv_set_state(k_ra_ota_state_committing, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Latch the inactive bank as the next boot bank and reboot.
 *
 * @details
 * Calls ``s_cfg.flash.set_startup`` to mark the inactive bank as the
 * boot bank, then invokes ``ra_ota_system_reset_hook`` (which on
 * hardware overrides to ``NVIC_SystemReset`` and on host is a no-op
 * for testability).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Bank latched (the call normally
 *                                  doesn't return on hardware).
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval k_ra_err_invalid_state   Module not in ``committing``.
 * @retval other                    Backend error from set_startup.
 *
 * @pre ``ra_ota_verify_signature`` succeeded.
 * @pre Module is in ``k_ra_ota_state_committing``.
 * @post On success state == ``done`` and the system reset hook fired.
 * @post On failure state == ``error``.
 *
 * @see ra_ota_system_reset_hook()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_commit_and_reboot(void)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (s_state != k_ra_ota_state_committing) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t e =
    s_cfg.flash.set_startup(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_index, true);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_done, k_ra_ok);
  /* On hardware ra_ota_system_reset_hook is overridden to call
   * NVIC_SystemReset; in the host build it is a no-op. */
  ra_ota_system_reset_hook();
  return k_ra_ok;
}

/**
 * @brief Drive one transition based on the current state.
 *
 * @details
 * Switches on ``s_state`` and dispatches to the matching public-API
 * function (``check_for_update``, ``download_to_inactive_bank``,
 * ``verify_signature`` or ``commit_and_reboot``). Terminal states
 * (``done`` / ``error``) return ``k_ra_ok`` so the caller may stop
 * polling.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Step completed (or terminal state reached).
 * @retval other   Whatever the dispatched function returned.
 *
 * @pre Module is initialised.
 * @post The state machine has advanced by at most one transition.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static ra_err_t priv_step_dispatch(void)
{
  switch (s_state) {
    case k_ra_ota_state_idle: {
      /* If a previous run_step already fetched and validated the
       * manifest, advance to download instead of re-fetching it. */
      if (s_manifest_valid) {
        return ra_ota_download_to_inactive_bank(&s_manifest);
      }
      ra_ota_manifest_t m;
      return ra_ota_check_for_update(&m);
    }
    case k_ra_ota_state_checking:
    case k_ra_ota_state_downloading:
      if (s_manifest_valid) {
        return ra_ota_download_to_inactive_bank(&s_manifest);
      }
      return k_ra_err_invalid_state;
    case k_ra_ota_state_verifying:
      return ra_ota_verify_signature(&s_manifest);
    case k_ra_ota_state_committing:
      return ra_ota_commit_and_reboot();
    case k_ra_ota_state_done:
    case k_ra_ota_state_error:
    case k_ra_ota_state_count:
    default:
      return k_ra_ok;
  }
}

/**
 * @brief Drive the OTA state machine one step forward.
 *
 * @details
 * Thin wrapper over ``priv_step_dispatch`` that gates on
 * ``s_initialised``. Intended for callers that opted out of running
 * the OTA worker as a background thread.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Step completed.
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval other                    Step-specific error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @post The state machine has advanced by at most one transition.
 *
 * @see ra_ota_get_state()
 * @see ra_ota_run_full_update()
 *
 * @note Thread-safe: no -- single owner only.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
ra_err_t ra_ota_run_step(void)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  return priv_step_dispatch();
}

/**
 * @brief Drive the OTA state machine through an end-to-end update.
 *
 * @details
 * Loops calling ``ra_ota_run_step`` for at most ``k_ra_ota_state_count``
 * iterations (NASA Rule 2 bound: idle -> checking -> downloading ->
 * verifying -> committing -> done). Stops early on ``done`` or
 * ``error``.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Update completed (or already done).
 * @retval k_ra_err_not_initialized Module not initialised.
 * @retval other                    Whatever the failing step returned.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @post Module is in ``done`` (success) or ``error`` (failure).
 *
 * @see ra_ota_run_step()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
ra_err_t ra_ota_run_full_update(void)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  /* Bounded by the longest legal sequence (idle -> checking ->
   * downloading -> verifying -> committing -> done). Six steps is
   * the upper bound; pad to ``k_ra_ota_state_count`` for safety. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra_ota_state_count; ++i) {
    if ((s_state == k_ra_ota_state_done) || (s_state == k_ra_ota_state_error)) {
      break;
    }
    const ra_err_t e = ra_ota_run_step();
    if (e != k_ra_ok) {
      return e;
    }
  }
  return s_last_err;
}

/* =============================================================================
 * Weak system-reset hook: real target overrides this with
 * ``NVIC_SystemReset()``.  Default is a no-op so unit tests don't
 * actually exit the process.
 * ============================================================================= */

/**
 * @brief Weak system-reset hook overridden by the target build.
 *
 * @details
 * Called from ``ra_ota_commit_and_reboot`` after the bank-swap is
 * latched. The hardware build overrides this with a definition that
 * calls ``NVIC_SystemReset``. The host (unit-test) build keeps the
 * weak no-op default so tests can observe post-commit state without
 * actually exiting the process.
 *
 * @return None.
 *
 * @pre Weak symbol; safe to leave unimplemented.
 * @post Default no-op; target override never returns.
 *
 * @par Example:
 * @code
 * // In target firmware:
 * void ra_ota_system_reset_hook(void) { NVIC_SystemReset(); }
 * @endcode
 *
 * @note Thread-safe: target override does not return, so trivially safe.
 * @since 0.1.0
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
/**
 * @brief Ra ota system reset hook.
 *
 * @details See implementation for details.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ra_ota_system_reset_hook(void)
{
  /* Intentionally empty. */
}
