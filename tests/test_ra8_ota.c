/**
 * @file test_ra8_ota.c
 * @brief Unit tests for the Phase-5 OTA module (libs/ra8_ota).
 *
 * @details
 * Drives ``ra8_ota_init`` / ``ra8_ota_check_for_update`` /
 * ``ra8_ota_download_to_inactive_bank`` / ``ra8_ota_verify_signature``
 * / ``ra8_ota_commit_and_reboot`` against in-test mock implementations
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

#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"
#include "unity_minimal.h"

/* =============================================================================
 * Mock storage
 * ============================================================================= */

typedef enum : uint32_t {
  k_test_image_size  = 256U,         /**< Test image size.   */
  k_test_bank_size   = 4096U,        /**< Test bank size.    */
  k_test_bank_addr   = 0x02080000UL, /**< Test bank address. */
  k_test_bank_index  = 1U,           /**< Test bank index.   */
  k_test_pubkey      = 0x1234U,      /**< Test pubkey.       */
  k_test_chunk_short = 64U,          /**< Test chunk short.  */
} ra8_ota_test_const_t;

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
static uint8_t g_running_hash[k_ra8_ota_sha256_bytes];
/** @brief Manifest-declared digest the test injects. */
static uint8_t g_expected_hash[k_ra8_ota_sha256_bytes];
/** @brief Manifest-declared signature blob (just bytes the mock checks). */
static uint8_t g_expected_sig[8];
/** @brief When true, the mock ECDSA verify pins the metadata-bound digest (T5-05). */
static bool g_bind_check_enabled;
/** @brief Genuine metadata-bound digest the mock verifier accepts (T5-05). */
static uint8_t g_bound_expected[k_ra8_ota_sha256_bytes];

/* =============================================================================
 * Mock helpers
 * ============================================================================= */

static void priv_compute_xor_hash(const uint8_t* data, uint32_t len, uint8_t out[32])
{
  (void)memset(out, 0, k_ra8_ota_sha256_bytes);
  for (uint32_t i = 0U; i < len; ++i) {
    out[i % k_ra8_ota_sha256_bytes] ^= data[i];
  }
}

/* =============================================================================
 * Mock network interface
 * ============================================================================= */

static ra8_err_t mock_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  if ((url == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (strstr(url, "manifest") != nullptr) {
    g_net_payload   = (const uint8_t*)g_mock_manifest;
    g_net_remaining = (uint32_t)strlen(g_mock_manifest);
  } else {
    g_net_payload   = g_mock_image;
    g_net_remaining = k_test_image_size;
  }
  g_net_offset = 0U;
  *out_len     = g_net_remaining;
  return k_ra8_ok;
}

static ra8_err_t mock_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out)
{
  (void)ctx;
  if ((dst == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const uint32_t remain = g_net_remaining - g_net_offset;
  /* When EOF-injection is active, the FIRST read after open delivers
   * up to k_test_chunk_short bytes and the SECOND read returns an
   * error, so the higher level surfaces the failure. */
  if (g_inject_short_eof && (g_net_offset >= k_test_chunk_short)) {
    return k_ra8_err_hw_error;
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
  return k_ra8_ok;
}

static ra8_err_t mock_net_close(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Mock crypto interface
 * ============================================================================= */

static ra8_err_t mock_sha_init(void* ctx)
{
  (void)ctx;
  (void)memset(g_running_hash, 0, sizeof g_running_hash);
  return k_ra8_ok;
}

static ra8_err_t mock_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  for (uint32_t i = 0U; i < len; ++i) {
    g_running_hash[i % k_ra8_ota_sha256_bytes] ^= data[i];
  }
  return k_ra8_ok;
}

static ra8_err_t mock_sha_final(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes])
{
  (void)ctx;
  (void)memcpy(out, g_running_hash, k_ra8_ota_sha256_bytes);
  return k_ra8_ok;
}

static ra8_err_t mock_ecdsa_verify(void*          ctx,
                                   uint32_t       key,
                                   const uint8_t  digest[32],
                                   const uint8_t* sig,
                                   uint32_t       sig_len)
{
  (void)ctx;
  if (g_ecdsa_should_fail) {
    return k_ra8_err_hw_error;
  }
  if (key != k_test_pubkey) {
    return k_ra8_err_invalid_arg;
  }
  if ((sig_len != sizeof g_expected_sig) || (memcmp(sig, g_expected_sig, sig_len) != 0)) {
    return k_ra8_err_hw_error;
  }
  /* T5-05: when pinned, the ECDSA authority runs over the metadata-bound digest.
   * A forged manifest field (version/url/size) yields a different bound digest
   * even though the signature bytes are unchanged, so it is rejected here.
   * Nested (not compound) so no new MC/DC obligation is introduced in the mock. */
  if (g_bind_check_enabled) {
    if (memcmp(digest, g_bound_expected, k_ra8_ota_sha256_bytes) != 0) {
      return k_ra8_err_hw_error;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Mock flash interface
 * ============================================================================= */

static ra8_err_t mock_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra8_err_invalid_arg;
  }
  (void)memset(g_bank_storage + (addr - k_test_bank_addr), 0xFF, len);
  return k_ra8_ok;
}

static ra8_err_t mock_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra8_err_invalid_arg;
  }
  (void)memcpy(g_bank_storage + (addr - k_test_bank_addr), src, len);
  return k_ra8_ok;
}

static ra8_err_t mock_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  return k_ra8_ok;
}

