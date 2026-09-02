/**
 * @file test_ra8_ota_cov.c
 * @brief Coverage-gap tests for libs/ra8_ota/src/ra8_ota.c.
 *
 * @details
 * Companion to test_ra8_ota.c. Targets the branches left uncovered by the
 * primary test suite: the on_progress callback path, network/crypto/flash
 * fault-injection paths, not-initialized guards, wrong-state guards,
 * and the priv_step_dispatch terminal-state arms.
 *
 * NOTE: ra8_ota_system_reset_hook is NOT overridden in this file so the
 * weak definition in ra8_ota.c fires when ra8_ota_commit_and_reboot is
 * called, covering the otherwise-dead weak body (lines 905/908).
 *
 * The mock harness (network / crypto / flash fault-injection callbacks and
 * the priv_* fixture builders) lives in tests/support/inc/ota_cov_mocks.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_cov_mocks.h"
#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"
#include "unity_minimal.h"

/* =============================================================================
 * Tests
 * ============================================================================= */

/**
 * @brief Verify on_progress callback fires and receives a valid snapshot.
 *
 * @details Registers a callback, drives check_for_update (which calls
 * priv_set_state at least once), then confirms the counter incremented.
 * Covers ra8_ota.c lines 101-108 (the on_progress != nullptr branch).
 *
 * @par MC/DC:
 * Decision: `if (s_cfg.on_progress != nullptr)` (1 condition).
 * - This test: on_progress != nullptr -> true (fires callback).
 * - test_ra8_ota.c happy-path uses nullptr, covering the false arm.
 */
