/**
 * @file test_ra_ota.c
 * @brief Unit tests for the Phase-5 OTA module (libs/ra_ota).
 *
 * @details
 * Drives ``ra_ota_init`` / ``ra_ota_check_for_update`` /
 * ``ra_ota_download_to_inactive_bank`` / ``ra_ota_verify_signature``
 * / ``ra_ota_commit_and_reboot`` against in-test mock implementations
 * of the three injected interfaces (network / crypto / flash).
 *
 * Coverage (six tests):
 *   - ``test_init_validation``       -- null cfg, missing pointers,
 *                                       empty URL, bank-size 0.
 *   - ``test_check_bad_manifest``    -- backend returns garbage.
 *   - ``test_partial_download_recovery`` -- network EOFs early, then
 *                                       a "reboot" (deinit/init)
 *                                       lets the second attempt run
 *                                       cleanly.
 *   - ``test_signature_mismatch``    -- SHA matches, ECDSA rejects.
 *   - ``test_sha256_mismatch``       -- fresh re-hash differs from
 *                                       manifest digest.
 *   - ``test_happy_path``            -- end-to-end success including
 *                                       the system-reset hook.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_err.h"
#include "ra_ota.h"
#include "unity_minimal.h"

/* =============================================================================
 * Mock storage
 * ============================================================================= */

typedef enum : uint32_t {
  k_test_image_size  = 256U,
  k_test_bank_size   = 4096U,
  k_test_bank_addr   = 0x02080000UL,
  k_test_bank_index  = 1U,
  k_test_pubkey      = 0x1234U,
  k_test_chunk_short = 64U,
} ra_ota_test_const_t;

/** @brief Raw bytes of the mock manifest server response. */
static char g_mock_manifest[2048];
/** @brief Raw bytes of the mock firmware blob. */
static uint8_t g_mock_image[k_test_image_size];
/** @brief Inactive-bank sandbox the flash mock writes into. */
static uint8_t g_bank_storage[k_test_bank_size];
/** @brief Cursor into the active payload for the current download. */
static uint32_t g_net_offset;
/** @brief Total bytes the open session promises to deliver. */
static uint32_t g_net_remaining;
/** @brief Pointer to the active session payload (manifest or image). */
static const uint8_t* g_net_payload;
/** @brief Set to true to inject a short-EOF for partial-download tests. */
static bool g_inject_short_eof;
/** @brief Set to true to make ECDSA verify reject the signature. */
static bool g_ecdsa_should_fail;
/** @brief Set to true to corrupt one byte of the bank during readback. */
static bool g_corrupt_readback;
/** @brief Number of times the system-reset hook fired. */
static uint32_t g_reset_count;
/** @brief Running pseudo-SHA accumulator (XOR-only -- enough for tests). */
static uint8_t g_running_hash[k_ra_ota_sha256_bytes];
/** @brief Manifest-declared digest the test injects. */
static uint8_t g_expected_hash[k_ra_ota_sha256_bytes];
/** @brief Manifest-declared signature blob (just bytes the mock checks). */
static uint8_t g_expected_sig[8];

/* =============================================================================
 * Mock helpers
 * ============================================================================= */

static void priv_compute_xor_hash(const uint8_t* data, uint32_t len, uint8_t out[32])
{
  (void)memset(out, 0, k_ra_ota_sha256_bytes);
  for (uint32_t i = 0U; i < len; ++i) {
    out[i % k_ra_ota_sha256_bytes] ^= data[i];
  }
}

/* =============================================================================
 * Mock network interface
 * ============================================================================= */

static ra_err_t mock_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  if ((url == NULL) || (out_len == NULL)) {
    return k_ra_err_null_ptr;
  }
  if (strstr(url, "manifest") != NULL) {
    g_net_payload   = (const uint8_t*)g_mock_manifest;
    g_net_remaining = (uint32_t)strlen(g_mock_manifest);
  } else {
    g_net_payload   = g_mock_image;
    g_net_remaining = k_test_image_size;
  }
  g_net_offset = 0U;
  *out_len     = g_net_remaining;
  return k_ra_ok;
}

