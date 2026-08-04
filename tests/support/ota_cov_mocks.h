/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ota_cov_mocks.h
 * @brief Shared mock harness for the ra8_ota coverage-gap test suite.
 *
 * @details
 * Header-only fixture extracted from test_ra8_ota_cov.c to keep that test
 * translation unit under the repository file-size cap. Provides the
 * network / crypto / flash mock callbacks with per-call fault injection
 * knobs, plus the priv_* fixture builders (image, manifest, config). The
 * mock state is file-scope static, so every including test binary gets an
 * independent copy.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"

/** @brief Erased-flash byte the mocked OTA bank reports after an erase. */
typedef enum : uint8_t {
  k_ota_cov_erased_byte = 0xFFU, /**< Matches real NOR/MRAM erase state. */
} ota_cov_fill_t;

/**
 * @enum ota_cov_mocks_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ota_cov_mocks_i_a5        = 0xA5U, /**< XOR key for the deterministic mock image pattern. */
  k_ota_cov_mocks_nibble_mask = 0x0FU, /**< Low-nibble mask while hex-encoding a digest byte. */
  k_ota_cov_mocks_sig_seed    = 0xC0U, /**< First byte of the synthetic signature ramp.       */
} ota_cov_mocks_uint8_const_t;

/**
 * @enum ota_cov_mocks_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_ota_cov_mocks_manifest_cap = 2048, /**< Manifest text buffer capacity. */
} ota_cov_mocks_uint16_const_t;

/* =============================================================================
 * Mock constants
 * ============================================================================= */

typedef enum : uint32_t {
  k_cov_image_size = 256U,         /**< Cov image size.   */
  k_cov_bank_size  = 4096U,        /**< Cov bank size.    */
  k_cov_bank_addr  = 0x02080000UL, /**< Cov bank address. */
  k_cov_bank_index = 1U,           /**< Cov bank index.   */
  k_cov_pubkey     = 0xBEEFU,      /**< Cov pubkey.       */
  k_cov_huge_len   = 9999U,        /**< Cov huge length.  */
} ra8_ota_cov_const_t;

/* =============================================================================
 * Mock state
 * ============================================================================= */

/** @brief Manifest JSON rendered for a valid fixture. */
static char g_cov_manifest[k_ota_cov_mocks_manifest_cap];
/** @brief Raw bytes of the mock firmware image. */
static uint8_t g_cov_image[k_cov_image_size];
/** @brief Inactive-bank sandbox. */
static uint8_t g_cov_bank[k_cov_bank_size];
/** @brief Running XOR pseudo-hash accumulator. */
static uint8_t g_cov_hash[k_ra8_ota_sha256_bytes];
/** @brief Expected hash stored in the manifest. */
static uint8_t g_cov_expected_hash[k_ra8_ota_sha256_bytes];
/** @brief Expected signature stored in the manifest. */
static uint8_t g_cov_sig[8];

/** @brief Network cursor. */
static uint32_t g_cov_net_offset;
/** @brief Total bytes the open session promises. */
static uint32_t g_cov_net_total;
/** @brief Pointer to the active network payload. */
static const uint8_t* g_cov_net_payload;

/* -- fault-injection flags -------------------------------------------------- */

/** @brief When >0, net.open fails on the Nth call (1=first). */
static int g_cov_net_open_fail_n;
/** @brief Net.open call counter. */
static int g_cov_net_open_call_cnt;
/** @brief When true, net.open reports content_len > manifest max. */
static bool g_cov_net_open_huge;
/** @brief When >0, net.read fails on the Nth call (1=first). */
static int g_cov_net_read_fail_n;
/** @brief Net.read call counter. */
static int g_cov_net_read_call_cnt;
/** @brief When true, first net.read returns 0 bytes (premature EOF). */
static bool g_cov_net_read_zero_first;

/** @brief When >0, sha256_init fails on the Nth call (1=first). */
static int g_cov_sha_init_fail_n;
/** @brief sha256_init call counter. */
static int g_cov_sha_init_call_cnt;
/** @brief When >0, sha256_update fails on the Nth call (1=first). */
static int g_cov_sha_upd_fail_n;
/** @brief sha256_update call counter. */
static int g_cov_sha_upd_call_cnt;

/** @brief When true, flash.erase returns error. */
static bool g_cov_flash_erase_fail;
/** @brief When true, flash.program returns error. */
static bool g_cov_flash_prog_fail;
/** @brief When true, flash.readback returns error. */
static bool g_cov_flash_rb_fail;
/** @brief When true, flash.set_startup returns error. */
static bool g_cov_flash_startup_fail;

/** @brief Count of on_progress callback invocations. */
static uint32_t g_cov_progress_cnt;

/* =============================================================================
 * Mock network interface
 * ============================================================================= */