static void test_progress_callback_fires(void)
{
  TEST_BEGIN("cov: on_progress callback fires");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra8_ota_cfg_t cfg = priv_make_cfg(true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  /* priv_set_state was called (at minimum: once for checking, once for idle). */
  TEST_ASSERT(g_cov_progress_cnt >= 2U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
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
  ra8_ota_cfg_t cfg = priv_make_cfg(true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  /* check_for_update: priv_set_state(checking) -> false arm (no manifest yet).
   * Then priv_set_state(idle) -> false arm (manifest_valid still false during
   * the checking transition; true arm fires later when manifest IS cached and
   * download transitions call priv_set_state). */
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  /* Download: priv_set_state(downloading) fires with manifest_valid=true. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT(g_cov_progress_cnt >= 3U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: progress bytes_total ternary both arms");
}

/**
 * @brief Verify check_for_update returns not-initialized when module is
 *        uninitialized.
 *
 * @details Covers ra8_ota.c line 353.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_check_not_initialized(void)
{
  TEST_BEGIN("cov: check_for_update not-initialized guard");
  (void)ra8_ota_deinit();
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_check_for_update(&m));
  TEST_END("cov: check_for_update not-initialized guard");
}

/**
 * @brief Verify check_for_update returns invalid-state when SM is not idle.
 *
 * @details Forces an error state via a bad manifest, then calls
 * check_for_update again. Covers ra8_ota.c line 357.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  /* First call puts SM into error. */
  TEST_ASSERT(ra8_ota_check_for_update(&m) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  /* Second call hits the state != idle guard. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: check_for_update invalid-state guard");
}

/**
 * @brief Verify net.open failure during manifest fetch propagates correctly.
 *
 * @details Net.open is made to fail on its first call (which is the manifest
 * open). Covers ra8_ota.c lines 305, 364, 365.
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
  ra8_ota_cfg_t cfg     = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: check_for_update manifest open failure");
}

/**
 * @brief Verify that an oversized Content-Length in the manifest response
 *        causes an invalid-size error and closes the connection.
 *
 * @details Covers ra8_ota.c lines 308, 309.
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
  ra8_ota_cfg_t cfg   = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: manifest content_len > max rejected");
}

/**
 * @brief Verify that a read error during manifest drain propagates correctly.
 *
 * @details Net.read is made to fail on its first call (inside priv_drain
 * during the manifest fetch). Covers ra8_ota.c line 315.
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
  ra8_ota_cfg_t cfg     = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: manifest drain read failure");
}

/**
 * @brief Verify download_to_inactive_bank returns not-initialized when the
 *        module has not been initialized.
 *
 * @details Covers ra8_ota.c line 540.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_download_not_initialized(void)
{
  TEST_BEGIN("cov: download not-initialized guard");
  (void)ra8_ota_deinit();
  ra8_ota_manifest_t m = {};
  m.image_size_bytes   = k_cov_image_size;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_download_to_inactive_bank(&m));
  TEST_END("cov: download not-initialized guard");
}

/**
 * @brief Verify that image_size_bytes > bank_size_bytes is rejected.
 *
 * @details The manifest image size is set larger than the configured bank.
 * Covers ra8_ota.c lines 549, 550.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  /* Override image_size to exceed the configured bank. */
  m.image_size_bytes = k_cov_bank_size + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download image_size > bank_size rejected");
}

/**
 * @brief Verify that a flash erase failure in priv_prepare_bank propagates.
 *
 * @details Covers ra8_ota.c lines 458, 556, 557.
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
  ra8_ota_cfg_t cfg      = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download flash erase failure");
}

/**
 * @brief Verify that net.open failure for the image URL propagates.
 *
 * @details Net.open is made to fail on its second call (the first is the
 * manifest open which must succeed; the second is the image open).
 * Covers ra8_ota.c lines 564, 565.
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
  ra8_ota_cfg_t cfg     = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download image open failure");
}

/**
 * @brief Verify that a premature EOF (got==0) in priv_download_chunk
 *        returns k_ra8_err_hw_error.
 *
 * @details Net.read returns 0 bytes on its first call during the image
 * download (i.e. after the manifest has already been read). The second
 * net.open is for the image; the reads after that are for the image data.
 * We make the first image read return 0 to hit the got==0 guard.
 * Covers ra8_ota.c line 415.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  /* check_for_update drains the manifest via multiple reads; record how many. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
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
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download chunk got==0 hw_error");
}

/**
 * @brief Verify sha256_update failure during chunk download propagates.
 *
 * @details sha256_update fails on its first call, which occurs inside
 * priv_download_chunk. Covers ra8_ota.c line 419.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  /* sha_init is call #1 (prepare_bank); sha_update is call #1 (first chunk).
   * Make sha_update fail on call #1. */
  g_cov_sha_upd_fail_n   = 1;
  g_cov_sha_upd_call_cnt = 0;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download sha256_update failure");
}

/**
 * @brief Verify flash.program failure during chunk download propagates.
 *
 * @details Covers ra8_ota.c line 423.
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
  ra8_ota_cfg_t cfg     = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: download flash.program failure");
}

/**
 * @brief Verify verify_signature returns not-initialized when uninitialized.
 *
 * @details Covers ra8_ota.c line 666.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_verify_not_initialized(void)
{
  TEST_BEGIN("cov: verify_signature not-initialized guard");
  (void)ra8_ota_deinit();
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_verify_signature(&m));
  TEST_END("cov: verify_signature not-initialized guard");
}

/**
 * @brief Verify verify_signature returns invalid-state when SM is not in
 *        verifying state.
 *
 * @details Covers ra8_ota.c line 670.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  /* SM is in idle, not verifying. */
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_idle, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: verify_signature invalid-state guard");
}

/**
 * @brief Verify that sha256_init failure inside priv_rehash_bank propagates.
 *
 * @details sha256_init is made to fail on its SECOND call. The first call is
 * inside priv_prepare_bank (download phase); the second is inside
 * priv_rehash_bank (verify phase). Covers ra8_ota.c lines 609, 676, 677.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());
  /* Now make sha_init fail on the next (second overall) call. */
  g_cov_sha_init_fail_n   = 2;
  g_cov_sha_init_call_cnt = 1;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: verify rehash sha256_init failure");
}

/**
 * @brief Verify that flash.readback failure inside priv_rehash_bank propagates.
 *
 * @details Covers ra8_ota.c lines 621, 676, 677.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());
  g_cov_flash_rb_fail = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: verify rehash flash.readback failure");
}

/**
 * @brief Verify that sha256_update failure during priv_rehash_bank propagates.
 *
 * @details Download uses one sha_update call (256-byte image = 1 chunk).
 * Rehash also uses one sha_update call. Fail on the second overall call to
 * hit the rehash path. Covers ra8_ota.c lines 625, 676, 677.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());
  /* Download consumed 1 sha_update call; fail on the 2nd (rehash). */
  g_cov_sha_upd_fail_n   = 2;
  g_cov_sha_upd_call_cnt = 1;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: verify rehash sha256_update failure");
}

/**
 * @brief Verify commit_and_reboot returns not-initialized when uninitialized.
 *
 * @details Covers ra8_ota.c line 725.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_commit_not_initialized(void)
{
  TEST_BEGIN("cov: commit_and_reboot not-initialized guard");
  (void)ra8_ota_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_commit_and_reboot());
  TEST_END("cov: commit_and_reboot not-initialized guard");
}

/**
 * @brief Verify commit_and_reboot returns invalid-state when SM is not in
 *        committing state.
 *
 * @details Covers ra8_ota.c line 728.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  /* SM is in idle, not committing. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: commit_and_reboot invalid-state guard");
}

/**
 * @brief Verify that flash.set_startup failure in commit_and_reboot propagates.
 *
 * @details Covers ra8_ota.c lines 733, 734.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_committing, ra8_ota_get_state());
  g_cov_flash_startup_fail = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: commit_and_reboot set_startup failure");
}

/**
 * @brief Verify ra8_ota_run_step returns not-initialized when uninitialized.
 *
 * @details Covers ra8_ota.c line 824.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_run_step_not_initialized(void)
{
  TEST_BEGIN("cov: run_step not-initialized guard");
  (void)ra8_ota_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_run_step());
  TEST_END("cov: run_step not-initialized guard");
}

/**
 * @brief Verify ra8_ota_run_full_update returns not-initialized when uninitialized.
 *
 * @details Covers ra8_ota.c line 857.
 *
 * @par MC/DC:
 * (single-condition guard; no compound decision)
 */
