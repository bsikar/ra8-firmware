/**
 * @file test_ra8_rsip_devsec.c
 * @brief Unit tests for the RSIP device-security surface (asym / vault / lifecycle)
 *
 * @details
 * Split sibling of the original test_ra8_rsip.c suite covering the
 * device-security surface of ra8_rsip.c against the
 * ``ra8_sim_mmap``-backed register window:
 *
 * - RSA sign / verify / encrypt / decrypt incl. engine-error
 * forwarding and the 4-condition size-selector MC/DC vectors;
 * - ECDSA sign / verify + ECDH shared-secret derivation;
 * - OEM boot-loader anti-rollback version state machine;
 * - key vault read / write / erase / count over the real MMIO
 * sequence, key wrap / unwrap, and the KDF (HKDF + HUK label)
 * incl. its 4-condition MC/DC vectors;
 * - lifecycle advance, debug level, tamper sources + DPA arm, and
 * DOTF routing.
 *
 * Sibling suites: test_ra8_rsip_core.c (engine lifecycle + SHA-256)
 * and test_ra8_rsip_sym.c (key install + symmetric ciphers).
 *
 * Each test resets ``ra8_sim_mmap``, ``ra8_sim_mmio`` and ``ra8_mstp``
 * first so cases stay independent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

/**
 * @enum rsip_devsec_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rsip_devsec_v0_ff  = 0xFFU,
  k_rsip_devsec_val_64 = 64,
} rsip_devsec_uint8_const_t;

/**
 * @enum rsip_devsec_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rsip_devsec_val_256  = 256,
  k_rsip_devsec_val_4096 = 4096,
} rsip_devsec_uint16_const_t;

/**
 * @enum rsip_devsec_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_rsip_devsec_body_a5a5a5a5 = 0xA5A5A5A5UL,
} rsip_devsec_uint32_const_t;

/**
 * @enum ra8_rsip_test_const_t
 * @brief Magic numbers used by the tests, named to keep the
 * no-magic-numbers rule satisfied.
 */
typedef enum : uint32_t {
  k_ra8_rsip_test_kv_fill   = 0x7CU,        /**< Staged KV_DATA fill byte. */
  k_ra8_rsip_test_kv_fill_w = 0x7C7C7C7CUL, /**< Fill byte in all 4 lanes. */
  k_ra8_rsip_test_kv_tail_w = 0x3F3E3D3CUL, /**< LE pack of bytes 60..63 of a
                                               *   0,1,2,... ramp: the value the
                                               *   single-address KV_DATA port
                                               *   holds after 16 overlapping
                                               *   FIFO writes on dumb RAM.     */
} ra8_rsip_test_const_t;

/**
 * @brief Reset the world before each test.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Initialise the engine for a sub-test that needs ENABLE asserted.
 * @since 0.1.0
 */
static void prep_running(void)
{
  prep();
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
}

/**
 * @brief One-vector ra8_rsip_kdf call with the fixed label and 32-byte output.
 *
 * @details Exists so each MC/DC vector reads as one source line under
 * the NASA Rule 4 function-size cap; only the operation selector and
 * the IKM handle vary across the vectors.
 * @since 0.1.0
 */
static ra8_err_t
kdf_vec(ra8_rsip_kdf_op_t op, const ra8_rsip_key_handle_t* ikm, ra8_rsip_key_handle_t* out)
{
  const uint8_t label[8] = {'l', 'a', 'b', 'e', 'l', 0U, 0U, 0U};
  return ra8_rsip_kdf(op, ikm, label, sizeof(label), nullptr, 0U, 32U, out);
}

/* ===========================================================================
 * Round-3 tests: asymmetric (RSA + ECDSA + ECDH)
 * ===========================================================================
 */

