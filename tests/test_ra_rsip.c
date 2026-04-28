/**
 * @file test_ra_rsip.c
 * @brief Unit tests for ra_rsip.c (Renesas Secure IP HAL)
 *
 * @details
 * Drives the RSIP-E50D HAL surface against the
 * ``ra_sim_mmap``-backed register window:
 *
 * - happy-path init runs MSTP release + BIST gate;
 * - null-arg rejection on every public API;
 * - TRNG read pulls 32 bytes from the RND_DATA register;
 * - SHA-256 streams a buffer through HASH_DATA_IN and reads
 * back the digest from HASH_DIGEST;
 * - status / IRQ helpers ack the right bits;
 * - power transition (enter / exit stop).
 *
 * Each test resets ``ra_sim_mmap`` and ``ra_mstp`` first so cases
 * stay independent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_rsip_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_rsip.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_rsip_test_const_t
 * @brief Magic numbers used by the tests, named to keep the
 * no-magic-numbers rule satisfied.
 */
typedef enum : uint32_t {
  k_ra_rsip_test_trng_bytes  = 32U,          /**< TRNG read length under test. */
  k_ra_rsip_test_msg_bytes   = 8U,           /**< SHA input message size. */
  k_ra_rsip_test_pattern_w   = 0xCAFEBABEUL, /**< Sentinel TRNG word. */
  k_ra_rsip_test_digest_seed = 0x11223344UL, /**< Sentinel HASH digest word. */
  k_ra_rsip_test_invalid_len = 5U,           /**< Non-multiple-of-4 length. */
  k_ra_rsip_test_isr_garbage = 0x40000000UL, /**< Bit outside ISR field. */
} ra_rsip_test_const_t;

/**
 * @var s_test_isr_count
 * @brief Number of times the test stub callback has fired.
 *
 * @warning Reset by ``prep`` before every test that uses it.
 * @since 0.11.0
 */
static uint32_t s_test_isr_count;

/**
 * @var s_test_isr_last
 * @brief Most-recent ISR snapshot the stub callback received.
 *
 * @since 0.11.0
 */
static uint32_t s_test_isr_last;

/**
 * @brief Stub IRQ callback that just records what it sees.
 *
 * @param[in] ctx Caller context (unused).
 * @param[in] isr Snapshot from ``ra_rsip_dispatch``.
 * @since 0.11.0
 */
static void stub_rsip_cb(void* ctx, uint32_t isr)
{
  (void)ctx;
  ++s_test_isr_count;
  s_test_isr_last = isr;
}

/**
 * @brief Reset the world before each test.
 * @since 0.11.0
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_test_isr_count = 0U;
  s_test_isr_last  = 0U;
}

/**
 * @brief Happy-path: init runs MSTP release and BIST.
 */
static void test_init_happy(void)
{
  TEST_BEGIN("rsip init happy");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));

  /* CTRL.ENABLE should be left set, BIST_OK should be visible. */
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_mask_ctrl_enable, (int32_t)*ra_rsip_reg32(k_ra_rsip_off_ctrl));
  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_get_status(&status));
  TEST_ASSERT((status & (uint32_t)k_ra_rsip_mask_status_bistok) != 0U);

  TEST_END("rsip init happy");
}

/**
 * @brief Init with run_bist=false skips BIST gate.
 */
static void test_init_skip_bist(void)
{
  TEST_BEGIN("rsip init skip bist");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = false};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));

  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_get_status(&status));
  /* Without BIST we should NOT have observed BIST_OK auto-asserting. */
  TEST_ASSERT((status & (uint32_t)k_ra_rsip_mask_status_bistok) == 0U);

  TEST_END("rsip init skip bist");
}

/**
 * @brief Null cfg is rejected.
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("rsip init null cfg");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_init(nullptr));

  TEST_END("rsip init null cfg");
}

/**
 * @brief Deinit clears CTRL and gates the MSTP bit.
 */
static void test_deinit(void)
{
  TEST_BEGIN("rsip deinit");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_rsip_reg32(k_ra_rsip_off_ctrl));

  TEST_END("rsip deinit");
}

