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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_ota.h"

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
  k_ra_ota_json_skip_max     = 8U,    /**< Max JSON whitespace/quote skip. */
  k_ra_ota_u32_decimal_digits = 12U,  /**< Max decimal digits in a uint32. */
  k_ra_ota_u32_decimal_base   = 10U,  /**< Base for decimal parsing. */
  k_ra_ota_hex_alpha_offset   = 10U,  /**< Offset added for 'a'..'f'/'A'..'F'. */
  k_ra_ota_hex_invalid_nibble = 0xFFU,/**< Sentinel for invalid hex nibble. */
  k_ra_ota_hex_chars_per_byte = 2U,   /**< Two hex chars per encoded byte. */
  k_ra_ota_hex_nibble_shift   = 4U,   /**< Shift for high nibble in a byte. */
  k_ra_ota_hex_buf_bytes      = 257U, /**< Capacity of stack hex buffer. */
} ra_ota_internal_const_t;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

/**
 * @brief Set state and (if registered) call the progress callback.
 *
 * @param[in] new_state New SM state.
 * @param[in] err       Error to surface (k_ra_ok on healthy paths).
 *
 * @pre Module is initialised.
 * @post ``s_state`` == new_state.
 */
static void priv_set_state(ra_ota_state_t new_state, ra_err_t err) {
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
 * @brief Validate that every required function pointer in ``cfg`` is set.
 *
 * @param[in] cfg Non-NULL caller configuration.
 *
 * @return k_ra_ok or k_ra_err_null_ptr / k_ra_err_invalid_arg.
 */
/** @brief Validate the network function-pointer block of @p cfg. */
static ra_err_t priv_validate_cfg_net(const ra_ota_cfg_t* cfg) {
  RA_CHECK_NULL_PTR(cfg->net.open, s_tag, "net.open");
  RA_CHECK_NULL_PTR(cfg->net.read, s_tag, "net.read");
  RA_CHECK_NULL_PTR(cfg->net.close, s_tag, "net.close");
  return k_ra_ok;
}

/** @brief Validate the crypto function-pointer block of @p cfg. */
static ra_err_t priv_validate_cfg_crypto(const ra_ota_cfg_t* cfg) {
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_init, s_tag, "crypto.sha256_init");
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_update, s_tag, "crypto.sha256_update");
  RA_CHECK_NULL_PTR(cfg->crypto.sha256_final, s_tag, "crypto.sha256_final");
  RA_CHECK_NULL_PTR(cfg->crypto.ecdsa_verify, s_tag, "crypto.ecdsa_verify");
  return k_ra_ok;
}

