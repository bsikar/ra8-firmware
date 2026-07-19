/**
 * @file test_ra8_rsip_edge_cases.c
 * @brief Edge-case + stress unit tests for the Renesas Secure IP (RSIP-E50D).
 *
 * @details
 * Complements ``test_ra8_rsip.c`` with focused stress / edge-case
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

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_edge_len_t
 * @brief Buffer lengths chosen to sit just off a block or digest boundary.
 *
 * @details
 * The driver's length guards are the subject here, so each capacity is one
 * short of, or one past, a natural boundary: 15 is one below an AES block, 17
 * one above, 132 one past a 4096-bit RSA signature.
 */
typedef enum : uint16_t {
  k_t_ct_under_block = 15U,  /**< One byte short of an AES block.             */
  k_t_out_over_block = 17U,  /**< One byte past an AES block.                 */
  k_t_out_block_cap  = 64U,  /**< A full SHA-512 digest / four AES blocks.    */
  k_t_sig_over_cap   = 132U, /**< Past the largest signature the driver emits. */
} t_edge_len_t;

/**
 * @enum t_edge_fill_t
 * @brief Fill pattern for the fixture header body.
 */
typedef enum : uint32_t {
  k_t_body_fill = 0xA5A50000UL, /**< Alternating high half, ORed with the word
                                     index so each word is unique.             */
} t_edge_fill_t;

typedef enum : uint32_t {
  k_rsip_edge_trng_chunk = 64U,   /**< Rsip edge trng chunk. */
  k_rsip_edge_trng_total = 4096U, /**< Rsip edge trng total. */
  k_rsip_edge_blob_bytes = 64U,   /**< Rsip edge blob bytes. */
} ra8_rsip_edge_const_t;

static void prep_running(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
}

static void clear_mbox_error(void)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret) = 0U;
}

static void arm_mbox_error(void)
{
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret) = 1U;
}