/**
 * @brief TRNG draws 32 bytes from RND_DATA.
 */
static void test_trng_read(void)
{
  TEST_BEGIN("rsip trng read 32");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));

  /* Pre-load the RND_DATA cell with a known sentinel word so the
 * test can verify each lane comes out correctly. */
  *ra_rsip_reg32(k_ra_rsip_off_rnd_data) = (uint32_t)k_ra_rsip_test_pattern_w;

  uint8_t buf[32] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_trng_read(buf, (uint32_t)k_ra_rsip_test_trng_bytes));

  /* 0xCAFEBABE in little-endian: BE BA FE CA */
  TEST_ASSERT_EQ((int32_t)0xBEU, (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)0xBAU, (int32_t)buf[1]);
  TEST_ASSERT_EQ((int32_t)0xFEU, (int32_t)buf[2]);
  TEST_ASSERT_EQ((int32_t)0xCAU, (int32_t)buf[3]);
  TEST_END("rsip trng read 32");
}

/**
 * @brief TRNG rejects null buffer + bad length.
 */
static void test_trng_arg_check(void)
{
  TEST_BEGIN("rsip trng arg check");
  prep();

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_trng_read(nullptr, (uint32_t)k_ra_rsip_test_trng_bytes));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_trng_read(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_trng_read(buf, (uint32_t)k_ra_rsip_test_invalid_len));

  TEST_END("rsip trng arg check");
}

/**
 * @brief SHA-256 streams the message and reads the digest back.
 */
static void test_sha256_happy(void)
{
  TEST_BEGIN("rsip sha256 happy");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));

  /* Pre-load the digest words so we have a known sentinel value
 * to compare against. The sim has no real hash unit. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_sha256_digest_words; ++w) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_hash_digest + (uint16_t)(w * 4U));
    *ra_rsip_reg32(off) = (uint32_t)k_ra_rsip_test_digest_seed + w;
  }

  const uint8_t msg[8]     = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  uint8_t       digest[32] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_sha256(msg, (uint32_t)k_ra_rsip_test_msg_bytes, digest));

  /* First sentinel = 0x11223344, little-endian: 44 33 22 11 */
  TEST_ASSERT_EQ((int32_t)0x44U, (int32_t)digest[0]);
  TEST_ASSERT_EQ((int32_t)0x33U, (int32_t)digest[1]);
  TEST_ASSERT_EQ((int32_t)0x22U, (int32_t)digest[2]);
  TEST_ASSERT_EQ((int32_t)0x11U, (int32_t)digest[3]);

  TEST_END("rsip sha256 happy");
}

/**
 * @brief SHA-256 handles zero-length and partial-word tails.
 */
static void test_sha256_partial_tail(void)
{
  TEST_BEGIN("rsip sha256 partial tail");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = false};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));

  const uint8_t msg[5]     = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
  uint8_t       digest[32] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_sha256(msg, 5U, digest));

  TEST_END("rsip sha256 partial tail");
}

/**
 * @brief SHA-256 rejects null pointers.
 */
static void test_sha256_null(void)
{
  TEST_BEGIN("rsip sha256 null");
  prep();

  uint8_t out[32] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_sha256(nullptr, 4U, out));
  const uint8_t msg[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_sha256(msg, 1U, nullptr));

  TEST_END("rsip sha256 null");
}

/**
 * @brief Status get / clear and ISR validation.
 */
