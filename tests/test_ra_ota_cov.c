/**
 * @file test_ra_ota_cov.c
 * @brief Coverage-gap tests for libs/ra_ota/src/ra_ota.c.
 *
 * @details
 * Companion to test_ra_ota.c. Targets the branches left uncovered by the
 * primary test suite: the on_progress callback path, network/crypto/flash
 * fault-injection paths, not-initialized guards, wrong-state guards,
 * and the priv_step_dispatch terminal-state arms.
 *
 * NOTE: ra_ota_system_reset_hook is NOT overridden in this file so the
 * weak definition in ra_ota.c fires when ra_ota_commit_and_reboot is
 * called, covering the otherwise-dead weak body (lines 905/908).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_err.h"
#include "ra_ota.h"
#include "ra_ota_internal.h"
#include "unity_minimal.h"

/* =============================================================================
 * Mock constants
 * ============================================================================= */

typedef enum : uint32_t {
  k_cov_image_size = 256U,
  k_cov_bank_size  = 4096U,
  k_cov_bank_addr  = 0x02080000UL,
  k_cov_bank_index = 1U,
  k_cov_pubkey     = 0xBEEFU,
  k_cov_huge_len   = 9999U,
} ra_ota_cov_const_t;

/* =============================================================================
 * Mock state
 * ============================================================================= */

/** @brief Manifest JSON rendered for a valid fixture. */
static char g_cov_manifest[2048];
/** @brief Raw bytes of the mock firmware image. */
static uint8_t g_cov_image[k_cov_image_size];
/** @brief Inactive-bank sandbox. */
static uint8_t g_cov_bank[k_cov_bank_size];
/** @brief Running XOR pseudo-hash accumulator. */
static uint8_t g_cov_hash[k_ra_ota_sha256_bytes];
/** @brief Expected hash stored in the manifest. */
static uint8_t g_cov_expected_hash[k_ra_ota_sha256_bytes];
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

static ra_err_t cov_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  (void)url;
  ++g_cov_net_open_call_cnt;
  if ((g_cov_net_open_fail_n > 0) && (g_cov_net_open_call_cnt >= g_cov_net_open_fail_n)) {
    return k_ra_err_hw_error;
  }
  if (g_cov_net_open_huge) {
    *out_len = k_cov_huge_len;
    return k_ra_ok;
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
  return k_ra_ok;
}

static ra_err_t cov_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out)
{
  (void)ctx;
  ++g_cov_net_read_call_cnt;
  if ((g_cov_net_read_fail_n > 0) && (g_cov_net_read_call_cnt >= g_cov_net_read_fail_n)) {
    return k_ra_err_hw_error;
  }
  if (g_cov_net_read_zero_first && (g_cov_net_read_call_cnt == 1)) {
    *out = 0U;
    return k_ra_ok;
  }
  const uint32_t avail = g_cov_net_total - g_cov_net_offset;
  const uint32_t n     = (cap < avail) ? cap : avail;
  if (n > 0U) {
    (void)memcpy(dst, g_cov_net_payload + g_cov_net_offset, n);
    g_cov_net_offset += n;
  }
  *out = n;
  return k_ra_ok;
}

static ra_err_t cov_net_close(void* ctx)
{
  (void)ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Mock crypto interface
 * ============================================================================= */

static ra_err_t cov_sha_init(void* ctx)
{
  (void)ctx;
  ++g_cov_sha_init_call_cnt;
  if ((g_cov_sha_init_fail_n > 0) && (g_cov_sha_init_call_cnt >= g_cov_sha_init_fail_n)) {
    return k_ra_err_hw_error;
  }
  (void)memset(g_cov_hash, 0, sizeof g_cov_hash);
  return k_ra_ok;
}

static ra_err_t cov_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  ++g_cov_sha_upd_call_cnt;
  if ((g_cov_sha_upd_fail_n > 0) && (g_cov_sha_upd_call_cnt >= g_cov_sha_upd_fail_n)) {
    return k_ra_err_hw_error;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    g_cov_hash[i % k_ra_ota_sha256_bytes] ^= data[i];
  }
  return k_ra_ok;
}