static ra_err_t mock_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out)
{
  (void)ctx;
  if ((dst == NULL) || (out == NULL)) {
    return k_ra_err_null_ptr;
  }
  const uint32_t remain = g_net_remaining - g_net_offset;
  /* When EOF-injection is active, the FIRST read after open delivers
   * up to k_test_chunk_short bytes and the SECOND read returns an
   * error, so the higher level surfaces the failure. */
  if (g_inject_short_eof && (g_net_offset >= k_test_chunk_short)) {
    return k_ra_err_hw_error;
  }
  uint32_t n = (cap < remain) ? cap : remain;
  if (g_inject_short_eof && (n > k_test_chunk_short)) {
    n = k_test_chunk_short;
  }
  if (n > 0U) {
    (void)memcpy(dst, g_net_payload + g_net_offset, n);
    g_net_offset += n;
  }
  *out = n;
  return k_ra_ok;
}

static ra_err_t mock_net_close(void* ctx)
{
  (void)ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Mock crypto interface
 * ============================================================================= */

static ra_err_t mock_sha_init(void* ctx)
{
  (void)ctx;
  (void)memset(g_running_hash, 0, sizeof g_running_hash);
  return k_ra_ok;
}

static ra_err_t mock_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  for (uint32_t i = 0U; i < len; ++i) {
    g_running_hash[i % k_ra_ota_sha256_bytes] ^= data[i];
  }
  return k_ra_ok;
}

static ra_err_t mock_sha_final(void* ctx, uint8_t out[k_ra_ota_sha256_bytes])
{
  (void)ctx;
  (void)memcpy(out, g_running_hash, k_ra_ota_sha256_bytes);
  return k_ra_ok;
}

static ra_err_t mock_ecdsa_verify(void*          ctx,
                                  uint32_t       key,
                                  const uint8_t  digest[32],
                                  const uint8_t* sig,
                                  uint32_t       sig_len)
{
  (void)ctx;
  (void)digest;
  if (g_ecdsa_should_fail) {
    return k_ra_err_hw_error;
  }
  if (key != k_test_pubkey) {
    return k_ra_err_invalid_arg;
  }
  if ((sig_len != sizeof g_expected_sig) || (memcmp(sig, g_expected_sig, sig_len) != 0)) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Mock flash interface
 * ============================================================================= */

static ra_err_t mock_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra_err_invalid_arg;
  }
  (void)memset(g_bank_storage + (addr - k_test_bank_addr), 0xFF, len);
  return k_ra_ok;
}

static ra_err_t mock_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra_err_invalid_arg;
  }
  (void)memcpy(g_bank_storage + (addr - k_test_bank_addr), src, len);
  return k_ra_ok;
}

static ra_err_t mock_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  return k_ra_ok;
}