/**
 * @brief RSA sign + verify happy path + bad-size rejection.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_rsa_sign_verify(void)
{
  TEST_BEGIN("rsip rsa sign+verify");
  prep_running();

  ra8_rsip_key_handle_t key        = {.alg        = (uint32_t)k_ra8_rsip_oem_cmd_rsa2048_priv,
                                      .body_words = (uint32_t)k_ra8_rsip_handle_words_rsa2048_priv};
  const uint8_t         digest[32] = {};
  uint8_t               sig[k_rsip_devsec_val_256] = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_2048, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_rsa_verify(&key, k_ra8_rsip_rsa_2048, digest, sizeof(digest), sig));

  /* Bad size selector. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_rsa_sign(&key, (ra8_rsip_rsa_size_t)5U, digest, sizeof(digest), sig));

  TEST_END("rsip rsa sign+verify");
}

/**
 * @brief RSA sign / encrypt / decrypt forward an engine-side completion error.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- forces ``internal_complete`` to read
  * a non-zero MBOX_RET so each RSA entry point takes its single-condition
  * ``err != k_ra8_ok`` forward-the-error branch)
 */
static void test_rsa_engine_error_paths(void)
{
  TEST_BEGIN("rsip rsa sign/encrypt/decrypt forward engine error");
  prep_running();

  ra8_rsip_key_handle_t key        = {.alg        = (uint32_t)k_ra8_rsip_oem_cmd_rsa2048_priv,
                                      .body_words = (uint32_t)k_ra8_rsip_handle_words_rsa2048_priv};
  const uint8_t         digest[32] = {};
  uint8_t               sig[k_rsip_devsec_val_256] = {};

  /* A non-zero MBOX_RET makes internal_complete report k_ra8_err_hw_error;
   * each RSA entry must propagate it unchanged. */
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret) = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_2048, digest, sizeof(digest), sig));

  const uint8_t pt[256]                    = {};
  uint8_t       ct[k_rsip_devsec_val_256]  = {};
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret) = 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    ra8_rsip_rsa_encrypt(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, pt, sizeof(pt), ct));

  uint8_t  recovered[k_rsip_devsec_val_256] = {};
  uint32_t recovered_len                    = 0U;
  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret)  = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rsip_rsa_decrypt(&key,
                                      k_ra8_rsip_rsa_2048,
                                      k_ra8_rsip_rsa_pad_oaep,
                                      ct,
                                      recovered,
                                      sizeof(recovered),
                                      &recovered_len));

  TEST_END("rsip rsa sign/encrypt/decrypt forward engine error");
}

/**
 * @brief ECDSA + ECDH happy path.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_ecc(void)
{
  TEST_BEGIN("rsip ecdsa + ecdh");
  prep_running();

  ra8_rsip_key_handle_t key        = {.alg        = (uint32_t)k_ra8_rsip_oem_cmd_ecc_secp256r1_priv,
                                      .body_words = (uint32_t)k_ra8_rsip_handle_words_ecc256_priv};
  const uint8_t         digest[32] = {};
  uint8_t               sig[k_rsip_devsec_val_64] = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_ecdsa_sign(&key, k_ra8_rsip_curve_secp256r1, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_ecdsa_verify(&key, k_ra8_rsip_curve_secp256r1, digest, sizeof(digest), sig));

  const uint8_t         peer_x[32] = {};
  const uint8_t         peer_y[32] = {};
  ra8_rsip_key_handle_t shared     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_ecdh_compute(&key, k_ra8_rsip_curve_secp256r1, peer_x, peer_y, &shared));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_hmac_sha256, shared.alg);

  TEST_END("rsip ecdsa + ecdh");
}

/* ===========================================================================
 * Round-3 tests: OEM boot loader version (anti-rollback)
 * ===========================================================================
 */

/**
 * @brief OEM_BL_VER read / increment / lock state machine.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_oem_bl_version(void)
{
  TEST_BEGIN("rsip oem bl version");
  prep_running();

  uint32_t v0 = k_rsip_devsec_v0_ff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_oem_bl_version_get(&v0));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_oem_bl_version_increment());
  uint32_t v1 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_oem_bl_version_get(&v1));
  TEST_ASSERT_EQ((v0 + 1U), v1);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_oem_bl_version_lock());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_oem_bl_version_increment());

  TEST_END("rsip oem bl version");
}

/* ===========================================================================
 * Round-3 tests: vault, wrap/unwrap, KDF
 * ===========================================================================
 */

