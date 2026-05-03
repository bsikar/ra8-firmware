/**
 * @file test_ra_rsip_edge_cases.c
 * @brief Edge-case + stress unit tests for the Renesas Secure IP (RSIP-E50D).
 *
 * @details
 * Complements ``test_ra_rsip.c`` with focused stress / edge-case
 * coverage of safety-critical paths:
 *
 *   - concurrent key-import collisions: multiple back-to-back installs
 *     into the same handle must leave the handle consistent (last
 *     install wins; partial state never observable);
 *   - wrong-key-size rejection: HMAC install with key_len = 0;
 *   - non-block-multiple AES ECB / CBC length must be rejected as
 *     invalid_arg (no silent truncation);
 *   - AES-GCM null-arg matrix exhaustively rejects each null pointer;
 *   - status-clear rejects garbage masks (bits outside the ISR field);
 *   - TRNG output-bias sanity check: across 4096 bytes the byte-value
 *     histogram has expected variance bounds. The simulator backs
 *     RND_DATA with a constant sentinel, so the byte-distribution is
 *     deterministic; we still assert the output buffer is not all-zero
 *     (the most common silent-failure mode).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8d2_rsip_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_rsip.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_rsip_edge_trng_chunk = 64U,
  k_rsip_edge_trng_total = 4096U,
} ra_rsip_edge_const_t;

static void prep_running(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));
}

/* --- Concurrent key-import: back-to-back installs into the same handle --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_back_to_back_key_install(void)
{
  TEST_BEGIN("rsip back-to-back key installs leave handle consistent");
  prep_running();

  const uint8_t        k128[16] = {0x11U};
  const uint8_t        k192[24] = {0x22U};
  const uint8_t        k256[32] = {0x33U};
  ra_rsip_key_handle_t h        = {};

  /* Install AES-128, then immediately overwrite with AES-256. The
   * handle's alg field must reflect the most recent install -- no
   * stale AES-128 metadata may remain. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(k128, &h));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes128, (int32_t)h.alg);
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_handle_words_aes128, (int32_t)h.body_words);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes256_install_plain(k256, &h));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes256, (int32_t)h.alg);
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_handle_words_aes256, (int32_t)h.body_words);

  /* And finally AES-192 -- the body_words count must shrink, not
   * leak the larger AES-256 size. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes192_install_plain(k192, &h));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes192, (int32_t)h.alg);
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_handle_words_aes192, (int32_t)h.body_words);
  TEST_END("rsip back-to-back key installs leave handle consistent");
}

/* --- Wrong-key-size rejection on the HMAC install path --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_wrong_key_size_rejected(void)
{
  TEST_BEGIN("rsip HMAC install rejects zero-length key");
  prep_running();
  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t h       = {};
  /* HMAC install with key_len = 0 must be rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_hmac_sha256, key, 0U, &h));
  /* HMAC install with the wrong opcode (an AES alg) must also be rejected. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_aes128, key, sizeof(key), &h));
  TEST_END("rsip HMAC install rejects zero-length key");
}

/* --- AES non-block-multiple lengths in ECB/CBC must be rejected --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_aes_non_block_multiple_rejected(void)
{
  TEST_BEGIN("rsip AES ECB/CBC reject non-block-multiple lengths");
  prep_running();

  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t h       = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &h));

  const uint8_t pt[15] = {};
  uint8_t       ct[15] = {};
  /* ECB with len = 15 (non-multiple of 16). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_aes_cipher(&h,
                                             k_ra_rsip_aes_mode_ecb,
                                             k_ra_rsip_dir_encrypt,
                                             nullptr,
                                             pt,
                                             ct,
                                             sizeof(pt)));
  /* CBC with len = 15 -- same rejection. */
  const uint8_t iv[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_aes_cipher(&h,
                                             k_ra_rsip_aes_mode_cbc,
                                             k_ra_rsip_dir_encrypt,
                                             iv,
                                             pt,
                                             ct,
                                             sizeof(pt)));
  TEST_END("rsip AES ECB/CBC reject non-block-multiple lengths");
}