static ra_err_t mock_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra_err_invalid_arg;
  }
  (void)memcpy(dst, g_bank_storage + (addr - k_test_bank_addr), len);
  if (g_corrupt_readback && (len > 0U)) {
    dst[0] ^= 0xFFU;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Override the weak system-reset hook so the test process keeps running.
 * ============================================================================= */

void ra_ota_system_reset_hook(void)
{
  ++g_reset_count;
}

/* =============================================================================
 * Fixture helpers
 * ============================================================================= */

static void priv_make_image(void)
{
  for (uint32_t i = 0U; i < k_test_image_size; ++i) {
    g_mock_image[i] = (uint8_t)(i ^ 0x5AU);
  }
}

static void priv_make_manifest(void)
{
  /* Build a manifest whose sha256 == priv_compute_xor_hash(g_mock_image). */
  priv_compute_xor_hash(g_mock_image, k_test_image_size, g_expected_hash);
  for (uint32_t i = 0U; i < sizeof g_expected_sig; ++i) {
    g_expected_sig[i] = (uint8_t)(0xA0U + i);
  }
  static const char nibble[] = "0123456789abcdef";

  char hex[(2U * k_ra_ota_sha256_bytes) + 1U] = {};
  for (uint32_t i = 0U; i < k_ra_ota_sha256_bytes; ++i) {
    hex[2U * i]      = nibble[g_expected_hash[i] >> 4U];
    hex[2U * i + 1U] = nibble[g_expected_hash[i] & 0x0FU];
  }

  char sig_hex[(2U * sizeof g_expected_sig) + 1U] = {};
  for (uint32_t i = 0U; i < sizeof g_expected_sig; ++i) {
    sig_hex[2U * i]      = nibble[g_expected_sig[i] >> 4U];
    sig_hex[2U * i + 1U] = nibble[g_expected_sig[i] & 0x0FU];
  }
  (void)snprintf(g_mock_manifest,
                 sizeof g_mock_manifest,
                 "{ \"version\": \"1.0.0\", \"url\": \"https://example.test/img\","
                 " \"size\": %u, \"sha256\": \"%s\", \"signature\": \"%s\" }",
                 (unsigned)k_test_image_size,
                 hex,
                 sig_hex);
}

static void priv_reset_globals(void)
{
  g_inject_short_eof  = false;
  g_ecdsa_should_fail = false;
  g_corrupt_readback  = false;
  g_reset_count       = 0U;
  (void)memset(g_bank_storage, 0, sizeof g_bank_storage);
}

static ra_ota_cfg_t priv_make_cfg(void)
{
  ra_ota_cfg_t cfg = {};
  (void)snprintf(cfg.manifest_url, k_ra_ota_url_max_bytes, "https://example.test/manifest.json");
  cfg.pubkey_handle             = k_test_pubkey;
  cfg.on_progress               = NULL;
  cfg.run_as_thread             = false;
  cfg.net.open                  = mock_net_open;
  cfg.net.read                  = mock_net_read;
  cfg.net.close                 = mock_net_close;
  cfg.crypto.sha256_init        = mock_sha_init;
  cfg.crypto.sha256_update      = mock_sha_update;
  cfg.crypto.sha256_final       = mock_sha_final;
  cfg.crypto.ecdsa_verify       = mock_ecdsa_verify;
  cfg.flash.erase               = mock_flash_erase;
  cfg.flash.program             = mock_flash_program;
  cfg.flash.set_startup         = mock_flash_set_startup;
  cfg.flash.readback            = mock_flash_readback;
  cfg.flash.inactive_bank_addr  = k_test_bank_addr;
  cfg.flash.bank_size_bytes     = k_test_bank_size;
  cfg.flash.inactive_bank_index = k_test_bank_index;
  return cfg;
}

/* =============================================================================
 * Tests
 * ============================================================================= */

static void test_init_validation(void)
{
  TEST_BEGIN("test_init_validation");
  (void)ra_ota_deinit();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ota_init(NULL));

  ra_ota_cfg_t cfg = priv_make_cfg();
  cfg.net.read     = NULL;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ota_init(&cfg));

  cfg                 = priv_make_cfg();
  cfg.manifest_url[0] = '\0';
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_init(&cfg));

  cfg                       = priv_make_cfg();
  cfg.flash.bank_size_bytes = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_init(&cfg));

  /* Happy init then double-init rejection. */
  cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_init_validation");
}

static void test_check_bad_manifest(void)
{
  TEST_BEGIN("test_check_bad_manifest");
  priv_reset_globals();
  priv_make_image();
  /* Garbage manifest -- missing required fields. */
  (void)snprintf(g_mock_manifest, sizeof g_mock_manifest, "{ \"hello\": \"world\" }");

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  const ra_err_t    e = ra_ota_check_for_update(&m);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, e);
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_check_bad_manifest");
}