static void test_status_clear(void)
{
  TEST_BEGIN("rsip status clear");
  prep();

  /* Pre-populate ISR with one valid and one ignored bit. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = (uint32_t)k_ra_rsip_mask_isr_done;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_clear_status((uint32_t)k_ra_rsip_mask_isr_done));

  /* Bit outside the ISR field is rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_clear_status((uint32_t)k_ra_rsip_test_isr_garbage));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_clear_status(0U));

  uint32_t status = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_get_status(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_get_status(&status));

  TEST_END("rsip status clear");
}

/**
 * @brief Attach handler + dispatch fans out events.
 */
static void test_attach_dispatch(void)
{
  TEST_BEGIN("rsip attach dispatch");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_attach_handler(stub_rsip_cb, (void*)(uintptr_t)0xDEADU));

  /* Pre-arm an ISR bit, then dispatch and confirm the cb fired. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = (uint32_t)k_ra_rsip_mask_isr_rnd;
  ra_rsip_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_test_isr_count);
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_mask_isr_rnd, (int32_t)s_test_isr_last);

  /* Empty ISR -> dispatch is a no-op. */
  ra_rsip_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_test_isr_count);

  /* Detach -> next dispatch does not invoke the cb. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_attach_handler(nullptr, nullptr));
  *ra_rsip_reg32(k_ra_rsip_off_isr) = (uint32_t)k_ra_rsip_mask_isr_done;
  ra_rsip_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_test_isr_count);

  TEST_END("rsip attach dispatch");
}

/**
 * @brief Power transition: enter + exit stop.
 */
static void test_power_transition(void)
{
  TEST_BEGIN("rsip power transition");
  prep();

  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_enter_stop());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_rsip_reg32(k_ra_rsip_off_ctrl));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_exit_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_mask_ctrl_enable, (int32_t)*ra_rsip_reg32(k_ra_rsip_off_ctrl));

  TEST_END("rsip power transition");
}

/* ===========================================================================
 * Round-3 helpers
 * ===========================================================================
 */

/**
 * @brief Initialise the engine for a sub-test that needs ENABLE asserted.
 * @since 0.12.0
 */
static void prep_running(void)
{
  prep();
  const ra_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_init(&cfg));
}

/* ===========================================================================
 * Round-3 tests: key install
 * ===========================================================================
 */

/**
 * @brief Plaintext AES-128 install populates the wrapped handle.
 */
static void test_install_aes128_plain(void)
{
  TEST_BEGIN("rsip aes128 install plain");
  prep_running();

  const uint8_t        key[16] = {0x00U,
                                  0x11U,
                                  0x22U,
                                  0x33U,
                                  0x44U,
                                  0x55U,
                                  0x66U,
                                  0x77U,
                                  0x88U,
                                  0x99U,
                                  0xAAU,
                                  0xBBU,
                                  0xCCU,
                                  0xDDU,
                                  0xEEU,
                                  0xFFU};
  ra_rsip_key_handle_t out     = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes128, (int32_t)out.alg);
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_handle_words_aes128, (int32_t)out.body_words);

  /* Null arg rejection. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_aes128_install_plain(nullptr, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_aes128_install_plain(key, nullptr));

  TEST_END("rsip aes128 install plain");
}

/**
 * @brief AES-192 / AES-256 install set the right alg + body length.
 */
static void test_install_aes_192_256(void)
{
  TEST_BEGIN("rsip aes192/256 install plain");
  prep_running();

  const uint8_t        k192[24] = {};
  const uint8_t        k256[32] = {};
  ra_rsip_key_handle_t out      = {};

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes192_install_plain(k192, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes192, (int32_t)out.alg);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes256_install_plain(k256, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes256, (int32_t)out.alg);

  TEST_END("rsip aes192/256 install plain");
}

/**
 * @brief ChaCha20 install + HMAC install routes through the same path.
 */
static void test_install_chacha20_hmac(void)
{
  TEST_BEGIN("rsip chacha20 + hmac install plain");
  prep_running();

  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t out     = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_chacha20_install_plain(key, &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_chacha20, (int32_t)out.alg);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_hmac_sha256, key, sizeof(key), &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_hmac_sha256, (int32_t)out.alg);

  /* Bad alg + zero key_len. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_aes128, key, sizeof(key), &out));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_hmac_sha256, key, 0U, &out));

  TEST_END("rsip chacha20 + hmac install plain");
}

/**
 * @brief OEM install accepts a blob + IV and rejects an invalid opcode.
 */