static ra_err_t cov_sha_final(void* ctx, uint8_t out[k_ra_ota_sha256_bytes])
{
  (void)ctx;
  (void)memcpy(out, g_cov_hash, k_ra_ota_sha256_bytes);
  return k_ra_ok;
}

static ra_err_t cov_ecdsa_verify(void*          ctx,
                                 uint32_t       key,
                                 const uint8_t  digest[32],
                                 const uint8_t* sig,
                                 uint32_t       sig_len)
{
  (void)ctx;
  (void)digest;
  (void)key;
  if ((sig_len != sizeof g_cov_sig) || (memcmp(sig, g_cov_sig, sig_len) != 0)) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Mock flash interface
 * ============================================================================= */

static ra_err_t cov_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)len;
  if (g_cov_flash_erase_fail) {
    return k_ra_err_hw_error;
  }
  (void)memset(g_cov_bank, 0xFF, sizeof g_cov_bank);
  return k_ra_ok;
}

static ra_err_t cov_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)src;
  (void)len;
  if (g_cov_flash_prog_fail) {
    return k_ra_err_hw_error;
  }
  const uint32_t off = addr - (uint32_t)k_cov_bank_addr;
  if ((off + len) <= sizeof g_cov_bank) {
    (void)memcpy(g_cov_bank + off, src, len);
  }
  return k_ra_ok;
}

static ra_err_t cov_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  if (g_cov_flash_startup_fail) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

static ra_err_t cov_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  if (g_cov_flash_rb_fail) {
    return k_ra_err_hw_error;
  }
  const uint32_t off = addr - (uint32_t)k_cov_bank_addr;
  if ((off + len) <= sizeof g_cov_bank) {
    (void)memcpy(dst, g_cov_bank + off, len);
  }
  return k_ra_ok;
}

/* =============================================================================
 * Progress callback
 * ============================================================================= */

static void cov_on_progress(const ra_ota_progress_t* p)
{
  (void)p;
  ++g_cov_progress_cnt;
}

/* =============================================================================
 * Fixture helpers
 * ============================================================================= */

/** @brief Reset all fault-injection flags and counters to safe defaults. */
static void priv_reset_flags(void)
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
static void priv_xor_hash(const uint8_t* data, uint32_t len, uint8_t out[32])
{
  (void)memset(out, 0, k_ra_ota_sha256_bytes);
  for (uint32_t i = 0U; i < len; ++i) {
    out[i % k_ra_ota_sha256_bytes] ^= data[i];
  }
}

/** @brief Fill the image buffer with a deterministic pattern. */
static void priv_make_image(void)
{
  for (uint32_t i = 0U; i < k_cov_image_size; ++i) {
    g_cov_image[i] = (uint8_t)(i ^ 0xA5U);
  }
}