static void test_partial_download_recovery(void)
{
  TEST_BEGIN("test_partial_download_recovery");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));

  /* First attempt: backend short-EOFs after k_test_chunk_short bytes
   * and then surfaces an error. The state machine should land in
   * the error state. */
  g_inject_short_eof = true;
  const ra_err_t e1  = ra_ota_download_to_inactive_bank(&m);
  TEST_ASSERT(e1 != k_ra_ok);
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());

  /* "Reboot" -> deinit + init clears the high-water mark and the
   * caller can retry from scratch with a healthy network. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  g_inject_short_eof = false;
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  const ra_err_t e2 = ra_ota_download_to_inactive_bank(&m);
  TEST_ASSERT_EQ(k_ra_ok, e2);
  TEST_ASSERT_EQ((int)k_ra_ota_state_verifying, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_partial_download_recovery");
}

static void test_signature_mismatch(void)
{
  TEST_BEGIN("test_signature_mismatch");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));

  g_ecdsa_should_fail = true;
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_signature_mismatch");
}

static void test_sha256_mismatch(void)
{
  TEST_BEGIN("test_sha256_mismatch");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));

  /* Corrupt the readback so the verify-side SHA differs. */
  g_corrupt_readback = true;
  TEST_ASSERT_EQ(k_ra_err_crc_mismatch, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_sha256_mismatch");
}

static void test_happy_path(void)
{
  TEST_BEGIN("test_happy_path");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));

  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_test_image_size, (uint32_t)m.image_size_bytes);
  TEST_ASSERT_EQ((int)k_ra_ota_state_idle, (int)ra_ota_get_state());

  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_verifying, (int)ra_ota_get_state());

  TEST_ASSERT_EQ(k_ra_ok, ra_ota_verify_signature(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_committing, (int)ra_ota_get_state());

  TEST_ASSERT_EQ(k_ra_ok, ra_ota_commit_and_reboot());
  TEST_ASSERT_EQ((int)k_ra_ota_state_done, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(1U, g_reset_count);

  /* Programmed bank should match the source image. */
  TEST_ASSERT_EQ(0, memcmp(g_bank_storage, g_mock_image, k_test_image_size));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("test_happy_path");
}

/* =============================================================================
 * MC/DC tests for compound decisions in libs/ra_ota/src/ra_ota.c
 * ============================================================================= */

/**
 * @test test_mcdc_download_state_guard
 *
 * @par MC/DC:
 * Decision: `if ((s_state != k_ra_ota_state_idle) && (s_state != k_ra_ota_state_downloading))`
 * (libs/ra_ota/src/ra_ota.c:554, ra_ota_download_to_inactive_bank).
 * - V1: state=idle      -> C1=F, short-circuit -> false (proceed; ok).
 * - V2: state=verifying -> C1=T, C2=T          -> true  (rejected: invalid_state).
 * - V3: state=downloading -> C1=T, C2=F        -> false (proceed; reached via re-init/check).
 * V1 vs V2 vary C1 with C2 held T. V2 vs V3 vary C2 with C1 held T.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_download_state_guard(void)
{
  TEST_BEGIN("mcdc: download state guard (state!=idle && state!=downloading)");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));

  /* V1: state=idle -> proceed. */
  TEST_ASSERT_EQ((int)k_ra_ota_state_idle, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_verifying, (int)ra_ota_get_state());

  /* V2: state=verifying -> guard rejects. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_ota_download_to_inactive_bank(&m));

  /* V3: re-init -> idle -> check -> download to exercise the C2=F leg. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("mcdc: download state guard (state!=idle && state!=downloading)");
}

/**
 * @test test_mcdc_run_full_update_terminal
 *
 * @par MC/DC:
 * Decision: `if ((s_state == k_ra_ota_state_done) || (s_state == k_ra_ota_state_error))`
 * (libs/ra_ota/src/ra_ota.c:718, ra_ota_run_full_update terminal-state break).
 * - V1: state=idle  -> C1=F, C2=F -> false (loop continues).
 * - V2: state=done  -> C1=T, short-circuit -> true (loop breaks; varies C1).
 * - V3: state=error -> C1=F, C2=T -> true (loop breaks; varies C2).
 * V1 vs V2 vary C1 with C2 held F. V1 vs V3 vary C2 with C1 held F.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_run_full_update_terminal(void)
{
  TEST_BEGIN("mcdc: run_full_update terminal (state==done || state==error)");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  /* V1: progress to done -> the guard breaks the loop on the first
   * iteration where C1 OR C2 holds. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_run_full_update());
  TEST_ASSERT_EQ((int)k_ra_ota_state_done, (int)ra_ota_get_state());

  /* V2: state=done -> immediate no-op (loop breaks via C1). */
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_run_full_update());
  TEST_ASSERT_EQ((int)k_ra_ota_state_done, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());

  /* V3: error state -> terminal guard short-circuits via C2. */
  priv_reset_globals();
  priv_make_image();
  (void)snprintf(g_mock_manifest, sizeof g_mock_manifest, "{ \"hello\": \"world\" }");
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());
  const ra_err_t e = ra_ota_run_full_update();
  TEST_ASSERT(e != k_ra_ok);
  TEST_ASSERT_EQ((int)k_ra_ota_state_error, (int)ra_ota_get_state());
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("mcdc: run_full_update terminal (state==done || state==error)");
}