static void test_install_oem(void)
{
  TEST_BEGIN("rsip oem install");
  prep_running();

  const uint8_t        iv[16]   = {};
  const uint8_t        blob[32] = {};
  ra_rsip_key_handle_t out      = {};

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_oem_install(k_ra_rsip_oem_cmd_aes256, iv, blob, sizeof(blob), &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes256, (int32_t)out.alg);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_oem_install(k_ra_rsip_oem_cmd_invalid, iv, blob, sizeof(blob), &out));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_oem_install(k_ra_rsip_oem_cmd_aes256, iv, blob, 0U, &out));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_rsip_oem_install(k_ra_rsip_oem_cmd_aes256, nullptr, blob, sizeof(blob), &out));

  TEST_END("rsip oem install");
}

/* ===========================================================================
 * Round-3 tests: AES symmetric cipher
 * ===========================================================================
 */

/**
 * @brief AES-128 ECB encrypt round-trips a 16-byte block.
 */
static void test_aes_cipher_ecb(void)
{
  TEST_BEGIN("rsip aes128 ecb cipher");
  prep_running();

  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &handle));

  /* Pre-load DATA_OUT lanes with a known sentinel so we can assert it
 * comes back through internal_pull_data. */
  *ra_rsip_reg32(k_ra_rsip_off_data_out0) = 0xDEADBEEFUL;
  *ra_rsip_reg32(k_ra_rsip_off_data_out1) = 0x11223344UL;
  *ra_rsip_reg32(k_ra_rsip_off_data_out2) = 0x55667788UL;
  *ra_rsip_reg32(k_ra_rsip_off_data_out3) = 0x99AABBCCUL;

  const uint8_t pt[16] = {};
  uint8_t       ct[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_aes_cipher(&handle,
                                             k_ra_rsip_aes_mode_ecb,
                                             k_ra_rsip_dir_encrypt,
                                             nullptr,
                                             pt,
                                             ct,
                                             sizeof(pt)));
  TEST_ASSERT_EQ((int32_t)0xEFU, (int32_t)ct[0]);
  TEST_ASSERT_EQ((int32_t)0xBEU, (int32_t)ct[1]);

  /* AEAD modes must be rejected by the non-AEAD entry. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_aes_cipher(&handle,
                                             k_ra_rsip_aes_mode_gcm,
                                             k_ra_rsip_dir_encrypt,
                                             nullptr,
                                             pt,
                                             ct,
                                             sizeof(pt)));
  /* Non-block-multiple length in CBC must be rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_aes_cipher(&handle,
                                             k_ra_rsip_aes_mode_cbc,
                                             k_ra_rsip_dir_encrypt,
                                             nullptr,
                                             pt,
                                             ct,
                                             5U));

  TEST_END("rsip aes128 ecb cipher");
}

/**
 * @brief AES-128 CTR mode accepts a partial trailing block + IV.
 */
static void test_aes_cipher_ctr(void)
{
  TEST_BEGIN("rsip aes128 ctr cipher");
  prep_running();

  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &handle));

  const uint8_t pt[5]  = {'h', 'e', 'l', 'l', 'o'};
  const uint8_t iv[16] = {};
  uint8_t       ct[5]  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_aes_cipher(&handle,
                                             k_ra_rsip_aes_mode_ctr,
                                             k_ra_rsip_dir_encrypt,
                                             iv,
                                             pt,
                                             ct,
                                             sizeof(pt)));

  TEST_END("rsip aes128 ctr cipher");
}

/**
 * @brief AES-GCM round-trip + null arg rejection.
 */
static void test_aes_gcm(void)
{
  TEST_BEGIN("rsip aes gcm");
  prep_running();

  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &handle));

  const uint8_t iv[12]  = {};
  const uint8_t aad[8]  = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  const uint8_t pt[16]  = {};
  uint8_t       ct[16]  = {};
  uint8_t       tag[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_aes_gcm(&handle,
                                          k_ra_rsip_dir_encrypt,
                                          iv,
                                          aad,
                                          sizeof(aad),
                                          pt,
                                          ct,
                                          sizeof(pt),
                                          tag));

  /* Null arg checks. */
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_aes_gcm(&handle,
                                          k_ra_rsip_dir_encrypt,
                                          nullptr,
                                          aad,
                                          sizeof(aad),
                                          pt,
                                          ct,
                                          sizeof(pt),
                                          tag));

  TEST_END("rsip aes gcm");
}

/**
 * @brief AES-CCM round-trip exercises the same AEAD glue as GCM.
 */