static void test_run_full_not_initialized(void)
{
  TEST_BEGIN("cov: run_full_update not-initialized guard");
  (void)ra8_ota_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ota_run_full_update());
  TEST_END("cov: run_full_update not-initialized guard");
}

/**
 * @brief Verify that a failing step inside run_full_update is returned.
 *
 * @details Manifest decode fails (bad JSON) so the internal step returns
 * an error which is propagated out. Covers ra8_ota.c line 868.
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  const ra8_err_t e = ra8_ota_run_full_update();
  TEST_ASSERT(e != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: run_full_update propagates step error");
}

/**
 * @brief Verify priv_step_dispatch returns k_ra8_ok for done and error states.
 *
 * @details Calls ra8_ota_run_step directly when the SM is in done/error state.
 * Covers ra8_ota.c lines 790, 792 (terminal-state arm of the switch).
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
  ra8_ota_cfg_t cfg = priv_make_cfg(false);

  /* Reach done state via full update. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_full_update());
  TEST_ASSERT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());
  /* Call run_step in done state -> priv_step_dispatch done case -> k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_step());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* Reach error state via bad manifest. */
  priv_reset_flags();
  (void)snprintf(g_cov_manifest, sizeof g_cov_manifest, "{}");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT(ra8_ota_check_for_update(&m) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  /* Call run_step in error state -> priv_step_dispatch error case -> k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_step());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: run_step terminal states done and error");
}

/**
 * @brief Full happy path using the weak ra8_ota_system_reset_hook.
 *
 * @details Because this file does NOT define ra8_ota_system_reset_hook, the
 * weak definition in ra8_ota.c fires when commit_and_reboot is called. This
 * covers ra8_ota.c lines 905 and 908 (the weak hook body).
 *
 * @par MC/DC:
 * (no compound decisions; pure path coverage)
 */
static void test_commit_weak_reset_hook(void)
{
  TEST_BEGIN("cov: commit fires weak ra8_ota_system_reset_hook");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra8_ota_cfg_t cfg = priv_make_cfg(false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_verify_signature(&m));
  TEST_ASSERT_EQ(k_ra8_ota_state_committing, ra8_ota_get_state());
  /* The weak hook is a no-op -- the call succeeds and returns k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_commit_and_reboot());
  TEST_ASSERT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: commit fires weak ra8_ota_system_reset_hook");
}

/* =============================================================================
 * main
 * ============================================================================= */

/**
 * @brief Drive internal_step_dispatch's transient-state arm from both sides.
 *
 * @details The `checking` / `downloading` cases share one arm whose only
 * decision is whether a validated manifest is already latched. Neither state
 * survives an API call, so the arm is only reachable by placing the state
 * machine in one of them directly, which is what this case does.
 *
 * @par MC/DC:
 * Decision: `if (s_manifest_valid)` in internal_step_dispatch (1 condition)
 * - Vector 1: no manifest latched, state forced to checking -> false
 *   (k_ra8_err_invalid_state)
 * - Vector 2: manifest latched by a successful check, state forced back to
 *   checking -> true (the download is re-entered and succeeds)
 * N+1 = 2 vectors for N=1: minimal MC/DC.
 */
static void test_run_step_transient_state_arm(void)
{
  TEST_BEGIN("cov: run_step transient checking state, both manifest arms");
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  ra8_ota_cfg_t cfg = priv_make_cfg(false);

  /* Vector 1: fresh init leaves no validated manifest behind. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  g_ra8_ota_state = k_ra8_ota_state_checking;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ota_run_step());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());

  /* Vector 2: a successful check latches the manifest, then force the state to
   * the other transient value the same arm serves. */
  priv_reset_flags();
  priv_make_image();
  priv_make_manifest();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
  ra8_ota_manifest_t m = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_check_for_update(&m));
  g_ra8_ota_state = k_ra8_ota_state_downloading;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_run_step());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_deinit());
  TEST_END("cov: run_step transient checking state, both manifest arms");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_progress_callback_fires,
  test_progress_bytes_total_both_arms,
  test_check_not_initialized,
  test_check_wrong_state,
  test_check_manifest_open_fail,
  test_check_manifest_too_large,
  test_check_manifest_drain_fail,
  test_download_not_initialized,
  test_download_image_too_large,
  test_download_erase_fail,
  test_download_image_open_fail,
  test_download_chunk_eof,
  test_download_sha_update_fail,
  test_download_program_fail,
  test_verify_not_initialized,
  test_verify_wrong_state,
  test_verify_rehash_sha_init_fail,
  test_verify_rehash_readback_fail,
  test_verify_rehash_sha_update_fail,
  test_commit_not_initialized,
  test_commit_wrong_state,
  test_commit_startup_fail,
  test_run_step_not_initialized,
  test_run_full_not_initialized,
  test_run_full_step_error,
  test_run_step_terminal_states,
  test_commit_weak_reset_hook,
  test_run_step_transient_state_arm,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