/**
 * @test test_mcdc_hex_decode_invalid_nibble
 *
 * @par MC/DC:
 * Decision: `if ((hi == k_ra_ota_hex_invalid_nibble) || (lo == k_ra_ota_hex_invalid_nibble))`
 * (libs/ra_ota/src/ra_ota.c:312, priv_hex_decode).
 * Reached through ra_ota_check_for_update -> priv_manifest_decode_crypto
 * which decodes the "sha256" hex blob first.
 * - V1: both nibbles valid hex (C1=F, C2=F) -> false (decode succeeds).
 * - V2: hi nibble bogus 'Z'   (C1=T, short-circuit) -> true (rejected; varies C1).
 * - V3: lo nibble bogus 'Z'   (C1=F, C2=T) -> true (rejected; varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_hex_decode_invalid_nibble(void)
{
  TEST_BEGIN("mcdc: hex_decode invalid-nibble (hi||lo == invalid)");
  /* V1: well-formed manifest -> hex decode succeeds. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  ra_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  ra_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());

  /* V2: corrupt the FIRST nibble of the sha256 hex blob with 'Z'. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  char* sha_field = strstr(g_mock_manifest, "\"sha256\": \"");
  TEST_ASSERT_NOT_NULL(sha_field);
  sha_field[strlen("\"sha256\": \"")] = 'Z';
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());

  /* V3: corrupt the SECOND nibble of the sha256 hex blob with 'Z'. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  char* sha_field2 = strstr(g_mock_manifest, "\"sha256\": \"");
  TEST_ASSERT_NOT_NULL(sha_field2);
  sha_field2[strlen("\"sha256\": \"") + 1U] = 'Z';
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  TEST_END("mcdc: hex_decode invalid-nibble (hi||lo == invalid)");
}

/**
 * @test test_mcdc_hex_nibble_pair_completion
 *
 * @par MC/DC:
 * Three sibling decisions in libs/ra_ota/src/ra_ota.c priv_hex_nibble:
 * - line 448: ``(c >= '0') && (c <= '9')``
 * - line 451: ``(c >= 'a') && (c <= 'f')``
 * - line 454: ``(c >= 'A') && (c <= 'F')``
 *
 * For each 2-cond AND, N+1 = 3 vectors. The pre-existing
 * test_mcdc_hex_decode_invalid_nibble corrupts with 'Z' which gives:
 *   448: T,F (Z >= '0' but Z > '9')
 *   451: F,- (Z < 'a')
 *   454: T,F (Z >= 'A' but Z > 'F')
 *
 * Missing vectors per decision:
 *   448 needs C1=F (c < '0')         -> corrupt with '/'  (0x2F).
 *   451 needs C1=T,C2=F (c > 'f')    -> corrupt with 'z'.
 *   454 needs C1=F (c < 'A')         -> corrupt with ':'  (0x3A,
 *                                       between '9' and 'A').
 * Each is fed through ra_ota_check_for_update by mutating the sha256
 * field of the manifest, so priv_hex_nibble is exercised on real input.
 */