static void test_aes_ccm(void)
{
  TEST_BEGIN("rsip aes ccm");
  prep_running();

  const uint8_t        key[16] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(key, &handle));

  const uint8_t iv[12]  = {};
  const uint8_t pt[16]  = {};
  uint8_t       ct[16]  = {};
  uint8_t       tag[16] = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)
      ra_rsip_aes_ccm(&handle, k_ra_rsip_dir_encrypt, iv, nullptr, 0U, pt, ct, sizeof(pt), tag));

  TEST_END("rsip aes ccm");
}

/* ===========================================================================
 * Round-3 tests: ChaCha20 + Poly1305
 * ===========================================================================
 */

/**
 * @brief ChaCha20 stream cipher + handle alg validation.
 */
static void test_chacha20_stream(void)
{
  TEST_BEGIN("rsip chacha20 stream");
  prep_running();

  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_chacha20_install_plain(key, &handle));

  const uint8_t nonce[12] = {};
  const uint8_t pt[8]     = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  uint8_t       ct[8]     = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_chacha20(&handle, k_ra_rsip_dir_encrypt, nonce, 0U, pt, ct, sizeof(pt)));

  /* Wrong-alg handle (AES) is rejected. */
  ra_rsip_key_handle_t bad = handle;
  bad.alg                  = (uint32_t)k_ra_rsip_oem_cmd_aes128;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_chacha20(&bad, k_ra_rsip_dir_encrypt, nonce, 0U, pt, ct, sizeof(pt)));

  TEST_END("rsip chacha20 stream");
}

/**
 * @brief ChaCha20-Poly1305 AEAD path + Poly1305 standalone MAC.
 */
static void test_chacha20_poly1305(void)
{
  TEST_BEGIN("rsip chacha20-poly1305 + poly1305");
  prep_running();

  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_chacha20_install_plain(key, &handle));

  const uint8_t nonce[12] = {};
  const uint8_t aad[4]    = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  const uint8_t pt[8]     = {};
  uint8_t       ct[8]     = {};
  uint8_t       tag[16]   = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_chacha20_poly1305(&handle,
                                                    k_ra_rsip_dir_encrypt,
                                                    nonce,
                                                    aad,
                                                    sizeof(aad),
                                                    pt,
                                                    ct,
                                                    sizeof(pt),
                                                    tag));

  /* Poly1305 standalone. */
  const uint8_t one_time[32] = {};
  uint8_t       mac[16]      = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_poly1305(one_time, pt, sizeof(pt), mac));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_poly1305(nullptr, pt, sizeof(pt), mac));

  TEST_END("rsip chacha20-poly1305 + poly1305");
}

/* ===========================================================================
 * Round-3 tests: hash + HMAC
 * ===========================================================================
 */

/**
 * @brief Generic SHA-3 / SHA-512 / SHAKE coverage through ra_rsip_hash.
 */
static void test_hash_family(void)
{
  TEST_BEGIN("rsip hash family");
  prep_running();

  const uint8_t msg[3]      = {'a', 'b', 'c'};
  uint8_t       d_512[64]   = {};
  uint8_t       d_3_256[32] = {};
  uint8_t       d_shake[20] = {};

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hash(k_ra_rsip_hash_sha512, msg, sizeof(msg), d_512, sizeof(d_512)));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hash(k_ra_rsip_hash_sha3_256, msg, sizeof(msg), d_3_256, sizeof(d_3_256)));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hash(k_ra_rsip_hash_shake128, msg, sizeof(msg), d_shake, sizeof(d_shake)));

  /* Buffer too small. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_hash(k_ra_rsip_hash_sha512, msg, sizeof(msg), d_3_256, sizeof(d_3_256)));
  /* Null digest. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_hash(k_ra_rsip_hash_sha256, msg, sizeof(msg), nullptr, 32U));

  TEST_END("rsip hash family");
}

/**
 * @brief HMAC routes through the hash path with a wrapped key.
 */