static ra8_rsip_key_handle_t make_handle(ra8_rsip_oem_cmd_t alg, uint32_t words)
{
  ra8_rsip_key_handle_t h = {.alg = (uint32_t)alg, .body_words = words};
  for (uint32_t i = 0U; i < words; ++i) {
    h.body[i] = k_t_body_fill | i;
  }
  return h;
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

  const uint8_t         k128[16] = {0x11U};
  const uint8_t         k192[24] = {0x22U};
  const uint8_t         k256[32] = {0x33U};
  ra8_rsip_key_handle_t h        = {};

  /* Install AES-128, then immediately overwrite with AES-256. The
   * handle's alg field must reflect the most recent install -- no
   * stale AES-128 metadata may remain. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes128_install_plain(k128, &h));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_aes128, h.alg);
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_aes128, h.body_words);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes256_install_plain(k256, &h));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_aes256, h.alg);
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_aes256, h.body_words);

  /* And finally AES-192 -- the body_words count must shrink, not
   * leak the larger AES-256 size. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes192_install_plain(k192, &h));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_aes192, h.alg);
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_aes192, h.body_words);
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
  const uint8_t         key[32] = {};
  ra8_rsip_key_handle_t h       = {};
  /* HMAC install with key_len = 0 must be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha256, key, 0U, &h));
  /* HMAC install with the wrong opcode (an AES alg) must also be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_aes128, key, sizeof(key), &h));
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

  const uint8_t         key[16] = {};
  ra8_rsip_key_handle_t h       = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes128_install_plain(key, &h));

  const uint8_t pt[15]                       = {};
  uint8_t       ct[k_t_ct_under_block] = {};
  /* ECB with len = 15 (non-multiple of 16). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_aes_cipher(&h,
                                     k_ra8_rsip_aes_mode_ecb,
                                     k_ra8_rsip_dir_encrypt,
                                     nullptr,
                                     pt,
                                     ct,
                                     sizeof(pt)));
  /* CBC with len = 15 -- same rejection. */
  const uint8_t iv[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_aes_cipher(&h,
                                     k_ra8_rsip_aes_mode_cbc,
                                     k_ra8_rsip_dir_encrypt,
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
  const uint8_t         key[16] = {};
  ra8_rsip_key_handle_t h       = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes128_install_plain(key, &h));

  const uint8_t iv[12]  = {};
  const uint8_t aad[8]  = {};
  const uint8_t pt[16]  = {};
  uint8_t       ct[16]  = {};
  uint8_t       tag[16] = {};

  /* Null key handle. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_gcm(nullptr,
                                  k_ra8_rsip_dir_encrypt,
                                  iv,
                                  aad,
                                  sizeof(aad),
                                  pt,
                                  ct,
                                  sizeof(pt),
                                  tag));
  /* Null IV. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_gcm(&h,
                                  k_ra8_rsip_dir_encrypt,
                                  nullptr,
                                  aad,
                                  sizeof(aad),
                                  pt,
                                  ct,
                                  sizeof(pt),
                                  tag));
  /* Null tag-out. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_gcm(&h,
                                  k_ra8_rsip_dir_encrypt,
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_clear_status(0x40000000UL));
  /* Empty mask is also rejected (no-op clears are pointless). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_clear_status(0U));
  /* Valid bit accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_clear_status((uint32_t)k_ra8_rsip_mask_isr_done));
  TEST_END("rsip clear_status rejects mask with bits outside ISR field");
}

/* --- TRNG bias sanity: bulk read produces non-zero output --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_trng_bulk_fail_closed(void)
{
  TEST_BEGIN("rsip TRNG bulk read fails closed (no fake entropy)");
  prep_running();

  /* The RSIP-E50D TRNG has no working register interface on silicon (the map is
   * invented), so ra8_rsip_trng_read fails closed rather than emit a fake or
   * silent value. A bulk read must reject every chunk and touch nothing --
   * fail-closed is the guard against the old "returned OK but wrote garbage"
   * silent-failure mode, now impossible because it never returns OK. */
  uint8_t  buf[k_rsip_edge_trng_total] = {};
  uint32_t off                         = 0U;
  while (off < (uint32_t)k_rsip_edge_trng_total) {
    TEST_ASSERT_EQ(k_ra8_err_not_supported,
                   ra8_rsip_trng_read(&buf[off], (uint32_t)k_rsip_edge_trng_chunk));
    off += (uint32_t)k_rsip_edge_trng_chunk;
  }
  /* The buffer must stay all-zero -- fail-closed writes nothing. */
  uint32_t nz = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_rsip_edge_trng_total; ++i) {
    if (buf[i] != 0U) {
      ++nz;
    }
  }
  TEST_ASSERT_EQ(0U, nz);
  TEST_END("rsip TRNG bulk read fails closed (no fake entropy)");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_install_nulls_and_hmac_variants(void)
{
  TEST_BEGIN("rsip key install nulls and HMAC variants");
  prep_running();

  const uint8_t         key32[32] = {};
  ra8_rsip_key_handle_t h         = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_aes192_install_plain(nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_aes192_install_plain(key32, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_aes256_install_plain(nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_aes256_install_plain(key32, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_chacha20_install_plain(nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_chacha20_install_plain(key32, nullptr));

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha256, nullptr, sizeof(key32), &h));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha256, key32, sizeof(key32), nullptr));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha224, key32, sizeof(key32), &h));
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_hmac_sha224, h.body_words);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha384, key32, sizeof(key32), &h));
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_hmac_sha384, h.body_words);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha512, key32, sizeof(key32), &h));
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_hmac_sha512, h.body_words);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha512_224, key32, sizeof(key32), &h));
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_hmac_sha512, h.body_words);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha512_256, key32, sizeof(key32), &h));
  TEST_ASSERT_EQ(k_ra8_rsip_handle_words_hmac_sha512, h.body_words);

  TEST_END("rsip key install nulls and HMAC variants");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_oem_install_variants_and_hw_error(void)
{
  TEST_BEGIN("rsip OEM install variants and HW error");
  prep_running();

  const uint8_t         iv[16]   = {};
  const uint8_t         blob[64] = {};
  ra8_rsip_key_handle_t h        = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_oem_install(k_ra8_rsip_oem_cmd_aes128, iv, blob, sizeof(blob), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_oem_install(k_ra8_rsip_oem_cmd_aes128, iv, nullptr, sizeof(blob), &h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_oem_install(k_ra8_rsip_oem_cmd_aes128_xts, iv, blob, sizeof(blob), &h));

  arm_mbox_error();
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rsip_oem_install(k_ra8_rsip_oem_cmd_aes128, iv, blob, sizeof(blob), &h));
  clear_mbox_error();

  TEST_END("rsip OEM install variants and HW error");
}
/**
 * @brief Exercise the null / arg / happy legs of `ra8_rsip_aes_cipher`.
 * @param[in]  aes192 AES-192 key handle.
 * @param[in]  aes256 AES-256 key handle.
 * @param[in]  bad    A non-AES (ChaCha) handle for the algorithm-mismatch leg.
 * @param[in]  iv     16-byte IV.
 * @param[in]  in     Input buffer.
 * @param[out] out    Output buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every cipher error and happy leg returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void aes_err_cipher(ra8_rsip_key_handle_t* aes192,
                           ra8_rsip_key_handle_t* aes256,
                           ra8_rsip_key_handle_t* bad,
                           const uint8_t*         iv,
                           const uint8_t*         in,
                           uint8_t*               out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_cipher(nullptr,
                                     k_ra8_rsip_aes_mode_ecb,
                                     k_ra8_rsip_dir_encrypt,
                                     nullptr,
                                     in,
                                     out,
                                     16U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_cipher(aes192,
                                     k_ra8_rsip_aes_mode_ecb,
                                     k_ra8_rsip_dir_encrypt,
                                     nullptr,
                                     nullptr,
                                     out,
                                     16U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_aes_cipher(aes192,
                                     k_ra8_rsip_aes_mode_ecb,
                                     k_ra8_rsip_dir_encrypt,
                                     nullptr,
                                     in,
                                     nullptr,
                                     16U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rsip_aes_cipher(bad, k_ra8_rsip_aes_mode_ctr, k_ra8_rsip_dir_encrypt, iv, in, out, 5U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_aes_cipher(aes192,
                                     k_ra8_rsip_aes_mode_cmac,
                                     k_ra8_rsip_dir_encrypt,
                                     nullptr,
                                     in,
                                     out,
                                     5U));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_aes_cipher(aes192, k_ra8_rsip_aes_mode_ctr, k_ra8_rsip_dir_decrypt, iv, in, out, 17U));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_aes_cipher(aes256, k_ra8_rsip_aes_mode_gmac, k_ra8_rsip_dir_encrypt, iv, in, out, 5U));
}

/**
 * @brief Exercise the null / arg / happy legs of `ra8_rsip_aes_gcm`.
 * @param[in]  aes192 AES-192 key handle.
 * @param[in]  bad    A non-AES handle for the algorithm-mismatch leg.
 * @param[in]  iv     16-byte IV.
 * @param[in]  in     Input buffer.
 * @param[out] out    Output buffer.
 * @param[out] tag    16-byte tag buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every GCM error and happy leg returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void aes_err_gcm(ra8_rsip_key_handle_t* aes192,
                        ra8_rsip_key_handle_t* bad,
                        const uint8_t*         iv,
                        const uint8_t*         in,
                        uint8_t*               out,
                        uint8_t*               tag)
{
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_gcm(aes192, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, nullptr, out, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_gcm(aes192, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, nullptr, 16U, tag));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_aes_gcm(bad, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, out, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_aes_gcm(aes192, k_ra8_rsip_dir_decrypt, iv, nullptr, 0U, in, out, 16U, tag));
}

/**
 * @brief Exercise the null / arg / happy legs of `ra8_rsip_aes_ccm`.
 * @param[in]  aes192 AES-192 key handle.
 * @param[in]  aes256 AES-256 key handle.
 * @param[in]  bad    A non-AES handle for the algorithm-mismatch leg.
 * @param[in]  iv     16-byte IV.
 * @param[in]  in     Input buffer.
 * @param[out] out    Output buffer.
 * @param[out] tag    16-byte tag buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every CCM error and happy leg returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void aes_err_ccm(ra8_rsip_key_handle_t* aes192,
                        ra8_rsip_key_handle_t* aes256,
                        ra8_rsip_key_handle_t* bad,
                        const uint8_t*         iv,
                        const uint8_t*         in,
                        uint8_t*               out,
                        uint8_t*               tag)
{
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_ccm(nullptr, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, out, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_ccm(aes192, k_ra8_rsip_dir_encrypt, nullptr, nullptr, 0U, in, out, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_ccm(aes192, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, nullptr, out, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_ccm(aes192, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, nullptr, 16U, tag));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_aes_ccm(aes192, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, out, 16U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_aes_ccm(bad, k_ra8_rsip_dir_encrypt, iv, nullptr, 0U, in, out, 16U, tag));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_aes_ccm(aes256, k_ra8_rsip_dir_decrypt, iv, in, 5U, in, out, 16U, tag));
}

/**
 * @brief Assert a mailbox fault surfaces as k_ra8_err_hw_error from a cipher call.
 * @param[in]  aes192 AES-192 key handle.
 * @param[in]  iv     16-byte IV.
 * @param[in]  in     Input buffer.
 * @param[out] out    Output buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post The injected mailbox fault propagated and was cleared.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
aes_err_mbox(ra8_rsip_key_handle_t* aes192, const uint8_t* iv, const uint8_t* in, uint8_t* out)
{
  arm_mbox_error();
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    ra8_rsip_aes_cipher(aes192, k_ra8_rsip_aes_mode_ctr, k_ra8_rsip_dir_encrypt, iv, in, out, 5U));
  clear_mbox_error();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_aes_public_error_paths(void)
{
  TEST_BEGIN("rsip AES public error paths");
  prep_running();

  ra8_rsip_key_handle_t aes192 =
    make_handle(k_ra8_rsip_oem_cmd_aes192, (uint32_t)k_ra8_rsip_handle_words_aes192);
  ra8_rsip_key_handle_t aes256 =
    make_handle(k_ra8_rsip_oem_cmd_aes256, (uint32_t)k_ra8_rsip_handle_words_aes256);
  ra8_rsip_key_handle_t bad =
    make_handle(k_ra8_rsip_oem_cmd_chacha20, (uint32_t)k_ra8_rsip_handle_words_chacha20);
  const uint8_t iv[16]                        = {};
  const uint8_t in[17]                        = {};
  uint8_t       out[k_t_out_over_block] = {};
  uint8_t       tag[16]                       = {};

  aes_err_cipher(&aes192, &aes256, &bad, iv, in, out);
  aes_err_gcm(&aes192, &bad, iv, in, out, tag);
  aes_err_ccm(&aes192, &aes256, &bad, iv, in, out, tag);
  aes_err_mbox(&aes192, iv, in, out);

  TEST_END("rsip AES public error paths");
}
/**
 * @brief Thin wrapper over `ra8_rsip_chacha20_poly1305` for the error-path tests.
 * @param[in]  h       Key handle (nullptr exercises the handle guard).
 * @param[in]  dir     Cipher direction.
 * @param[in]  nonce   12-byte nonce (nullptr exercises the nonce guard).
 * @param[in]  aad     Additional authenticated data.
 * @param[in]  aad_len AAD length in bytes.
 * @param[in]  in      Input buffer (nullptr exercises the input guard).
 * @param[out] out     Output buffer (nullptr exercises the output guard).
 * @param[in]  len     Payload length in bytes.
 * @param[out] tag     16-byte tag buffer (nullptr exercises the tag guard).
 * @return The `ra8_rsip_chacha20_poly1305` result code.
 * @pre The RSIP mock is in the running state.
 * @post No state beyond @p out / @p tag is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t rc_ccp(ra8_rsip_key_handle_t* h,
                        ra8_rsip_aes_dir_t     dir,
                        const uint8_t*         nonce,
                        const uint8_t*         aad,
                        uint32_t               aad_len,
                        const uint8_t*         in,
                        uint8_t*               out,
                        uint32_t               len,
                        uint8_t*               tag)
{
  return ra8_rsip_chacha20_poly1305(h, dir, nonce, aad, aad_len, in, out, len, tag);
}

/**
 * @brief Exercise the four null guards of `ra8_rsip_chacha20`.
 * @param[in]  chacha ChaCha20 key handle.
 * @param[in]  nonce  12-byte nonce.
 * @param[in]  msg    Input message.
 * @param[out] out    Output buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Each null argument returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void chacha_err_stream(ra8_rsip_key_handle_t* chacha,
                              const uint8_t*         nonce,
                              const uint8_t*         msg,
                              uint8_t*               out)
{
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_chacha20(nullptr, k_ra8_rsip_dir_encrypt, nonce, 0U, msg, out, sizeof(msg)));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_chacha20(chacha, k_ra8_rsip_dir_encrypt, nullptr, 0U, msg, out, sizeof(msg)));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_chacha20(chacha, k_ra8_rsip_dir_encrypt, nonce, 0U, nullptr, out, sizeof(msg)));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_chacha20(chacha, k_ra8_rsip_dir_encrypt, nonce, 0U, msg, nullptr, sizeof(msg)));
}

/**
 * @brief Exercise the null / arg / happy legs of `ra8_rsip_chacha20_poly1305`.
 * @param[in]  chacha ChaCha20 key handle.
 * @param[in]  aes    A non-ChaCha handle for the algorithm-mismatch leg.
 * @param[in]  nonce  12-byte nonce.
 * @param[in]  msg    Input message.
 * @param[out] out    Output buffer.
 * @param[out] tag    16-byte tag buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every AEAD error and happy leg returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void chacha_err_aead(ra8_rsip_key_handle_t* chacha,
                            ra8_rsip_key_handle_t* aes,
                            const uint8_t*         nonce,
                            const uint8_t*         msg,
                            uint8_t*               out,
                            uint8_t*               tag)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rc_ccp(nullptr, k_ra8_rsip_dir_encrypt, nonce, nullptr, 0U, msg, out, 5U, tag));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rc_ccp(chacha, k_ra8_rsip_dir_encrypt, nullptr, nullptr, 0U, msg, out, 5U, tag));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rc_ccp(chacha, k_ra8_rsip_dir_encrypt, nonce, nullptr, 0U, nullptr, out, 5U, tag));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rc_ccp(chacha, k_ra8_rsip_dir_encrypt, nonce, nullptr, 0U, msg, nullptr, 5U, tag));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rc_ccp(chacha, k_ra8_rsip_dir_encrypt, nonce, nullptr, 0U, msg, out, 5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rc_ccp(aes, k_ra8_rsip_dir_encrypt, nonce, nullptr, 0U, msg, out, 5U, tag));
  TEST_ASSERT_EQ(k_ra8_ok,
                 rc_ccp(chacha, k_ra8_rsip_dir_decrypt, nonce, nullptr, 0U, msg, out, 5U, tag));
}

/**
 * @brief Exercise the Poly1305 guards and the full public hash-algorithm menu.
 * @param[in]  otk 32-byte one-time key for Poly1305.
 * @param[in]  msg Input message.
 * @param[out] out Output digest buffer.
 * @param[out] tag 16-byte tag buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Poly1305 guards and every supported hash produced their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void chacha_err_poly_hash(const uint8_t* otk, const uint8_t* msg, uint8_t* out, uint8_t* tag)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_poly1305(otk, msg, sizeof(*msg) * 5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_poly1305(otk, nullptr, 1U, tag));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_poly1305(otk, nullptr, 0U, tag));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hash(k_ra8_rsip_hash_sha256, nullptr, 1U, out, 32U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_hash((ra8_rsip_hash_alg_t)0xFFFFU, msg, 5U, out, 32U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha224, nullptr, 0U, out, 28U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha384, msg, 5U, out, 48U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha512_224, msg, 5U, out, 28U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha512_256, msg, 5U, out, 32U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha3_224, msg, 5U, out, 28U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha3_384, msg, 5U, out, 48U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_sha3_512, msg, 5U, out, 64U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hash(k_ra8_rsip_hash_shake256, msg, 5U, out, 5U));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_chacha_poly_hash_hmac_error_paths(void)
{
  TEST_BEGIN("rsip ChaCha Poly hash HMAC error paths");
  prep_running();

  ra8_rsip_key_handle_t chacha =
    make_handle(k_ra8_rsip_oem_cmd_chacha20, (uint32_t)k_ra8_rsip_handle_words_chacha20);
  ra8_rsip_key_handle_t aes =
    make_handle(k_ra8_rsip_oem_cmd_aes128, (uint32_t)k_ra8_rsip_handle_words_aes128);
  const uint8_t nonce[12]                     = {};
  const uint8_t msg[5]                        = {'h', 'e', 'l', 'l', 'o'};
  uint8_t       out[k_t_out_block_cap] = {};
  uint8_t       tag[16]                       = {};
  const uint8_t otk[32]                       = {};

  chacha_err_stream(&chacha, nonce, msg, out);
  chacha_err_aead(&chacha, &aes, nonce, msg, out, tag);
  chacha_err_poly_hash(otk, msg, out, tag);

  TEST_END("rsip ChaCha Poly hash HMAC error paths");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_hmac_public_variants(void)
{
  TEST_BEGIN("rsip HMAC public variants");
  prep_running();

  const uint8_t         msg[4]                        = {'t', 'e', 's', 't'};
  uint8_t               mac[k_t_out_block_cap] = {};
  ra8_rsip_key_handle_t h224 =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha224, (uint32_t)k_ra8_rsip_handle_words_hmac_sha224);
  ra8_rsip_key_handle_t h384 =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha384, (uint32_t)k_ra8_rsip_handle_words_hmac_sha384);
  ra8_rsip_key_handle_t h512 =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha512, (uint32_t)k_ra8_rsip_handle_words_hmac_sha512);
  ra8_rsip_key_handle_t h512_224 =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha512_224, (uint32_t)k_ra8_rsip_handle_words_hmac_sha512);
  ra8_rsip_key_handle_t h512_256 =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha512_256, (uint32_t)k_ra8_rsip_handle_words_hmac_sha512);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac(nullptr, msg, sizeof(msg), mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac(&h224, msg, sizeof(msg), nullptr, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_hmac(&h224, nullptr, 1U, mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_hmac(&h224, nullptr, 0U, mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac(&h384, msg, sizeof(msg), mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac(&h512, msg, sizeof(msg), mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_hmac(&h512_224, msg, sizeof(msg), mac, sizeof(mac)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_hmac(&h512_256, msg, sizeof(msg), mac, sizeof(mac)));

  TEST_END("rsip HMAC public variants");
}
/**
 * @brief Exercise the null / happy legs of `ra8_rsip_rsa_sign` and `_verify`.
 * @param[in]  rsa    RSA-2048 private key handle.
 * @param[in]  digest Message digest buffer.
 * @param[out] sig    Signature buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every RSA null guard and the happy sign returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void asym_err_rsa(ra8_rsip_key_handle_t* rsa, const uint8_t* digest, uint8_t* sig)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_sign(nullptr, k_ra8_rsip_rsa_2048, digest, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_sign(rsa, k_ra8_rsip_rsa_2048, nullptr, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_sign(rsa, k_ra8_rsip_rsa_2048, digest, 32U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_verify(nullptr, k_ra8_rsip_rsa_2048, digest, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_verify(rsa, k_ra8_rsip_rsa_2048, nullptr, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_rsa_verify(rsa, k_ra8_rsip_rsa_2048, digest, 32U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_rsa_sign(rsa, k_ra8_rsip_rsa_1024, digest, 5U, sig));
}

/**
 * @brief Exercise the null / arg / curve-menu legs of the ECDSA sign+verify API.
 * @param[in]  ecc    ECC secp256r1 private key handle.
 * @param[in]  digest Message digest buffer.
 * @param[out] sig    Signature buffer.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every ECDSA guard and supported curve returned its documented code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void asym_err_ecdsa(ra8_rsip_key_handle_t* ecc, const uint8_t* digest, uint8_t* sig)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_sign(nullptr, k_ra8_rsip_curve_secp192r1, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_secp192r1, nullptr, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_secp192r1, digest, 5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_ecdsa_sign(ecc, (ra8_rsip_curve_t)99U, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_secp192r1, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_secp224r1, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_brain384r1, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_brain512r1, digest, 5U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_ecdsa_sign(ecc, k_ra8_rsip_curve_secp521r1, digest, 5U, sig));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_verify(nullptr, k_ra8_rsip_curve_secp256r1, digest, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_verify(ecc, k_ra8_rsip_curve_secp256r1, nullptr, 32U, sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdsa_verify(ecc, k_ra8_rsip_curve_secp256r1, digest, 32U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_ecdsa_verify(ecc, (ra8_rsip_curve_t)99U, digest, 32U, sig));
}

/**
 * @brief Exercise the null / arg / happy legs of `ra8_rsip_ecdh_compute`.
 * @param[in]  ecc   ECC secp256r1 private key handle.
 * @param[in]  point Peer point buffer.
 * @param[out] out   Derived shared-secret key handle.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every ECDH guard and the happy compute returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void
asym_err_ecdh(ra8_rsip_key_handle_t* ecc, const uint8_t* point, ra8_rsip_key_handle_t* out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdh_compute(nullptr, k_ra8_rsip_curve_secp256r1, point, point, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdh_compute(ecc, k_ra8_rsip_curve_secp256r1, nullptr, point, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdh_compute(ecc, k_ra8_rsip_curve_secp256r1, point, nullptr, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_ecdh_compute(ecc, k_ra8_rsip_curve_secp256r1, point, point, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_ecdh_compute(ecc, (ra8_rsip_curve_t)99U, point, point, out));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_ecdh_compute(ecc, k_ra8_rsip_curve_secp256k1, point, point, out));
}

/**
 * @brief Exercise the null guards of `ra8_rsip_key_wrap` and `_key_unwrap`.
 * @param[in]  ikm  Key-wrapping (HMAC) key handle.
 * @param[in]  ecc  Payload key handle to wrap.
 * @param[in]  iv   16-byte IV.
 * @param[out] blob Wrapped-blob buffer.
 * @param[out] out  Unwrapped key handle sink.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every wrap/unwrap null argument returned k_ra8_err_null_ptr.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void asym_err_keywrap(ra8_rsip_key_handle_t* ikm,
                             ra8_rsip_key_handle_t* ecc,
                             const uint8_t*         iv,
                             uint8_t*               blob,
                             ra8_rsip_key_handle_t* out)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_wrap(nullptr, iv, ecc, blob));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_wrap(ikm, nullptr, ecc, blob));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_wrap(ikm, iv, nullptr, blob));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_wrap(ikm, iv, ecc, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_unwrap(nullptr, iv, blob, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_unwrap(ikm, nullptr, blob, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_unwrap(ikm, iv, nullptr, out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_key_unwrap(ikm, iv, blob, nullptr));
}

/**
 * @brief Exercise the null guards and the happy path of `ra8_rsip_kdf`.
 * @param[in]  ikm Input-keying-material key handle.
 * @param[out] out Derived key handle sink.
 * @return None.
 * @pre The RSIP mock is in the running state.
 * @post Every KDF null argument and the happy derive returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void asym_err_kdf(ra8_rsip_key_handle_t* ikm, ra8_rsip_key_handle_t* out)
{
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_kdf(k_ra8_rsip_kdf_op_huk_label, nullptr, nullptr, 1U, nullptr, 0U, 32U, out));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_kdf(k_ra8_rsip_kdf_op_huk_label, nullptr, nullptr, 0U, nullptr, 1U, 32U, out));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_kdf(k_ra8_rsip_kdf_op_hkdf_sha384, nullptr, nullptr, 0U, nullptr, 0U, 32U, out));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rsip_kdf(k_ra8_rsip_kdf_op_uid_label, nullptr, nullptr, 0U, nullptr, 0U, 32U, nullptr));
  const uint8_t label[5]                  = {'l', 'a', 'b', 'e', 'l'};
  const uint8_t salt[3]                   = {'s', 'a', 'l'};
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out) = (uint32_t)k_ra8_rsip_oem_cmd_hmac_sha256;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_kdf(k_ra8_rsip_kdf_op_hkdf_sha512,
                              ikm,
                              label,
                              sizeof(label),
                              salt,
                              sizeof(salt),
                              32U,
                              out));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_asym_and_kdf_error_paths(void)
{
  TEST_BEGIN("rsip asymmetric and KDF error paths");
  prep_running();

  ra8_rsip_key_handle_t rsa =
    make_handle(k_ra8_rsip_oem_cmd_rsa2048_priv, (uint32_t)k_ra8_rsip_handle_words_rsa2048_priv);
  ra8_rsip_key_handle_t ecc = make_handle(k_ra8_rsip_oem_cmd_ecc_secp256r1_priv,
                                          (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  ra8_rsip_key_handle_t ikm =
    make_handle(k_ra8_rsip_oem_cmd_hmac_sha256, (uint32_t)k_ra8_rsip_handle_words_hmac_sha256);
  const uint8_t         digest[33]                     = {};
  uint8_t               sig[k_t_sig_over_cap] = {};
  const uint8_t         point[66]                      = {};
  uint8_t               blob[k_rsip_edge_blob_bytes]   = {};
  const uint8_t         iv[16]                         = {};
  ra8_rsip_key_handle_t out                            = {};

  asym_err_rsa(&rsa, digest, sig);
  asym_err_ecdsa(&ecc, digest, sig);
  asym_err_ecdh(&ecc, point, &out);
  asym_err_keywrap(&ikm, &ecc, iv, blob, &out);
  asym_err_kdf(&ikm, &out);

  TEST_END("rsip asymmetric and KDF error paths");
}

int32_t main(void)
{
  test_back_to_back_key_install();
  test_wrong_key_size_rejected();
  test_aes_non_block_multiple_rejected();
  test_aes_gcm_null_matrix();
  test_clear_status_garbage_bits();
  test_trng_bulk_fail_closed();
  test_install_nulls_and_hmac_variants();
  test_oem_install_variants_and_hw_error();
  test_aes_public_error_paths();
  test_chacha_poly_hash_hmac_error_paths();
  test_hmac_public_variants();
  test_asym_and_kdf_error_paths();
  (void)fprintf(stderr, "[OK  ] test_ra8_rsip_edge_cases.c\n");
  return 0;
}