/**
 * @brief Vault read / write / erase / count drive the real MMIO sequence.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_kv(void)
{
  TEST_BEGIN("rsip kv read/write/erase/count");
  prep_running();

  uint8_t blob[k_rsip_devsec_val_64];
  for (uint32_t i = 0U; i < sizeof(blob); ++i) {
    blob[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_kv_write(3U, blob));

  /* Real sequence: 16 LE words streamed into the single-address
   * KV_DATA port (on dumb RAM the last word wins), then the slot /
   * command / mailbox operands. The driver keeps no shadow copy. */
  TEST_ASSERT_EQ(k_ra8_rsip_test_kv_tail_w, *ra8_rsip_reg32(k_ra8_rsip_off_kv_data));
  TEST_ASSERT_EQ(3U, *ra8_rsip_reg32(k_ra8_rsip_off_kv_slot));
  TEST_ASSERT_EQ(k_ra8_rsip_kv_op_write, *ra8_rsip_reg32(k_ra8_rsip_off_kv_ctrl));
  TEST_ASSERT_EQ(k_ra8_rsip_kv_op_write, *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op));

  /* Read decode: stage the word the engine would present on KV_DATA
   * and verify the driver's real 16-word drain + LE unpack. */
  *ra8_rsip_reg32(k_ra8_rsip_off_kv_data) = (uint32_t)k_ra8_rsip_test_kv_fill_w;
  uint8_t back[k_rsip_devsec_val_64]      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_kv_read(3U, back));
  TEST_ASSERT_EQ(k_ra8_rsip_kv_op_read, *ra8_rsip_reg32(k_ra8_rsip_off_kv_ctrl));
  for (uint32_t i = 0U; i < sizeof(back); ++i) {
    TEST_ASSERT_EQ(k_ra8_rsip_test_kv_fill, back[i]);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_kv_erase(3U));
  TEST_ASSERT_EQ(k_ra8_rsip_kv_op_erase, *ra8_rsip_reg32(k_ra8_rsip_off_kv_ctrl));

  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_kv_count(&count));

  /* Bad slot index. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_kv_read(99U, back));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_kv_write(99U, blob));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_kv_erase(99U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_kv_read(3U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_kv_write(3U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_kv_count(nullptr));

  TEST_END("rsip kv read/write/erase/count");
}

/**
 * @brief Vault read forwards an engine-side completion error.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- a non-zero MBOX_RET makes
  * ``internal_complete`` report ``k_ra8_err_hw_error`` and
  * ``ra8_rsip_kv_read`` takes its single-condition forward-the-error
  * branch before touching the data port)
 */
static void test_kv_engine_error(void)
{
  TEST_BEGIN("rsip kv engine error");
  prep_running();

  *ra8_rsip_reg32(k_ra8_rsip_off_mbox_ret) = 1U;
  uint8_t back[k_rsip_devsec_val_64]       = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_rsip_kv_read(3U, back));
  /* The failed command must not have drained the data port into the
   * caller's buffer. */
  TEST_ASSERT_EQ(0x00U, back[0]);

  TEST_END("rsip kv engine error");
}

/**
 * @brief Key wrap / unwrap round-trips through a KEK handle.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_key_wrap_unwrap(void)
{
  TEST_BEGIN("rsip key wrap/unwrap");
  prep_running();

  const uint8_t         kek_bytes[16] = {};
  ra8_rsip_key_handle_t kek           = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_aes128_install_plain(kek_bytes, &kek));

  ra8_rsip_key_handle_t src = {.alg        = (uint32_t)k_ra8_rsip_oem_cmd_aes128,
                               .body_words = (uint32_t)k_ra8_rsip_handle_words_aes128};
  for (uint32_t i = 0U; i < src.body_words; ++i) {
    src.body[i] = k_rsip_devsec_body_a5a5a5a5 ^ i;
  }

  const uint8_t iv[16]                     = {};
  uint8_t       blob[k_rsip_devsec_val_64] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_wrap(&kek, iv, &src, blob));

  /* Pre-load KW_HANDLE so unwrap can resolve the alg word. */
  *ra8_rsip_reg32(k_ra8_rsip_off_kw_handle) = (uint32_t)k_ra8_rsip_oem_cmd_aes128;
  ra8_rsip_key_handle_t dest                = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_key_unwrap(&kek, iv, blob, &dest));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_aes128, dest.alg);

  /* Wrong-alg KEK rejection. */
  ra8_rsip_key_handle_t bad_kek = src; /* alg is AES128 -- accepted, change it. */
  bad_kek.alg                   = (uint32_t)k_ra8_rsip_oem_cmd_chacha20;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_key_wrap(&bad_kek, iv, &src, blob));

  TEST_END("rsip key wrap/unwrap");
}