/** @brief Render a valid manifest JSON into g_cov_manifest. */
static void priv_make_manifest(void)
{
  priv_xor_hash(g_cov_image, k_cov_image_size, g_cov_expected_hash);
  for (uint32_t i = 0U; i < sizeof g_cov_sig; ++i) {
    g_cov_sig[i] = (uint8_t)(0xC0U + i);
  }
  static const char nibble[]                                   = "0123456789abcdef";
  char              hex_sha[(2U * k_ra_ota_sha256_bytes) + 1U] = {};
  for (uint32_t i = 0U; i < k_ra_ota_sha256_bytes; ++i) {
    hex_sha[2U * i]      = nibble[g_cov_expected_hash[i] >> 4U];
    hex_sha[2U * i + 1U] = nibble[g_cov_expected_hash[i] & 0x0FU];
  }
  char hex_sig[(2U * sizeof g_cov_sig) + 1U] = {};
  for (uint32_t i = 0U; i < sizeof g_cov_sig; ++i) {
    hex_sig[2U * i]      = nibble[g_cov_sig[i] >> 4U];
    hex_sig[2U * i + 1U] = nibble[g_cov_sig[i] & 0x0FU];
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
 * @return A fully populated ra_ota_cfg_t.
 */
static ra_ota_cfg_t priv_make_cfg(bool with_progress)
{
  ra_ota_cfg_t cfg = {};
  (void)snprintf(cfg.manifest_url, k_ra_ota_url_max_bytes, "https://test.example/manifest.json");
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

/* =============================================================================
 * Tests
 * ============================================================================= */

/**
 * @brief Verify on_progress callback fires and receives a valid snapshot.
 *
 * @details Registers a callback, drives check_for_update (which calls
 * priv_set_state at least once), then confirms the counter incremented.
 * Covers ra_ota.c lines 101-108 (the on_progress != nullptr branch).
 *
 * @par MC/DC:
 * Decision: `if (s_cfg.on_progress != nullptr)` (1 condition).
 * - This test: on_progress != nullptr -> true (fires callback).
 * - test_ra_ota.c happy-path uses nullptr, covering the false arm.
 */
static void test_progress_callback_fires(void)
{
  TEST_BEGIN("cov: on_progress callback fires");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(true);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  /* priv_set_state was called (at minimum: once for checking, once for idle). */
  TEST_ASSERT(g_cov_progress_cnt >= 2U);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: on_progress callback fires");
}

/**
 * @brief Verify progress callback fires with bytes_total == 0 before manifest cached.
 *
 * @details On the FIRST priv_set_state call (state -> checking), s_manifest_valid
 * is still false so the ternary on line 104 evaluates the false arm (bytes_total=0).
 * After a successful check_for_update, manifest_valid becomes true, so a subsequent
 * priv_set_state call exercises the true arm.
 *
 * @par MC/DC:
 * Decision: `s_manifest_valid ? s_manifest.image_size_bytes : 0U` (1 condition).
 * - False arm: covered during the initial checking-state transition.
 * - True arm: covered during subsequent transitions once manifest is cached.
 */
static void test_progress_bytes_total_both_arms(void)
{
  TEST_BEGIN("cov: progress bytes_total ternary both arms");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(true);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  /* check_for_update: priv_set_state(checking) -> false arm (no manifest yet).
   * Then priv_set_state(idle) -> false arm (manifest_valid still false during
   * the checking transition; true arm fires later when manifest IS cached and
   * download transitions call priv_set_state). */
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  /* Download: priv_set_state(downloading) fires with manifest_valid=true. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT(g_cov_progress_cnt >= 3U);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: progress bytes_total ternary both arms");
}

/**
 * @brief Verify check_for_update returns not-initialized when module is
 *        uninitialized.
 *
 * @details Covers ra_ota.c line 353.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_check_not_initialized(void)
{
  TEST_BEGIN("cov: check_for_update not-initialized guard");
  (void)ra_ota_deinit();
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_check_for_update(&m));
  TEST_END("cov: check_for_update not-initialized guard");
}

/**
 * @brief Verify check_for_update returns invalid-state when SM is not idle.
 *
 * @details Forces an error state via a bad manifest, then calls
 * check_for_update again. Covers ra_ota.c line 357.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_check_wrong_state(void)
{
  TEST_BEGIN("cov: check_for_update invalid-state guard");
  priv_reset_flags();
  priv_make_image();
  (void)snprintf(g_cov_manifest, sizeof g_cov_manifest, "{}");
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  /* First call puts SM into error. */
  TEST_ASSERT(ra_ota_check_for_update(&m) != k_ra_ok);
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  /* Second call hits the state != idle guard. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: check_for_update invalid-state guard");
}

/**
 * @brief Verify net.open failure during manifest fetch propagates correctly.
 *
 * @details Net.open is made to fail on its first call (which is the manifest
 * open). Covers ra_ota.c lines 305, 364, 365.
 *
 * @par MC/DC:
 * (single-condition error-return guards; no compound decisions)
 */
static void test_check_manifest_open_fail(void)
{
  TEST_BEGIN("cov: check_for_update manifest open failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_net_open_fail_n = 1;
  ra_ota_cfg_t cfg      = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: check_for_update manifest open failure");
}

/**
 * @brief Verify that an oversized Content-Length in the manifest response
 *        causes an invalid-size error and closes the connection.
 *
 * @details Covers ra_ota.c lines 308, 309.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_check_manifest_too_large(void)
{
  TEST_BEGIN("cov: manifest content_len > max rejected");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_net_open_huge = true;
  ra_ota_cfg_t cfg    = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: manifest content_len > max rejected");
}

/**
 * @brief Verify that a read error during manifest drain propagates correctly.
 *
 * @details Net.read is made to fail on its first call (inside priv_drain
 * during the manifest fetch). Covers ra_ota.c line 315.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_check_manifest_drain_fail(void)
{
  TEST_BEGIN("cov: manifest drain read failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_net_read_fail_n = 1;
  ra_ota_cfg_t cfg      = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: manifest drain read failure");
}

/**
 * @brief Verify download_to_inactive_bank returns not-initialized when the
 *        module has not been initialized.
 *
 * @details Covers ra_ota.c line 540.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_not_initialized(void)
{
  TEST_BEGIN("cov: download not-initialized guard");
  (void)ra_ota_deinit();
  ra_ota_manifest_t m = {};
  m.image_size_bytes  = k_cov_image_size;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_download_to_inactive_bank(&m));
  TEST_END("cov: download not-initialized guard");
}

/**
 * @brief Verify that image_size_bytes > bank_size_bytes is rejected.
 *
 * @details The manifest image size is set larger than the configured bank.
 * Covers ra_ota.c lines 549, 550.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_image_too_large(void)
{
  TEST_BEGIN("cov: download image_size > bank_size rejected");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  /* Override image_size to exceed the configured bank. */
  m.image_size_bytes = k_cov_bank_size + 1U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download image_size > bank_size rejected");
}

/**
 * @brief Verify that a flash erase failure in priv_prepare_bank propagates.
 *
 * @details Covers ra_ota.c lines 458, 556, 557.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_download_erase_fail(void)
{
  TEST_BEGIN("cov: download flash erase failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_flash_erase_fail = true;
  ra_ota_cfg_t cfg       = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download flash erase failure");
}

/**
 * @brief Verify that net.open failure for the image URL propagates.
 *
 * @details Net.open is made to fail on its second call (the first is the
 * manifest open which must succeed; the second is the image open).
 * Covers ra_ota.c lines 564, 565.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_download_image_open_fail(void)
{
  TEST_BEGIN("cov: download image open failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_net_open_fail_n = 2;
  ra_ota_cfg_t cfg      = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download image open failure");
}

/**
 * @brief Verify that a premature EOF (got==0) in priv_download_chunk
 *        returns k_ra_err_hw_error.
 *
 * @details Net.read returns 0 bytes on its first call during the image
 * download (i.e. after the manifest has already been read). The second
 * net.open is for the image; the reads after that are for the image data.
 * We make the first image read return 0 to hit the got==0 guard.
 * Covers ra_ota.c line 415.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_chunk_eof(void)
{
  TEST_BEGIN("cov: download chunk got==0 hw_error");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  /* check_for_update drains the manifest via multiple reads; record how many. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  const int reads_for_manifest = g_cov_net_read_call_cnt;
  /* The next read call (first for the image) should return 0. */
  g_cov_net_read_zero_first = true;
  g_cov_net_read_call_cnt   = 0;
  /* Map "first call in THIS phase" to the absolute countdown. */
  g_cov_net_read_fail_n = 0;
  /* Reset counters so zero-first fires on read #1 of the image phase. */
  g_cov_net_read_call_cnt = 0;
  /* Re-arm: zero_first fires when call_cnt == 1 on the NEXT open. */
  g_cov_net_open_call_cnt   = 0;
  g_cov_net_read_zero_first = true;
  /* The image net.read call counter is reset each open; reuse zero_first. */
  (void)reads_for_manifest;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download chunk got==0 hw_error");
}

/**
 * @brief Verify sha256_update failure during chunk download propagates.
 *
 * @details sha256_update fails on its first call, which occurs inside
 * priv_download_chunk. Covers ra_ota.c line 419.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_sha_update_fail(void)
{
  TEST_BEGIN("cov: download sha256_update failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  /* sha_init is call #1 (prepare_bank); sha_update is call #1 (first chunk).
   * Make sha_update fail on call #1. */
  g_cov_sha_upd_fail_n   = 1;
  g_cov_sha_upd_call_cnt = 0;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download sha256_update failure");
}

/**
 * @brief Verify flash.program failure during chunk download propagates.
 *
 * @details Covers ra_ota.c line 423.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_program_fail(void)
{
  TEST_BEGIN("cov: download flash.program failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  g_cov_flash_prog_fail = true;
  ra_ota_cfg_t cfg      = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: download flash.program failure");
}

/**
 * @brief Verify verify_signature returns not-initialized when uninitialized.
 *
 * @details Covers ra_ota.c line 666.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_verify_not_initialized(void)
{
  TEST_BEGIN("cov: verify_signature not-initialized guard");
  (void)ra_ota_deinit();
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_verify_signature(&m));
  TEST_END("cov: verify_signature not-initialized guard");
}

/**
 * @brief Verify verify_signature returns invalid-state when SM is not in
 *        verifying state.
 *
 * @details Covers ra_ota.c line 670.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_verify_wrong_state(void)
{
  TEST_BEGIN("cov: verify_signature invalid-state guard");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  /* SM is in idle, not verifying. */
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_idle, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: verify_signature invalid-state guard");
}

/**
 * @brief Verify that sha256_init failure inside priv_rehash_bank propagates.
 *
 * @details sha256_init is made to fail on its SECOND call. The first call is
 * inside priv_prepare_bank (download phase); the second is inside
 * priv_rehash_bank (verify phase). Covers ra_ota.c lines 609, 676, 677.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_verify_rehash_sha_init_fail(void)
{
  TEST_BEGIN("cov: verify rehash sha256_init failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_verifying, ra_ota_get_state());
  /* Now make sha_init fail on the next (second overall) call. */
  g_cov_sha_init_fail_n   = 2;
  g_cov_sha_init_call_cnt = 1;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: verify rehash sha256_init failure");
}