static inline ra8_err_t cov_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  (void)url;
  ++g_cov_net_open_call_cnt;
  if ((g_cov_net_open_fail_n > 0) && (g_cov_net_open_call_cnt >= g_cov_net_open_fail_n)) {
    return k_ra8_err_hw_error;
  }
  if (g_cov_net_open_huge) {
    *out_len = k_cov_huge_len;
    return k_ra8_ok;
  }
  if (strstr(url, "manifest") != nullptr) {
    g_cov_net_payload = (const uint8_t*)g_cov_manifest;
    g_cov_net_total   = (uint32_t)strlen(g_cov_manifest);
  } else {
    g_cov_net_payload = g_cov_image;
    g_cov_net_total   = k_cov_image_size;
  }
  g_cov_net_offset = 0U;
  *out_len         = g_cov_net_total;
  return k_ra8_ok;
}

static inline ra8_err_t cov_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out)
{
  (void)ctx;
  ++g_cov_net_read_call_cnt;
  if ((g_cov_net_read_fail_n > 0) && (g_cov_net_read_call_cnt >= g_cov_net_read_fail_n)) {
    return k_ra8_err_hw_error;
  }
  if (g_cov_net_read_zero_first && (g_cov_net_read_call_cnt == 1)) {
    *out = 0U;
    return k_ra8_ok;
  }
  const uint32_t avail = g_cov_net_total - g_cov_net_offset;
  const uint32_t n     = (cap < avail) ? cap : avail;
  if (n > 0U) {
    (void)memcpy(dst, g_cov_net_payload + g_cov_net_offset, n);
    g_cov_net_offset += n;
  }
  *out = n;
  return k_ra8_ok;
}

static inline ra8_err_t cov_net_close(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Mock crypto interface
 * ============================================================================= */

static inline ra8_err_t cov_sha_init(void* ctx)
{
  (void)ctx;
  ++g_cov_sha_init_call_cnt;
  if ((g_cov_sha_init_fail_n > 0) && (g_cov_sha_init_call_cnt >= g_cov_sha_init_fail_n)) {
    return k_ra8_err_hw_error;
  }
  (void)memset(g_cov_hash, 0, sizeof g_cov_hash);
  return k_ra8_ok;
}

static inline ra8_err_t cov_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  ++g_cov_sha_upd_call_cnt;
  if ((g_cov_sha_upd_fail_n > 0) && (g_cov_sha_upd_call_cnt >= g_cov_sha_upd_fail_n)) {
    return k_ra8_err_hw_error;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    g_cov_hash[i % k_ra8_ota_sha256_bytes] ^= data[i];
  }
  return k_ra8_ok;
}

static inline ra8_err_t cov_sha_final(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes])
{
  (void)ctx;
  (void)memcpy(out, g_cov_hash, k_ra8_ota_sha256_bytes);
  return k_ra8_ok;
}

static inline ra8_err_t cov_ecdsa_verify(void*          ctx,
                                         uint32_t       key,
                                         const uint8_t  digest[32],
                                         const uint8_t* sig,
                                         uint32_t       sig_len)
{
  (void)ctx;
  (void)digest;
  (void)key;
  if ((sig_len != sizeof g_cov_sig) || (memcmp(sig, g_cov_sig, sig_len) != 0)) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Mock flash interface
 * ============================================================================= */

static inline ra8_err_t cov_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)len;
  if (g_cov_flash_erase_fail) {
    return k_ra8_err_hw_error;
  }
  (void)memset(g_cov_bank, k_ota_cov_erased_byte, sizeof g_cov_bank);
  return k_ra8_ok;
}

static inline ra8_err_t
cov_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)src;
  (void)len;
  if (g_cov_flash_prog_fail) {
    return k_ra8_err_hw_error;
  }
  const uint32_t off = addr - (uint32_t)k_cov_bank_addr;
  if ((off + len) <= sizeof g_cov_bank) {
    (void)memcpy(g_cov_bank + off, src, len);
  }
  return k_ra8_ok;
}

static inline ra8_err_t cov_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  if (g_cov_flash_startup_fail) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