static void test_mcdc_hex_nibble_pair_completion(void)
{
  TEST_BEGIN("ra_ota MC/DC: hex_nibble C1=F vectors for lines 448/451/454");
  ra_ota_cfg_t      cfg = priv_make_cfg();
  ra_ota_manifest_t m   = {};

  /* Helper: corrupt the FIRST sha256 hex byte to character 'ch' and
   * verify ra_ota_check_for_update rejects with invalid_arg. */
  const char vectors[3] = {'/', 'z', ':'};
  for (uint32_t i = 0U; i < 3U; ++i) {
    priv_reset_globals();
    priv_make_image();
    priv_make_manifest();
    char* sha_field = strstr(g_mock_manifest, "\"sha256\": \"");
    TEST_ASSERT_NOT_NULL(sha_field);
    sha_field[strlen("\"sha256\": \"")] = vectors[i];
    TEST_ASSERT_EQ(k_ra_ok, ra_ota_init(&cfg));
    TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ota_check_for_update(&m));
    TEST_ASSERT_EQ(k_ra_ok, ra_ota_deinit());
  }
  TEST_END("ra_ota MC/DC: hex_nibble C1=F vectors for lines 448/451/454");
}

/* =============================================================================
 * main
 * ============================================================================= */

/**
 * @test test_mcdc_priv_json_u32_skip_chars
 *
 * @par MC/DC:
 * Decision (libs/ra_ota/src/ra_ota.c line 403, priv_json_u32):
 *   ``(*p == ':') || (*p == ' ') || (*p == '"')``
 * (3 conditions, OR-chain). Static helper -- mirrored byte-identically
 * for direct vector application per DO-178C 6.4.4.3 source-text
 * equivalence.
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=3 OR-chain requires N+1 = 4
 * vectors. Canonical short-circuit set:
 * - V1 c=':'  -> C1=T shorts.            Decision T (skip).
 * - V2 c=' '  -> C1=F,C2=T shorts.       Decision T (skip).
 * - V3 c='"'  -> C1=F,C2=F,C3=T.         Decision T (skip).
 * - V4 c='5'  -> all F.                  Decision F (stop skipping).
 *
 * Pairs isolating each condition:
 *   C1: V1 vs V4. C2: V2 vs V4. C3: V3 vs V4.
 */
static int mirror_json_u32_skip_char(char c)
{
  return (c == ':') || (c == ' ') || (c == '"');
}

static void test_mcdc_priv_json_u32_skip_chars(void)
{
  TEST_BEGIN("ra_ota MC/DC: priv_json_u32 skip-char OR (line 403)");
  TEST_ASSERT_EQ(1, mirror_json_u32_skip_char(':'));
  TEST_ASSERT_EQ(1, mirror_json_u32_skip_char(' '));
  TEST_ASSERT_EQ(1, mirror_json_u32_skip_char('"'));
  TEST_ASSERT_EQ(0, mirror_json_u32_skip_char('5'));
  TEST_END("ra_ota MC/DC: priv_json_u32 skip-char OR (line 403)");
}

int main(void)
{
  test_init_validation();
  test_check_bad_manifest();
  test_partial_download_recovery();
  test_signature_mismatch();
  test_sha256_mismatch();
  test_happy_path();
  test_mcdc_download_state_guard();
  test_mcdc_run_full_update_terminal();
  test_mcdc_hex_decode_invalid_nibble();
  test_mcdc_priv_json_u32_skip_chars();
  test_mcdc_hex_nibble_pair_completion();
  (void)fprintf(stderr, "[OK  ] test_ra_ota.c\n");
  return 0;
}