/**
 * @brief Verify that flash.readback failure inside priv_rehash_bank propagates.
 *
 * @details Covers ra_ota.c lines 621, 676, 677.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_verify_rehash_readback_fail(void)
{
  TEST_BEGIN("cov: verify rehash flash.readback failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_verifying, ra_ota_get_state());
  g_cov_flash_rb_fail = true;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: verify rehash flash.readback failure");
}

/**
 * @brief Verify that sha256_update failure during priv_rehash_bank propagates.
 *
 * @details Download uses one sha_update call (256-byte image = 1 chunk).
 * Rehash also uses one sha_update call. Fail on the second overall call to
 * hit the rehash path. Covers ra_ota.c lines 625, 676, 677.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_verify_rehash_sha_update_fail(void)
{
  TEST_BEGIN("cov: verify rehash sha256_update failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_verifying, ra_ota_get_state());
  /* Download consumed 1 sha_update call; fail on the 2nd (rehash). */
  g_cov_sha_upd_fail_n   = 2;
  g_cov_sha_upd_call_cnt = 1;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: verify rehash sha256_update failure");
}

/**
 * @brief Verify commit_and_reboot returns not-initialized when uninitialized.
 *
 * @details Covers ra_ota.c line 725.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_commit_not_initialized(void)
{
  TEST_BEGIN("cov: commit_and_reboot not-initialized guard");
  (void)ra_ota_deinit();
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_commit_and_reboot());
  TEST_END("cov: commit_and_reboot not-initialized guard");
}

/**
 * @brief Verify commit_and_reboot returns invalid-state when SM is not in
 *        committing state.
 *
 * @details Covers ra_ota.c line 728.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_commit_wrong_state(void)
{
  TEST_BEGIN("cov: commit_and_reboot invalid-state guard");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  /* SM is in idle, not committing. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: commit_and_reboot invalid-state guard");
}

/**
 * @brief Verify that flash.set_startup failure in commit_and_reboot propagates.
 *
 * @details Covers ra_ota.c lines 733, 734.
 *
 * @par MC/DC:
 * (single-condition guards; no compound decisions)
 */