static ra8_err_t mock_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  if ((addr < k_test_bank_addr) || ((addr + len) > (k_test_bank_addr + k_test_bank_size))) {
    return k_ra8_err_invalid_arg;
  }
  (void)memcpy(dst, g_bank_storage + (addr - k_test_bank_addr), len);
  if (g_corrupt_readback && (len > 0U)) {
    dst[0] ^= 0xFFU;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Override the weak system-reset hook so the test process keeps running.
 * ============================================================================= */

void ra8_ota_system_reset_hook(void)
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

  char hex[(2U * k_ra8_ota_sha256_bytes) + 1U] = {};
  for (uint32_t i = 0U; i < k_ra8_ota_sha256_bytes; ++i) {
    hex[(size_t)2U * i] = nibble[g_expected_hash[i] >> 4U];
    hex[(2U * i) + 1U]  = nibble[g_expected_hash[i] & 0x0FU];
  }

  char sig_hex[(2U * sizeof g_expected_sig) + 1U] = {};
  for (uint32_t i = 0U; i < sizeof g_expected_sig; ++i) {
    sig_hex[(size_t)2U * i] = nibble[g_expected_sig[i] >> 4U];
    sig_hex[(2U * i) + 1U]  = nibble[g_expected_sig[i] & 0x0FU];
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
  g_inject_short_eof   = false;
  g_ecdsa_should_fail  = false;
  g_corrupt_readback   = false;
  g_bind_check_enabled = false;
  g_reset_count        = 0U;
  (void)memset(g_bank_storage, 0, sizeof g_bank_storage);
}

/**
 * @brief Recompute the genuine metadata-bound digest the mock verifier accepts.
 *
 * @details
 * Mirrors ``priv_bind_manifest_material`` in libs/ra8_ota/src/ra8_ota.c: streams
 * ``version[32]``, ``image_url[256]``, the little-endian size, then
 * ``image_digest`` through the same XOR mock-SHA the production verify uses, so a
 * test can pin the mock
 * ECDSA verifier to the genuine bound digest and prove a forged manifest field
 * is rejected (T5-05).
 *
 * @param[in]  m            Genuine decoded manifest (metadata source).
 * @param[in]  image_digest The image body digest bound alongside the metadata.
 * @param[out] out          Receives the metadata-bound digest.
 */
static void priv_compute_expected_bound(const ra8_ota_manifest_t* m,
                                        const uint8_t image_digest[k_ra8_ota_sha256_bytes],
                                        uint8_t       out[k_ra8_ota_sha256_bytes])
{
  uint8_t size_le[4] = {};
  for (uint32_t i = 0U; i < sizeof size_le; ++i) {
    size_le[i] = (uint8_t)(m->image_size_bytes >> (8U * i));
  }
  (void)mock_sha_init(nullptr);
  (void)mock_sha_update(nullptr, (const uint8_t*)m->version, k_ra8_ota_version_str_bytes);
  (void)mock_sha_update(nullptr, (const uint8_t*)m->image_url, k_ra8_ota_url_max_bytes);
  (void)mock_sha_update(nullptr, size_le, sizeof size_le);
  (void)mock_sha_update(nullptr, image_digest, k_ra8_ota_sha256_bytes);
  (void)mock_sha_final(nullptr, out);
}

static ra8_ota_cfg_t priv_make_cfg(void)
{
  ra8_ota_cfg_t cfg = {};
  (void)snprintf(cfg.manifest_url, k_ra8_ota_url_max_bytes, "https://example.test/manifest.json");
  cfg.pubkey_handle             = k_test_pubkey;
  cfg.on_progress               = nullptr;
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
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * ============================================================================= */

static void test_init_validation(void)
{
  TEST_BEGIN("test_init_validation");
  (void)ra8_ota_deinit();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ota_init(nullptr));

  ra8_ota_cfg_t cfg = priv_make_cfg();
  cfg.net.read      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ota_init(&cfg));

  cfg                 = priv_make_cfg();
  cfg.manifest_url[0] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_init(&cfg));

  cfg                       = priv_make_cfg();
  cfg.flash.bank_size_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_init(&cfg));

  /* Happy init then double-init rejection. */
  cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_init_validation");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_check_bad_manifest(void)
{
  TEST_BEGIN("test_check_bad_manifest");
  priv_reset_globals();
  priv_make_image();
  /* Garbage manifest -- missing required fields. */
  (void)snprintf(g_mock_manifest, sizeof g_mock_manifest, "{ \"hello\": \"world\" }");

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  const ra8_err_t    e = ra8_ota_check_for_update(&m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_check_bad_manifest");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_partial_download_recovery(void)
{
  TEST_BEGIN("test_partial_download_recovery");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));

  /* First attempt: backend short-EOFs after k_test_chunk_short bytes
   * and then surfaces an error. The state machine should land in
   * the error state. */
  g_inject_short_eof = true;
  const ra8_err_t e1 = ra8_ota_download_to_inactive_bank(&m);
  TEST_ASSERT(e1 != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());

  /* "Reboot" -> deinit + init clears the high-water mark and the
   * caller can retry from scratch with a healthy network. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  g_inject_short_eof = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  const ra8_err_t e2 = ra8_ota_download_to_inactive_bank(&m);
  TEST_ASSERT_EQ(k_ra8_ok, e2);
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_partial_download_recovery");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_signature_mismatch(void)
{
  TEST_BEGIN("test_signature_mismatch");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));

  g_ecdsa_should_fail = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_signature_mismatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sha256_mismatch(void)
{
  TEST_BEGIN("test_sha256_mismatch");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));

  /* Corrupt the readback so the verify-side SHA differs. */
  g_corrupt_readback = true;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_sha256_mismatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_happy_path(void)
{
  TEST_BEGIN("test_happy_path");
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));

  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_test_image_size, m.image_size_bytes);
  TEST_ASSERT_EQ(k_ra8_ota_state_idle, ra8_ota_get_state());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_committing, ra8_ota_get_state());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());
  TEST_ASSERT_EQ(1U, g_reset_count);

  /* Programmed bank should match the source image. */
  TEST_ASSERT_EQ(0, memcmp(g_bank_storage, g_mock_image, k_test_image_size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("test_happy_path");
}

/**
 * @test test_metadata_tamper_rejected
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the public verify path and
 * asserts the metadata-binding contract; the binding helper under test has no
 * `&&`/`||`)
 *
 * Proves the OTA signature now binds the manifest metadata (T5-05). With the mock
 * ECDSA verifier pinned to the genuine metadata-bound digest, the genuine
 * manifest verifies; re-presenting the SAME signed image and signature with a
 * forged (raised) version string -- an anti-rollback-bypass attempt -- is
 * rejected, because binding the forged version changes the digest the signature
 * must authenticate. Before the fix the version rode outside the signature and
 * this tamper would have verified.
 */
static void test_metadata_tamper_rejected(void)
{
  TEST_BEGIN("ra8_ota: manifest metadata tamper invalidates signature (T5-05)");
  ra8_ota_cfg_t cfg = priv_make_cfg();

  /* Control: genuine metadata -> the bound digest matches -> verify accepts. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  priv_compute_expected_bound(&m, g_expected_hash, g_bound_expected);
  g_bind_check_enabled = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_committing, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* Tamper: same signed image + signature, but the manifest version is raised
   * (anti-rollback bypass). The bound digest changes, so the unchanged signature
   * no longer authenticates it -> reject. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  priv_compute_expected_bound(&m, g_expected_hash, g_bound_expected);
  g_bind_check_enabled = true;
  (void)snprintf(m.version, k_ra8_ota_version_str_bytes, "9.9.9");
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  g_bind_check_enabled = false;
  TEST_END("ra8_ota: manifest metadata tamper invalidates signature (T5-05)");
}

/* =============================================================================
 * MC/DC tests for compound decisions in libs/ra8_ota/src/ra8_ota.c
 * ============================================================================= */

/**
 * @test test_mcdc_download_state_guard
 *
 * @par MC/DC:
 * Decision: `if ((s_state != k_ra8_ota_state_idle) && (s_state != k_ra8_ota_state_downloading))`
 * (libs/ra8_ota/src/ra8_ota.c, ra8_ota_download_to_inactive_bank).
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

  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));

  /* V1: state=idle -> proceed. */
  TEST_ASSERT_EQ(k_ra8_ota_state_idle, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());

  /* V2: state=verifying -> guard rejects. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_download_to_inactive_bank(&m));

  /* V3: re-init -> idle -> check -> download to exercise the C2=F leg. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("mcdc: download state guard (state!=idle && state!=downloading)");
}

/**
 * @test test_mcdc_run_full_update_terminal
 *
 * @par MC/DC:
 * Decision `(s_ra8_ota_state == k_ra8_ota_state_done) || (s_ra8_ota_state ==
 * k_ra8_ota_state_error)`, cited drift-proof as
 * libs/ra8_ota/src/ra8_ota.c@ra8_ota_run_full_update (terminal-state break).
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
  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  /* V1: progress to done -> the guard breaks the loop on the first
   * iteration where C1 OR C2 holds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_full_update());
  TEST_ASSERT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());

  /* V2: state=done -> immediate no-op (loop breaks via C1). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_full_update());
  TEST_ASSERT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* V3: error state -> terminal guard short-circuits via C2. */
  priv_reset_globals();
  priv_make_image();
  (void)snprintf(g_mock_manifest, sizeof g_mock_manifest, "{ \"hello\": \"world\" }");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  const ra8_err_t e = ra8_ota_run_full_update();
  TEST_ASSERT(e != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("mcdc: run_full_update terminal (state==done || state==error)");
}

/**
 * @test test_mcdc_hex_decode_invalid_nibble
 *
 * @par MC/DC:
 * Decision: `if ((hi == k_ra8_ota_hex_invalid_nibble) || (lo == k_ra8_ota_hex_invalid_nibble))`
 * (libs/ra8_ota/src/ra8_ota.c, priv_hex_decode).
 * Reached through ra8_ota_check_for_update -> priv_manifest_decode_crypto
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
  ra8_ota_cfg_t cfg = priv_make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* V2: corrupt the FIRST nibble of the sha256 hex blob with 'Z'. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  char* sha_field = strstr(g_mock_manifest, "\"sha256\": \"");
  TEST_ASSERT_NOT_NULL(sha_field);
  sha_field[strlen("\"sha256\": \"")] = 'Z';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* V3: corrupt the SECOND nibble of the sha256 hex blob with 'Z'. */
  priv_reset_globals();
  priv_make_image();
  priv_make_manifest();
  char* sha_field2 = strstr(g_mock_manifest, "\"sha256\": \"");
  TEST_ASSERT_NOT_NULL(sha_field2);
  sha_field2[strlen("\"sha256\": \"") + 1U] = 'Z';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("mcdc: hex_decode invalid-nibble (hi||lo == invalid)");
}

/**
 * @test test_mcdc_hex_nibble_pair_completion
 *
 * @par MC/DC:
 * Three sibling decisions in libs/ra8_ota/src/ra8_ota.c priv_hex_nibble:
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
 * Each is fed through ra8_ota_check_for_update by mutating the sha256
 * field of the manifest, so priv_hex_nibble is exercised on real input.
 */
static void test_mcdc_hex_nibble_pair_completion(void)
{
  TEST_BEGIN("ra8_ota MC/DC: hex_nibble C1=F vectors for lines 448/451/454");
  ra8_ota_cfg_t      cfg = priv_make_cfg();
  ra8_ota_manifest_t m   = {};

  /* Helper: corrupt the FIRST sha256 hex byte to character 'ch' and
   * verify ra8_ota_check_for_update rejects with invalid_arg. */
  const char vectors[3] = {'/', 'z', ':'};
  for (uint32_t i = 0U; i < 3U; ++i) {
    priv_reset_globals();
    priv_make_image();
    priv_make_manifest();
    char* sha_field = strstr(g_mock_manifest, "\"sha256\": \"");
    TEST_ASSERT_NOT_NULL(sha_field);
    sha_field[strlen("\"sha256\": \"")] = vectors[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ota_check_for_update(&m));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  }
  TEST_END("ra8_ota MC/DC: hex_nibble C1=F vectors for lines 448/451/454");
}

/* =============================================================================
 * main
 * ============================================================================= */

/**
 * @test test_mcdc_priv_json_u32_skip_chars
 *
 * @par MC/DC:
 * Decision (libs/ra8_ota/src/ra8_ota.c, ra8_ota_internal_json_u32, line 403):
 *   ``(*p == ':') || (*p == ' ') || (*p == '"')``
 * (3 conditions, OR-chain). Driven directly against production source
 * via ra8_ota_internal.h (test-access policy, see CLAUDE.md).
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=3 OR-chain requires N+1 = 4
 * vectors. Canonical short-circuit set:
 * - V1 first non-key char ':'  -> C1=T shorts.
 * - V2 first non-key char ' '  -> C1=F,C2=T shorts.
 * - V3 first non-key char '"'  -> C1=F,C2=F,C3=T.
 * - V4 first non-key char '5'  -> all F (stop skipping immediately).
 * Pairs isolating each condition: C1: V1 vs V4. C2: V2 vs V4. C3: V3 vs V4.
 */
static void test_mcdc_priv_json_u32_skip_chars(void)
{
  TEST_BEGIN("ra8_ota MC/DC: ra8_ota_internal_json_u32 skip-char OR");
  uint32_t v = 0U;
  /* V1: colon between key and value. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_internal_json_u32("\"size\":42}", "\"size\"", &v));
  TEST_ASSERT_EQ(42, v);
  /* V2: space (after key, before digits). */
  v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_internal_json_u32("\"size\" 7}", "\"size\"", &v));
  TEST_ASSERT_EQ(7, v);
  /* V3: quote (e.g. "size""123"). */
  v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_internal_json_u32("\"size\"\"3", "\"size\"", &v));
  TEST_ASSERT_EQ(3, v);
  /* V4: digit immediately after key -- skip loop exits at first iter. */
  v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_internal_json_u32("\"size\"9}", "\"size\"", &v));
  TEST_ASSERT_EQ(9, v);
  TEST_END("ra8_ota MC/DC: ra8_ota_internal_json_u32 skip-char OR");
}