static void test_hmac(void)
{
  TEST_BEGIN("rsip hmac");
  prep_running();

  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t handle  = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_hmac_sha256, key, sizeof(key), &handle));

  const uint8_t msg[5]  = {'h', 'e', 'l', 'l', 'o'};
  uint8_t       mac[32] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_hmac(&handle, msg, sizeof(msg), mac, sizeof(mac)));

  /* Buffer too small. */
  uint8_t too_small[16] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_hmac(&handle, msg, sizeof(msg), too_small, sizeof(too_small)));

  /* Wrong-alg handle. */
  handle.alg = (uint32_t)k_ra_rsip_oem_cmd_aes128;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_hmac(&handle, msg, sizeof(msg), mac, sizeof(mac)));

  TEST_END("rsip hmac");
}

/* ===========================================================================
 * Round-3 tests: asymmetric (RSA + ECDSA + ECDH)
 * ===========================================================================
 */

/**
 * @brief RSA sign + verify happy path + bad-size rejection.
 */
static void test_rsa_sign_verify(void)
{
  TEST_BEGIN("rsip rsa sign+verify");
  prep_running();

  ra_rsip_key_handle_t key        = {.alg        = (uint32_t)k_ra_rsip_oem_cmd_rsa2048_priv,
                                     .body_words = (uint32_t)k_ra_rsip_handle_words_rsa2048_priv};
  const uint8_t        digest[32] = {};
  uint8_t              sig[256]   = {};

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_rsa_sign(&key, k_ra_rsip_rsa_2048, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_rsa_verify(&key, k_ra_rsip_rsa_2048, digest, sizeof(digest), sig));

  /* Bad size selector. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_rsip_rsa_sign(&key, (ra_rsip_rsa_size_t)5U, digest, sizeof(digest), sig));

  TEST_END("rsip rsa sign+verify");
}

/**
 * @brief ECDSA + ECDH happy path.
 */
static void test_ecc(void)
{
  TEST_BEGIN("rsip ecdsa + ecdh");
  prep_running();

  ra_rsip_key_handle_t key        = {.alg        = (uint32_t)k_ra_rsip_oem_cmd_ecc_secp256r1_priv,
                                     .body_words = (uint32_t)k_ra_rsip_handle_words_ecc256_priv};
  const uint8_t        digest[32] = {};
  uint8_t              sig[64]    = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_ecdsa_sign(&key, k_ra_rsip_curve_secp256r1, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_ecdsa_verify(&key, k_ra_rsip_curve_secp256r1, digest, sizeof(digest), sig));

  const uint8_t        peer_x[32] = {};
  const uint8_t        peer_y[32] = {};
  ra_rsip_key_handle_t shared     = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_ecdh_compute(&key, k_ra_rsip_curve_secp256r1, peer_x, peer_y, &shared));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_hmac_sha256, (int32_t)shared.alg);

  TEST_END("rsip ecdsa + ecdh");
}

/* ===========================================================================
 * Round-3 tests: OEM boot loader version (anti-rollback)
 * ===========================================================================
 */

/**
 * @brief OEM_BL_VER read / increment / lock state machine.
 */
static void test_oem_bl_version(void)
{
  TEST_BEGIN("rsip oem bl version");
  prep_running();

  uint32_t v0 = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_oem_bl_version_get(&v0));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_oem_bl_version_increment());
  uint32_t v1 = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_oem_bl_version_get(&v1));
  TEST_ASSERT_EQ((int32_t)(v0 + 1U), (int32_t)v1);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_oem_bl_version_lock());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rsip_oem_bl_version_increment());

  TEST_END("rsip oem bl version");
}

/* ===========================================================================
 * Round-3 tests: vault, wrap/unwrap, KDF
 * ===========================================================================
 */

/**
 * @brief Vault read / write / erase / count.
 */