static inline ra8_err_t cov_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  if (g_cov_flash_rb_fail) {
    return k_ra8_err_hw_error;
  }
  const uint32_t off = addr - (uint32_t)k_cov_bank_addr;
  if ((off + len) <= sizeof g_cov_bank) {
    (void)memcpy(dst, g_cov_bank + off, len);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Progress callback
 * ============================================================================= */

static inline void cov_on_progress(const ra8_ota_progress_t* p)
{
  (void)p;
  ++g_cov_progress_cnt;
}

/* =============================================================================
 * Fixture helpers
 * ============================================================================= */

/** @brief Reset all fault-injection flags and counters to safe defaults. */
static inline void priv_reset_flags(void)
{
  g_cov_net_open_fail_n     = 0;
  g_cov_net_open_call_cnt   = 0;
  g_cov_net_open_huge       = false;
  g_cov_net_read_fail_n     = 0;
  g_cov_net_read_call_cnt   = 0;
  g_cov_net_read_zero_first = false;
  g_cov_sha_init_fail_n     = 0;
  g_cov_sha_init_call_cnt   = 0;
  g_cov_sha_upd_fail_n      = 0;
  g_cov_sha_upd_call_cnt    = 0;
  g_cov_flash_erase_fail    = false;
  g_cov_flash_prog_fail     = false;
  g_cov_flash_rb_fail       = false;
  g_cov_flash_startup_fail  = false;
  g_cov_progress_cnt        = 0U;
  (void)memset(g_cov_bank, 0, sizeof g_cov_bank);
}

/**
 * @brief Compute an XOR-based pseudo-hash of a byte range.
 *
 * @param[in]  data Input bytes.
 * @param[in]  len  Byte count.
 * @param[out] out  32-byte destination.
 */
static inline void priv_xor_hash(const uint8_t* data, uint32_t len, uint8_t out[32])
{
  (void)memset(out, 0, k_ra8_ota_sha256_bytes);
  for (uint32_t i = 0U; i < len; ++i) {
    out[i % k_ra8_ota_sha256_bytes] ^= data[i];
  }
}

/** @brief Fill the image buffer with a deterministic pattern. */
static inline void priv_make_image(void)
{
  for (uint32_t i = 0U; i < k_cov_image_size; ++i) {
    g_cov_image[i] = (uint8_t)(i ^ k_ota_cov_mocks_i_a5);
  }
}

/** @brief Render a valid manifest JSON into g_cov_manifest. */
static inline void priv_make_manifest(void)
{
  priv_xor_hash(g_cov_image, k_cov_image_size, g_cov_expected_hash);
  for (uint32_t i = 0U; i < sizeof g_cov_sig; ++i) {
    g_cov_sig[i] = (uint8_t)(k_ota_cov_mocks_sig_seed + i);
  }
  static const char nibble[]                                    = "0123456789abcdef";
  char              hex_sha[(2U * k_ra8_ota_sha256_bytes) + 1U] = {};
  for (uint32_t i = 0U; i < k_ra8_ota_sha256_bytes; ++i) {
    hex_sha[(size_t)2U * i] = nibble[g_cov_expected_hash[i] >> 4U];
    hex_sha[(2U * i) + 1U]  = nibble[g_cov_expected_hash[i] & k_ota_cov_mocks_nibble_mask];
  }
  char hex_sig[(2U * sizeof g_cov_sig) + 1U] = {};
  for (uint32_t i = 0U; i < sizeof g_cov_sig; ++i) {
    hex_sig[(size_t)2U * i] = nibble[g_cov_sig[i] >> 4U];
    hex_sig[(2U * i) + 1U]  = nibble[g_cov_sig[i] & k_ota_cov_mocks_nibble_mask];
  }
  (void)snprintf(g_cov_manifest,
                 sizeof g_cov_manifest,
                 "{ \"version\": \"2.0.0\", \"url\": \"https://test.example/img\","
                 " \"size\": %u, \"sha256\": \"%s\", \"signature\": \"%s\" }",
                 (unsigned)k_cov_image_size,
                 hex_sha,
                 hex_sig);
}

/**
 * @brief Build a minimal valid configuration.
 *
 * @param[in] with_progress When true, wire cov_on_progress into the cfg.
 * @return A fully populated ra8_ota_cfg_t.
 */
static inline ra8_ota_cfg_t priv_make_cfg(bool with_progress)
{
  ra8_ota_cfg_t cfg = {};
  (void)snprintf(cfg.manifest_url, k_ra8_ota_url_max_bytes, "https://test.example/manifest.json");
  cfg.pubkey_handle             = k_cov_pubkey;
  cfg.on_progress               = with_progress ? cov_on_progress : nullptr;
  cfg.run_as_thread             = false;
  cfg.net.open                  = cov_net_open;
  cfg.net.read                  = cov_net_read;
  cfg.net.close                 = cov_net_close;
  cfg.crypto.sha256_init        = cov_sha_init;
  cfg.crypto.sha256_update      = cov_sha_update;
  cfg.crypto.sha256_final       = cov_sha_final;
  cfg.crypto.ecdsa_verify       = cov_ecdsa_verify;
  cfg.flash.erase               = cov_flash_erase;
  cfg.flash.program             = cov_flash_program;
  cfg.flash.set_startup         = cov_flash_set_startup;
  cfg.flash.readback            = cov_flash_readback;
  cfg.flash.inactive_bank_addr  = k_cov_bank_addr;
  cfg.flash.bank_size_bytes     = k_cov_bank_size;
  cfg.flash.inactive_bank_index = k_cov_bank_index;
  return cfg;
}