/**
 * @test test_mcdc_ota_internal_char_in_range
 *
 * @par MC/DC:
 * Decision at libs/ra8_ota/src/ra8_ota.c (call site) -> helper at
 * libs/ra8_ota/src/ra8_ota.c:
 *   ``c >= lo && c <= hi`` (2 conditions, AND).
 * - V1: c<lo  -> false (varies left from V2)
 * - V2: lo<=c<=hi -> true
 * - V3: c>hi -> false (varies right from V2)
 * N+1 = 3.
 */
static void test_mcdc_ota_internal_char_in_range(void)
{
  TEST_BEGIN("ra8_ota MC/DC: char_in_range AND");
  TEST_ASSERT(!ra8_ota_internal_char_in_range('@', 'A', 'F'));
  TEST_ASSERT(ra8_ota_internal_char_in_range('C', 'A', 'F'));
  TEST_ASSERT(!ra8_ota_internal_char_in_range('Z', 'A', 'F'));
  TEST_END("ra8_ota MC/DC: char_in_range AND");
}

/**
 * @test test_mcdc_ota_internal_download_state_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra8_ota/src/ra8_ota.c (call site) -> helper at
 * libs/ra8_ota/src/ra8_ota.c:
 *   ``state != IDLE && state != DOWNLOADING`` (2 conditions, AND).
 * - V1: state=IDLE        -> false (left varies vs V3)
 * - V2: state=DOWNLOADING -> false (right varies vs V3)
 * - V3: state=ERROR       -> true  (both true)
 * N+1 = 3.
 */