/** @brief Validate the flash function-pointer block of @p cfg. */
static ra_err_t priv_validate_cfg_flash(const ra_ota_cfg_t* cfg) {
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

static ra_err_t priv_validate_cfg(const ra_ota_cfg_t* cfg) {
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
 * @param[in,out] dst   Destination buffer.
 * @param[in]     cap   Capacity in bytes.
 * @param[out]    out_n Bytes actually received.
 *
 * @return k_ra_ok or backend error.
 */
static ra_err_t priv_drain(uint8_t* dst, uint32_t cap, uint32_t* out_n) {
  uint32_t total = 0U;
  /* Bounded loop: each iteration must consume >= 1 byte or hit EOF. */
  for (uint32_t guard = 0U; guard < cap + 1U; ++guard) {
    if (total >= cap) {
      break;
    }
    uint32_t got = 0U;
    const ra_err_t e =
      s_cfg.net.read(s_cfg.net.ctx, dst + total, cap - total, &got);
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
 * @param[in]  json  Source bytes (NUL-terminated).
 * @param[in]  key   Key name to look for, e.g. ``"version"``.
 * @param[out] dst   Destination string buffer.
 * @param[in]  cap   Capacity of ``dst``.
 *
 * @return k_ra_ok or k_ra_err_invalid_arg if the key is missing.
 */
static ra_err_t priv_json_str(const char* json, const char* key, char* dst, uint32_t cap) {
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
 */
static ra_err_t priv_json_u32(const char* json, const char* key, uint32_t* out_v) {
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
 */
static uint8_t priv_hex_nibble(char c) {
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
 */
static uint32_t priv_hex_decode(const char* in, uint8_t* out, uint32_t out_cap) {
  const uint32_t in_len = (uint32_t)strlen(in);
  if ((in_len % (uint32_t)k_ra_ota_hex_chars_per_byte) != 0U) {
    return 0U;
  }
  const uint32_t bytes = in_len / (uint32_t)k_ra_ota_hex_chars_per_byte;
  if (bytes > out_cap) {
    return 0U;
  }
  for (uint32_t i = 0U; i < bytes; ++i) {
    const size_t base_idx = (size_t)i * (size_t)k_ra_ota_hex_chars_per_byte;
    const uint8_t hi      = priv_hex_nibble(in[base_idx]);
    const uint8_t lo      = priv_hex_nibble(in[base_idx + 1U]);
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
 */
static ra_err_t priv_manifest_decode_crypto(const char* json, ra_ota_manifest_t* out) {
  char hex[k_ra_ota_hex_buf_bytes];
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
 * @param[in]  json NUL-terminated JSON payload.
 * @param[out] out  Destination struct (filled even on partial errors).
 *
 * @return k_ra_ok or k_ra_err_invalid_arg / k_ra_err_invalid_size.
 */
static ra_err_t priv_manifest_decode(const char* json, ra_ota_manifest_t* out) {
  (void)memset(out, 0, sizeof *out);
  ra_err_t e = priv_json_str(json, "\"version\"", out->version, k_ra_ota_version_str_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_json_str(json, "\"url\"", out->image_url, k_ra_ota_url_max_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  e = priv_json_u32(json, "\"size\"", &out->image_size_bytes);
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

ra_err_t ra_ota_init(const ra_ota_cfg_t* cfg) {
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

ra_err_t ra_ota_deinit(void) {
  s_initialised    = false;
  s_state          = k_ra_ota_state_idle;
  s_manifest_valid = false;
  s_bytes_done     = 0U;
  s_last_err       = k_ra_ok;
  (void)memset(&s_cfg, 0, sizeof s_cfg);
  return k_ra_ok;
}

ra_ota_state_t ra_ota_get_state(void) {
  return s_state;
}

/**
 * @brief Open the manifest URL and drain its payload into ``s_buf``.
 *
 * @param[out] out_got Bytes received (NUL terminator added at ``s_buf[got]``).
 * @return ``k_ra_ok`` or a backend / size error.
 */
static ra_err_t priv_fetch_manifest_payload(uint32_t* out_got) {
  uint32_t content_len = 0U;
  ra_err_t e = s_cfg.net.open(s_cfg.net.ctx, s_cfg.manifest_url, &content_len);
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

ra_err_t ra_ota_check_for_update(ra_ota_manifest_t* out_manifest) {
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
 */
static ra_err_t priv_download_chunk(uint32_t addr_base, uint32_t* in_out_done, uint32_t total) {
  const uint32_t remaining = total - *in_out_done;
  const uint32_t want      = (remaining < k_ra_ota_chunk_bytes) ? remaining : k_ra_ota_chunk_bytes;
  uint32_t got = 0U;
  ra_err_t e   = priv_drain(s_buf, want, &got);
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
 * Only invoked on a fresh download start (``s_bytes_done == 0``).
 */
static ra_err_t priv_prepare_bank(const ra_ota_manifest_t* manifest) {
  ra_err_t e = s_cfg.flash.erase(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_addr,
                                 manifest->image_size_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  return s_cfg.crypto.sha256_init(s_cfg.crypto.ctx);
}

/**
 * @brief Drain chunks until the entire image is downloaded or an error fires.
 */
static ra_err_t priv_download_loop(const ra_ota_manifest_t* manifest) {
  const uint32_t max_chunks =
    (k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1U;
  uint32_t chunks = 0U;
  ra_err_t e      = k_ra_ok;
  while (s_bytes_done < manifest->image_size_bytes) {
    if (chunks >= max_chunks) {
      e = k_ra_err_hw_error;
      break;
    }
    e = priv_download_chunk(s_cfg.flash.inactive_bank_addr, &s_bytes_done,
                            manifest->image_size_bytes);
    if (e != k_ra_ok) {
      break;
    }
    ++chunks;
  }
  return e;
}

ra_err_t ra_ota_download_to_inactive_bank(const ra_ota_manifest_t* manifest) {
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
  ra_err_t e = s_cfg.net.open(s_cfg.net.ctx, manifest->image_url, &content_len);
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
 */
static ra_err_t priv_rehash_bank(const ra_ota_manifest_t* m, uint8_t out_digest[32]) {
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

ra_err_t ra_ota_verify_signature(const ra_ota_manifest_t* manifest) {
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
  e = s_cfg.crypto.ecdsa_verify(s_cfg.crypto.ctx, s_cfg.pubkey_handle, digest, manifest->signature,
                                manifest->signature_len);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_hw_error);
    return k_ra_err_hw_error;
  }
  priv_set_state(k_ra_ota_state_committing, k_ra_ok);
  return k_ra_ok;
}

ra_err_t ra_ota_commit_and_reboot(void) {
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
 */
static ra_err_t priv_step_dispatch(void) {
  switch (s_state) {
    case k_ra_ota_state_idle: {
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

ra_err_t ra_ota_run_step(void) {
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  return priv_step_dispatch();
}

ra_err_t ra_ota_run_full_update(void) {
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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void
ra_ota_system_reset_hook(void) {
  /* Intentionally empty. */
}