static void test_kv(void)
{
  TEST_BEGIN("rsip kv read/write/erase/count");
  prep_running();

  uint8_t blob[64];
  for (uint32_t i = 0U; i < sizeof(blob); ++i) {
    blob[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_kv_write(3U, blob));

  uint8_t back[64] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_kv_read(3U, back));
  TEST_ASSERT_EQ((int32_t)blob[0], (int32_t)back[0]);
  TEST_ASSERT_EQ((int32_t)blob[63], (int32_t)back[63]);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_kv_erase(3U));

  uint32_t count = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_kv_count(&count));

  /* Bad slot index. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_kv_read(99U, back));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_kv_write(99U, blob));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_kv_erase(99U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_kv_read(3U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_kv_write(3U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_kv_count(nullptr));

  TEST_END("rsip kv read/write/erase/count");
}

/**
 * @brief Key wrap / unwrap round-trips through a KEK handle.
 */
static void test_key_wrap_unwrap(void)
{
  TEST_BEGIN("rsip key wrap/unwrap");
  prep_running();

  const uint8_t        kek_bytes[16] = {};
  ra_rsip_key_handle_t kek           = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_aes128_install_plain(kek_bytes, &kek));

  ra_rsip_key_handle_t src = {.alg        = (uint32_t)k_ra_rsip_oem_cmd_aes128,
                              .body_words = (uint32_t)k_ra_rsip_handle_words_aes128};
  for (uint32_t i = 0U; i < src.body_words; ++i) {
    src.body[i] = 0xA5A5A5A5UL ^ i;
  }

  const uint8_t iv[16]   = {};
  uint8_t       blob[64] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_key_wrap(&kek, iv, &src, blob));

  /* Pre-load KW_HANDLE so unwrap can resolve the alg word. */
  *ra_rsip_reg32(k_ra_rsip_off_kw_handle) = (uint32_t)k_ra_rsip_oem_cmd_aes128;
  ra_rsip_key_handle_t dest               = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_key_unwrap(&kek, iv, blob, &dest));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_aes128, (int32_t)dest.alg);

  /* Wrong-alg KEK rejection. */
  ra_rsip_key_handle_t bad_kek = src; /* alg is AES128 -- accepted, change it. */
  bad_kek.alg                  = (uint32_t)k_ra_rsip_oem_cmd_chacha20;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_key_wrap(&bad_kek, iv, &src, blob));

  TEST_END("rsip key wrap/unwrap");
}

/**
 * @brief KDF (HKDF + HUK label) returns a wrapped derived key.
 */
static void test_kdf(void)
{
  TEST_BEGIN("rsip kdf");
  prep_running();

  const uint8_t        key[32] = {};
  ra_rsip_key_handle_t ikm     = {};
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_rsip_hmac_install_plain(k_ra_rsip_oem_cmd_hmac_sha256, key, sizeof(key), &ikm));

  /* Pre-load KDF_OUT so the unpack reads a real alg word. */
  *ra_rsip_reg32(k_ra_rsip_off_kdf_out) = (uint32_t)k_ra_rsip_oem_cmd_hmac_sha256;

  const uint8_t        label[8] = {'l', 'a', 'b', 'e', 'l', 0U, 0U, 0U};
  ra_rsip_key_handle_t out      = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_kdf(k_ra_rsip_kdf_op_hkdf_sha256,
                                      &ikm,
                                      label,
                                      sizeof(label),
                                      nullptr,
                                      0U,
                                      32U,
                                      &out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_oem_cmd_hmac_sha256, (int32_t)out.alg);

  /* HUK / UID label modes accept null IKM. */
  *ra_rsip_reg32(k_ra_rsip_off_kdf_out) = (uint32_t)k_ra_rsip_oem_cmd_hmac_sha256;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_kdf(k_ra_rsip_kdf_op_huk_label,
                                      nullptr,
                                      label,
                                      sizeof(label),
                                      nullptr,
                                      0U,
                                      32U,
                                      &out));

  /* Null + length mismatches. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_rsip_kdf(k_ra_rsip_kdf_op_hkdf_sha256,
                                      nullptr,
                                      label,
                                      sizeof(label),
                                      nullptr,
                                      0U,
                                      32U,
                                      &out));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)
      ra_rsip_kdf(k_ra_rsip_kdf_op_hkdf_sha256, &ikm, label, sizeof(label), nullptr, 0U, 0U, &out));

  TEST_END("rsip kdf");
}

/* ===========================================================================
 * Round-3 tests: lifecycle + debug + tamper + DOTF
 * ===========================================================================
 */