/**
 * @brief KDF (HKDF + HUK label) returns a wrapped derived key.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_kdf(void)
{
  TEST_BEGIN("rsip kdf");
  prep_running();

  const uint8_t         key[32] = {};
  ra8_rsip_key_handle_t ikm     = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha256, key, sizeof(key), &ikm));

  /* Pre-load KDF_OUT so the unpack reads a real alg word. */
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out) = (uint32_t)k_ra8_rsip_oem_cmd_hmac_sha256;

  const uint8_t         label[8] = {'l', 'a', 'b', 'e', 'l', 0U, 0U, 0U};
  ra8_rsip_key_handle_t out      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_kdf(k_ra8_rsip_kdf_op_hkdf_sha256,
                              &ikm,
                              label,
                              sizeof(label),
                              nullptr,
                              0U,
                              32U,
                              &out));
  TEST_ASSERT_EQ(k_ra8_rsip_oem_cmd_hmac_sha256, out.alg);

  /* HUK / UID label modes accept null IKM. */
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out) = (uint32_t)k_ra8_rsip_oem_cmd_hmac_sha256;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_kdf(k_ra8_rsip_kdf_op_huk_label,
                              nullptr,
                              label,
                              sizeof(label),
                              nullptr,
                              0U,
                              32U,
                              &out));

  /* Null + length mismatches. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rsip_kdf(k_ra8_rsip_kdf_op_hkdf_sha256,
                              nullptr,
                              label,
                              sizeof(label),
                              nullptr,
                              0U,
                              32U,
                              &out));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,

    ra8_rsip_kdf(k_ra8_rsip_kdf_op_hkdf_sha256, &ikm, label, sizeof(label), nullptr, 0U, 0U, &out));

  TEST_END("rsip kdf");
}

/* ===========================================================================
 * Round-3 tests: lifecycle + debug + tamper + DOTF
 * ===========================================================================
 */

/**
 * @brief Lifecycle read / advance + invalid transitions.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_life(void)
{
  TEST_BEGIN("rsip life");
  prep_running();

  ra8_rsip_life_state_t st = k_ra8_rsip_life_cm;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_life_get(&st));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_life_advance(k_ra8_rsip_life_dpl));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_life_get(&st));
  TEST_ASSERT_EQ(k_ra8_rsip_life_dpl, st);

  /* Backward transition rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_rsip_life_advance(k_ra8_rsip_life_cm));

  /* Unknown state rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_life_advance((ra8_rsip_life_state_t)99U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_life_get(nullptr));

  TEST_END("rsip life");
}

/**
 * @brief Debug-level read + write.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_debug_level(void)
{
  TEST_BEGIN("rsip debug level");
  prep_running();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_debug_level_set(k_ra8_rsip_debug_al1));
  ra8_rsip_debug_level_t out = k_ra8_rsip_debug_al0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_debug_level_get(&out));
  TEST_ASSERT_EQ(k_ra8_rsip_debug_al1, out);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_debug_level_set((ra8_rsip_debug_level_t)99U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_debug_level_get(nullptr));

  TEST_END("rsip debug level");
}

/**
 * @brief Tamper enable / status / ack + DPA arm.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_tamper(void)
{
  TEST_BEGIN("rsip tamper + dpa");
  prep_running();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rsip_tamper_enable((uint32_t)k_ra8_rsip_tamper_src_ext0 |
                                        (uint32_t)k_ra8_rsip_tamper_src_volt));
  uint32_t flags = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_tamper_status(&flags));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_tamper_ack((uint32_t)k_ra8_rsip_tamper_src_ext0));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_tamper_enable(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_tamper_ack(0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_tamper_ack(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_tamper_status(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_dpa_arm(true));
  TEST_ASSERT(((*ra8_rsip_reg32(k_ra8_rsip_off_ctrl)) & (uint32_t)k_ra8_rsip_mask_ctrl_dpa_arm) !=
              0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_dpa_arm(false));
  TEST_ASSERT(((*ra8_rsip_reg32(k_ra8_rsip_off_ctrl)) & (uint32_t)k_ra8_rsip_mask_ctrl_dpa_arm) ==
              0U);

  TEST_END("rsip tamper + dpa");
}

/**
 * @brief DOTF route on/off + bad-arg rejection.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_dotf_route(void)
{
  TEST_BEGIN("rsip dotf route");
  prep_running();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_dotf_route(0U, 5U, true));
  TEST_ASSERT(((*ra8_rsip_reg32(k_ra8_rsip_off_dotf0_ctrl)) & (uint32_t)k_ra8_rsip_dotf_on) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_dotf_route(1U, 0U, false));
  TEST_ASSERT_EQ(0, *ra8_rsip_reg32(k_ra8_rsip_off_dotf1_ctrl));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_dotf_route(2U, 0U, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_dotf_route(0U, 99U, true));

  TEST_END("rsip dotf route");
}

/* ---------------------------------------------------------------------------
 * MC/DC vector tests
 * ------------------------------------------------------------------------ */