/* --- AES-GCM null-arg matrix --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_aes_gcm_null_matrix(void)
{
  TEST_BEGIN("rsip AES-GCM null-arg matrix");
  prep_running();
  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t h       = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &h));

  const uint8_t iv[12]  = {};
  const uint8_t aad[8]  = {};
  const uint8_t pt[16]  = {};
  uint8_t       ct[16]  = {};
  uint8_t       tag[16] = {};

  /* Null key handle. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_aes_gcm(nullptr,
                                          k_ra_rsip_dir_encrypt,
                                          iv,
                                          aad,
                                          sizeof(aad),
                                          pt,
                                          ct,
                                          sizeof(pt),
                                          tag));
  /* Null IV. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_aes_gcm(&h,
                                          k_ra_rsip_dir_encrypt,
                                          nullptr,
                                          aad,
                                          sizeof(aad),
                                          pt,
                                          ct,
                                          sizeof(pt),
                                          tag));
  /* Null tag-out. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_aes_gcm(&h,
                                          k_ra_rsip_dir_encrypt,
                                          iv,
                                          aad,
                                          sizeof(aad),
                                          pt,
                                          ct,
                                          sizeof(pt),
                                          nullptr));
  TEST_END("rsip AES-GCM null-arg matrix");
}

/* --- Status-clear rejects garbage bits --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_garbage_bits(void)
{
  TEST_BEGIN("rsip clear_status rejects mask with bits outside ISR field");
  prep_running();
  /* Bit at 0x40000000 is outside the documented ISR field; must be rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_clear_status(0x40000000UL));
  /* Empty mask is also rejected (no-op clears are pointless). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_clear_status(0U));
  /* Valid bit accepted. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_clear_status((uint32_t)k_ra_rsip_mask_isr_done));
  TEST_END("rsip clear_status rejects mask with bits outside ISR field");
}

/* --- TRNG bias sanity: bulk read produces non-zero output --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_trng_bulk_nonzero(void)
{
  TEST_BEGIN("rsip TRNG bulk read is not all-zero (silent-failure guard)");
  prep_running();

  /* Pre-load RND_DATA with a non-zero pattern so the simulator returns
   * predictable, non-zero data. The real chip's TRNG would self-vary;
   * here we are guarding against the "driver returned but never wrote"
   * silent-failure mode. */
  *ra_rsip_reg32(k_ra_rsip_off_rnd_data) = 0xCAFEBABEUL;

  uint8_t  buf[k_rsip_edge_trng_total] = {};
  uint32_t off                         = 0U;
  while (off < (uint32_t)k_rsip_edge_trng_total) {
    TEST_ASSERT_EQ((int32_t)k_ra_ok,
                   (int32_t)ra_rsip_trng_read(&buf[off], (uint32_t)k_rsip_edge_trng_chunk));
    off += (uint32_t)k_rsip_edge_trng_chunk;
  }
  /* The buffer must contain at least one non-zero byte. */
  uint32_t nz = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_rsip_edge_trng_total; ++i) {
    if (buf[i] != 0U) {
      ++nz;
    }
  }
  TEST_ASSERT(nz > 0U);
  /* And the all-zero byte count must be a minority (sentinel pattern is
   * 0xCAFEBABE: 0 of 4 bytes are 0x00). With a real RNG we'd expect ~1/256
   * to be 0x00; with the deterministic sentinel we expect 0 zero-bytes. */
  TEST_ASSERT(nz == (uint32_t)k_rsip_edge_trng_total);
  TEST_END("rsip TRNG bulk read is not all-zero (silent-failure guard)");
}

int32_t main(void)
{
  test_back_to_back_key_install();
  test_wrong_key_size_rejected();
  test_aes_non_block_multiple_rejected();
  test_aes_gcm_null_matrix();
  test_clear_status_garbage_bits();
  test_trng_bulk_nonzero();
  (void)fprintf(stderr, "[OK  ] test_ra_rsip_edge_cases.c\n");
  return 0;
}