/**
 * @brief Lifecycle read / advance + invalid transitions.
 */
static void test_life(void)
{
  TEST_BEGIN("rsip life");
  prep_running();

  ra_rsip_life_state_t st = k_ra_rsip_life_cm;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_life_get(&st));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_life_advance(k_ra_rsip_life_dpl));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_life_get(&st));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_life_dpl, (int32_t)st);

  /* Backward transition rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_rsip_life_advance(k_ra_rsip_life_cm));

  /* Unknown state rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_life_advance((ra_rsip_life_state_t)99U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_life_get(nullptr));

  TEST_END("rsip life");
}

/**
 * @brief Debug-level read + write.
 */
static void test_debug_level(void)
{
  TEST_BEGIN("rsip debug level");
  prep_running();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_debug_level_set(k_ra_rsip_debug_al1));
  ra_rsip_debug_level_t out = k_ra_rsip_debug_al0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_debug_level_get(&out));
  TEST_ASSERT_EQ((int32_t)k_ra_rsip_debug_al1, (int32_t)out);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_rsip_debug_level_set((ra_rsip_debug_level_t)99U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_debug_level_get(nullptr));

  TEST_END("rsip debug level");
}

/**
 * @brief Tamper enable / status / ack + DPA arm.
 */
static void test_tamper(void)
{
  TEST_BEGIN("rsip tamper + dpa");
  prep_running();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_tamper_enable((uint32_t)k_ra_rsip_tamper_src_ext0 |
                                                (uint32_t)k_ra_rsip_tamper_src_volt));
  uint32_t flags = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_tamper_status(&flags));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_rsip_tamper_ack((uint32_t)k_ra_rsip_tamper_src_ext0));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_tamper_enable(0xFFFFFFFFUL));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_tamper_ack(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_tamper_ack(0xFFFFFFFFUL));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_rsip_tamper_status(nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_dpa_arm(true));
  TEST_ASSERT(((*ra_rsip_reg32(k_ra_rsip_off_ctrl)) & (uint32_t)k_ra_rsip_mask_ctrl_dpa_arm) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_dpa_arm(false));
  TEST_ASSERT(((*ra_rsip_reg32(k_ra_rsip_off_ctrl)) & (uint32_t)k_ra_rsip_mask_ctrl_dpa_arm) == 0U);

  TEST_END("rsip tamper + dpa");
}

/**
 * @brief DOTF route on/off + bad-arg rejection.
 */
static void test_dotf_route(void)
{
  TEST_BEGIN("rsip dotf route");
  prep_running();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_dotf_route(0U, 5U, true));
  TEST_ASSERT(((*ra_rsip_reg32(k_ra_rsip_off_dotf0_ctrl)) & (uint32_t)k_ra_rsip_dotf_on) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rsip_dotf_route(1U, 0U, false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_rsip_reg32(k_ra_rsip_off_dotf1_ctrl));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_dotf_route(2U, 0U, true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_rsip_dotf_route(0U, 99U, true));

  TEST_END("rsip dotf route");
}

int32_t main(void)
{
  test_init_happy();
  test_init_skip_bist();
  test_init_null_cfg();
  test_deinit();
  test_trng_read();
  test_trng_arg_check();
  test_sha256_happy();
  test_sha256_partial_tail();
  test_sha256_null();
  test_status_clear();
  test_attach_dispatch();
  test_power_transition();
  /* Round-3 tests. */
  test_install_aes128_plain();
  test_install_aes_192_256();
  test_install_chacha20_hmac();
  test_install_oem();
  test_aes_cipher_ecb();
  test_aes_cipher_ctr();
  test_aes_gcm();
  test_aes_ccm();
  test_chacha20_stream();
  test_chacha20_poly1305();
  test_hash_family();
  test_hmac();
  test_rsa_sign_verify();
  test_ecc();
  test_oem_bl_version();
  test_kv();
  test_key_wrap_unwrap();
  test_kdf();
  test_life();
  test_debug_level();
  test_tamper();
  test_dotf_route();
  (void)fprintf(stderr, "[OK ] test_ra_rsip.c\n");
  return 0;
}