/**
 * @test test_mcdc_rsa_sign_size_quad
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_rsip.c ra8_rsip_rsa_sign):
 * 4-cond AND-NOT chain:
 * ``size != 1024 && size != 2048 && size != 3072 && size != 4096``
 * Short-circuit MC/DC: N+1 = 5 vectors. Each accepted size must produce F at
 * the position where it short-circuits to F; one out-of-range value drives
 * all four conditions T -> dec T -> invalid_arg.
 * - V1: size=1024  -> C1=F short -> dec F (proceeds; downstream may fail)
 * - V2: size=2048  -> C1=T, C2=F -> dec F
 * - V3: size=3072  -> C1=T, C2=T, C3=F -> dec F
 * - V4: size=4096  -> C1=T, C2=T, C3=T, C4=F -> dec F
 * - V5: size=999   -> all T -> dec T -> invalid_arg (independence anchor for all)
 */
static void test_mcdc_rsa_sign_size_quad(void)
{
  TEST_BEGIN("rsip rsa_sign MC/DC: 4-cond size selector");
  prep_running();
  ra8_rsip_key_handle_t key                     = {};
  key.alg                                       = (uint32_t)k_ra8_rsip_sym_alg_aes128;
  const uint8_t digest[64]                      = {};
  uint8_t       sig[k_rsip_devsec_val_4096 / 8] = {};
  /* V1..V4: each accepted size; downstream may fail with non-real key, only
   * the size-decision branch matters for MC/DC. */
  (void)ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_1024, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_2048, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_3072, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_sign(&key, k_ra8_rsip_rsa_4096, digest, sizeof(digest), sig);
  /* V5: bogus size -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_rsa_sign(&key, (ra8_rsip_rsa_size_t)999U, digest, sizeof(digest), sig));
  TEST_END("rsip rsa_sign MC/DC: 4-cond size selector");
}

/**
 * @test test_mcdc_rsa_verify_size_quad
 *
 * @par MC/DC:
 * Same shape as test_mcdc_rsa_sign_size_quad, applied to
 * ra8_rsip_rsa_verify (libs/ra8_hal/src/ra8_rsip.c). N+1 = 5.
 */
static void test_mcdc_rsa_verify_size_quad(void)
{
  TEST_BEGIN("rsip rsa_verify MC/DC: 4-cond size selector");
  prep_running();
  ra8_rsip_key_handle_t key                     = {};
  key.alg                                       = (uint32_t)k_ra8_rsip_sym_alg_aes128;
  const uint8_t digest[64]                      = {};
  uint8_t       sig[k_rsip_devsec_val_4096 / 8] = {};
  (void)ra8_rsip_rsa_verify(&key, k_ra8_rsip_rsa_1024, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_verify(&key, k_ra8_rsip_rsa_2048, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_verify(&key, k_ra8_rsip_rsa_3072, digest, sizeof(digest), sig);
  (void)ra8_rsip_rsa_verify(&key, k_ra8_rsip_rsa_4096, digest, sizeof(digest), sig);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_rsa_verify(&key, (ra8_rsip_rsa_size_t)999U, digest, sizeof(digest), sig));
  TEST_END("rsip rsa_verify MC/DC: 4-cond size selector");
}

/**
 * @test test_mcdc_kdf_hkdf_ikm_required_quad
 *
 * @par MC/DC:
 * Decision: ``if (((op == HKDF_SHA256) || (op == HKDF_SHA384) ||
 *               (op == HKDF_SHA512)) && (ikm == NULL))``
 * (4 conditions, libs/ra8_hal/src/ra8_rsip.c internal_kdf_validate).
 * Reached via the public ra8_rsip_kdf entry point.
 * Short-circuit AND-of-OR; minimal MC/DC = N+1 = 5 vectors:
 *  - V1: op=HKDF256, ikm=NULL -> C1=T short, C4=T -> dec T (null_ptr). [C1 indep]
 *  - V2: op=HKDF384, ikm=NULL -> C1=F,C2=T,C4=T -> dec T.              [C2 indep]
 *  - V3: op=HKDF512, ikm=NULL -> C1=F,C2=F,C3=T,C4=T -> dec T.         [C3 indep]
 *  - V4: op=HUK_LBL, ikm=NULL -> outer-OR all-F -> dec F.              [outer-OR all-F]
 *  - V5: op=HKDF256, ikm!=NULL -> C1=T,C4=F -> dec F.                  [C4 indep]
 *
 * V1 vs V4 isolate C1 + outer-OR; V2 vs V4 isolate C2; V3 vs V4 isolate C3;
 * V1 vs V5 isolate C4 (ikm pointer).
 */
static void test_mcdc_kdf_hkdf_ikm_required_quad(void)
{
  TEST_BEGIN("rsip kdf MC/DC: (HKDF256||HKDF384||HKDF512) && ikm==NULL");
  prep_running();

  const uint8_t         hmac_key[32] = {};
  ra8_rsip_key_handle_t ikm          = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_hmac_install_plain(k_ra8_rsip_oem_cmd_hmac_sha256, hmac_key, sizeof(hmac_key), &ikm));

  ra8_rsip_key_handle_t out = {};

  /* V1: HKDF-SHA256 + ikm==NULL -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, kdf_vec(k_ra8_rsip_kdf_op_hkdf_sha256, nullptr, &out));

  /* V2: HKDF-SHA384 + ikm==NULL -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, kdf_vec(k_ra8_rsip_kdf_op_hkdf_sha384, nullptr, &out));

  /* V3: HKDF-SHA512 + ikm==NULL -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, kdf_vec(k_ra8_rsip_kdf_op_hkdf_sha512, nullptr, &out));

  /* V4: HUK_LABEL + ikm==NULL -> outer-OR all-false -> proceed (OK). */
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out) = (uint32_t)k_ra8_rsip_oem_cmd_hmac_sha256;
  TEST_ASSERT_EQ(k_ra8_ok, kdf_vec(k_ra8_rsip_kdf_op_huk_label, nullptr, &out));

  /* V5: HKDF-SHA256 + valid ikm -> C4=F -> proceed (OK). */
  *ra8_rsip_reg32(k_ra8_rsip_off_kdf_out) = (uint32_t)k_ra8_rsip_oem_cmd_hmac_sha256;
  TEST_ASSERT_EQ(k_ra8_ok, kdf_vec(k_ra8_rsip_kdf_op_hkdf_sha256, &ikm, &out));

  TEST_END("rsip kdf MC/DC: (HKDF256||HKDF384||HKDF512) && ikm==NULL");
}

int32_t main(void)
{
  test_rsa_sign_verify();
  test_rsa_engine_error_paths();
  test_ecc();
  test_oem_bl_version();
  test_kv();
  test_kv_engine_error();
  test_key_wrap_unwrap();
  test_kdf();
  test_life();
  test_debug_level();
  test_tamper();
  test_dotf_route();
  test_mcdc_rsa_sign_size_quad();
  test_mcdc_rsa_verify_size_quad();
  test_mcdc_kdf_hkdf_ikm_required_quad();
  (void)fprintf(stderr, "[OK ] test_ra8_rsip_devsec.c\n");
  return 0;
}