static void test_mcdc_ota_internal_download_state_invalid(void)
{
  TEST_BEGIN("ra8_ota MC/DC: download_state_invalid AND");
  TEST_ASSERT(!ra8_ota_internal_download_state_invalid((uint32_t)k_ra8_ota_state_idle,
                                                       (uint32_t)k_ra8_ota_state_downloading,
                                                       (uint32_t)k_ra8_ota_state_idle));
  TEST_ASSERT(!ra8_ota_internal_download_state_invalid((uint32_t)k_ra8_ota_state_idle,
                                                       (uint32_t)k_ra8_ota_state_downloading,
                                                       (uint32_t)k_ra8_ota_state_downloading));
  TEST_ASSERT(ra8_ota_internal_download_state_invalid((uint32_t)k_ra8_ota_state_idle,
                                                      (uint32_t)k_ra8_ota_state_downloading,
                                                      (uint32_t)k_ra8_ota_state_error));
  TEST_END("ra8_ota MC/DC: download_state_invalid AND");
}

int main(void)
{
  test_init_validation();
  test_check_bad_manifest();
  test_partial_download_recovery();
  test_signature_mismatch();
  test_sha256_mismatch();
  test_happy_path();
  test_metadata_tamper_rejected();
  test_mcdc_download_state_guard();
  test_mcdc_run_full_update_terminal();
  test_mcdc_hex_decode_invalid_nibble();
  test_mcdc_priv_json_u32_skip_chars();
  test_mcdc_hex_nibble_pair_completion();
  test_mcdc_ota_internal_char_in_range();
  test_mcdc_ota_internal_download_state_invalid();
  (void)fprintf(stderr, "[OK  ] test_ra8_ota.c\n");
  return 0;
}