static void test_commit_startup_fail(void)
{
  TEST_BEGIN("cov: commit_and_reboot set_startup failure");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_committing, ra_ota_get_state());
  g_cov_flash_startup_fail = true;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: commit_and_reboot set_startup failure");
}

/**
 * @brief Verify ra_ota_run_step returns not-initialized when uninitialized.
 *
 * @details Covers ra_ota.c line 824.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_run_step_not_initialized(void)
{
  TEST_BEGIN("cov: run_step not-initialized guard");
  (void)ra_ota_deinit();
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_run_step());
  TEST_END("cov: run_step not-initialized guard");
}

/**
 * @brief Verify ra_ota_run_full_update returns not-initialized when uninitialized.
 *
 * @details Covers ra_ota.c line 857.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_run_full_not_initialized(void)
{
  TEST_BEGIN("cov: run_full_update not-initialized guard");
  (void)ra_ota_deinit();
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ota_run_full_update());
  TEST_END("cov: run_full_update not-initialized guard");
}

/**
 * @brief Verify that a failing step inside run_full_update is returned.
 *
 * @details Manifest decode fails (bad JSON) so the internal step returns
 * an error which is propagated out. Covers ra_ota.c line 868.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_run_full_step_error(void)
{
  TEST_BEGIN("cov: run_full_update propagates step error");
  priv_reset_flags();
  priv_make_image();
  (void)snprintf(g_cov_manifest, sizeof g_cov_manifest, "{}");
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  const ra_err_t e = ra_ota_run_full_update();
  TEST_ASSERT(e != k_ra_ok);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: run_full_update propagates step error");
}

/**
 * @brief Verify priv_step_dispatch returns k_ra_ok for done and error states.
 *
 * @details Calls ra_ota_run_step directly when the SM is in done/error state.
 * Covers ra_ota.c lines 790, 792 (terminal-state arm of the switch).
 *
 * @par MC/DC:
 * (switch fall-through to a single return; no compound decision)
 */
static void test_run_step_terminal_states(void)
{
  TEST_BEGIN("cov: run_step terminal states done and error");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);

  /* Reach done state via full update. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_run_full_update());
  TEST_ASSERT_EQ(k_ra_ota_state_done, ra_ota_get_state());
  /* Call run_step in done state -> priv_step_dispatch done case -> k_ra_ok. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_run_step());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());

  /* Reach error state via bad manifest. */
  priv_reset_flags();
  (void)snprintf(g_cov_manifest, sizeof g_cov_manifest, "{}");
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT(ra_ota_check_for_update(&m) != k_ra_ok);
  TEST_ASSERT_EQ(k_ra_ota_state_error, ra_ota_get_state());
  /* Call run_step in error state -> priv_step_dispatch error case -> k_ra_ok. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_run_step());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: run_step terminal states done and error");
}

/**
 * @brief Full happy path using the weak ra_ota_system_reset_hook.
 *
 * @details Because this file does NOT define ra_ota_system_reset_hook, the
 * weak definition in ra_ota.c fires when commit_and_reboot is called. This
 * covers ra_ota.c lines 905 and 908 (the weak hook body).
 *
 * @par MC/DC:
 * (no compound decisions; pure path coverage)
 */
static void test_commit_weak_reset_hook(void)
{
  TEST_BEGIN("cov: commit fires weak ra_ota_system_reset_hook");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra_ota_state_committing, ra_ota_get_state());
  /* The weak hook is a no-op -- the call succeeds and returns k_ra_ok. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra_ota_state_done, ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("cov: commit fires weak ra_ota_system_reset_hook");
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(void)
{
  test_progress_callback_fires();
  test_progress_bytes_total_both_arms();
  test_check_not_initialized();
  test_check_wrong_state();
  test_check_manifest_open_fail();
  test_check_manifest_too_large();
  test_check_manifest_drain_fail();
  test_download_not_initialized();
  test_download_image_too_large();
  test_download_erase_fail();
  test_download_image_open_fail();
  test_download_chunk_eof();
  test_download_sha_update_fail();
  test_download_program_fail();
  test_verify_not_initialized();
  test_verify_wrong_state();
  test_verify_rehash_sha_init_fail();
  test_verify_rehash_readback_fail();
  test_verify_rehash_sha_update_fail();
  test_commit_not_initialized();
  test_commit_wrong_state();
  test_commit_startup_fail();
  test_run_step_not_initialized();
  test_run_full_not_initialized();
  test_run_full_step_error();
  test_run_step_terminal_states();
  test_commit_weak_reset_hook();
  (void)fprintf(stderr, "[OK  ] test_ra_ota_cov.c\n");
  return 0;
}
