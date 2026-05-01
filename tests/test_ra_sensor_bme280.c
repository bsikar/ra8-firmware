/**
 * @file test_ra_sensor_bme280.c
 * @brief Unit tests for the Bosch BME280 driver (ra_sensor_bme280)
 *
 * @details
 * Host-compiled tests covering:
 *
 *   1. Lifecycle (init / deinit / double-init / pre-init access).
 *   2. Argument validation on the public API.
 *   3. Bosch datasheet rev 1.6 sec 4.2.3 known-answer compensation:
 *      dig_T1=27504, dig_T2=26435, dig_T3=-1000, adc_T=519888 ->
 *      T=25.08 degC (DegC*100 == 2508; millidegrees == 25080).
 *   4. Calibration block decode against a hand-built byte array using
 *      the test-only ``ra_sensor_bme280_test_decode_calib`` hook.
 *
 * No real I2C traffic happens; the IIC_B path is exercised through
 * ``RA_SIMULATOR_MODE`` short-circuits inside the driver, the same
 * pattern used by ``test_ra_touch.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_iic_b_regs.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_mstp.h"
#include "ra_sensor_bme280.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_bme280_test_const_t
 * @brief Numeric constants used across the test suite.
 */
typedef enum : uint32_t {
  k_test_ch0 = 0U,        /**< Only IIC_B channel.            */
  k_test_byte_shift = 8U, /**< Bits per byte.                 */
  /* Bosch datasheet 4.2.3 known-answer vectors. */
  k_test_dig_T1 = 27504U,
  k_test_adc_T = 519888U,
  k_test_expected_T_x100 = 2508U, /**< 25.08 degC * 100.  */
  k_test_expected_T_mc = 25080U,  /**< Millidegrees C.    */
  k_test_T_mc_tolerance = 50U,    /**< +-50 mdegC.        */
} ra_bme280_test_const_t;

/**
 * @enum ra_bme280_test_signed_t
 * @brief Signed datasheet constants (must be a separate enum since the
 *        unsigned-base enum above cannot host int16_t values).
 */
typedef enum : int16_t {
  k_test_dig_T2 = 26435,
  k_test_dig_T3 = -1000,
} ra_bme280_test_signed_t;

/**
 * @brief Pre-arm IIC_B status registers so polling helpers fall through.
 *
 * @details
 * Mirrors the prime_iic_b() helper in ``test_ra_touch.c``: NTST gets
 * the "TX buffer empty" + "RX buffer full" bits set and BCST gets the
 * "bus free" bit set so the wait loops exit immediately.
 */
static void prime_iic_b(void) {
  volatile r_iic_b_regs_t *reg = ra_iic_b(0U);
  reg->NTST = (uint32_t)k_ra_iic_b_msk_ntst_tdbef0 |
              (uint32_t)k_ra_iic_b_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra_iic_b_msk_bcst_bfref;
}

/**
 * @brief Reset simulator + MSTP before each lifecycle test.
 */
static void prep(void) {
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  prime_iic_b();
}

/**
 * @brief Standard configuration used for happy-path tests.
 */
static const ra_sensor_bme280_cfg_t k_cfg_default = {
    .i2c_channel = (uint8_t)k_test_ch0,
    .target_7b = (uint8_t)k_ra_sensor_bme280_addr_low,
    .osrs_t = k_ra_sensor_bme280_osrs_x2,
    .osrs_p = k_ra_sensor_bme280_osrs_x16,
    .osrs_h = k_ra_sensor_bme280_osrs_x1,
    .filter = k_ra_sensor_bme280_filter_16,
    .standby = k_ra_sensor_bme280_sb_500_ms,
};

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief init -> deinit cycles cleanly.
 */
static void test_init_deinit_happy(void) {
  TEST_BEGIN("test_init_deinit_happy");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_init(&k_cfg_default));
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_deinit());
  TEST_END("test_init_deinit_happy");
}

/**
 * @brief Double-init returns invalid_state.
 */
static void test_double_init_rejected(void) {
  TEST_BEGIN("test_double_init_rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_init(&k_cfg_default));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_sensor_bme280_init(&k_cfg_default));
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_deinit());
  TEST_END("test_double_init_rejected");
}

/**
 * @brief NULL config is rejected.
 */
static void test_init_null_cfg(void) {
  TEST_BEGIN("test_init_null_cfg");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sensor_bme280_init(NULL));
  TEST_END("test_init_null_cfg");
}

/**
 * @brief Out-of-range channel and address are rejected.
 */
static void test_init_invalid_args(void) {
  TEST_BEGIN("test_init_invalid_args");
  prep();
  ra_sensor_bme280_cfg_t bad = k_cfg_default;
  bad.i2c_channel = 9U; /* RA8D2 has only channel 0. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sensor_bme280_init(&bad));

  bad = k_cfg_default;
  bad.target_7b = 0x42U; /* Not the BME280 address. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_sensor_bme280_init(&bad));
  TEST_END("test_init_invalid_args");
}

/**
 * @brief Pre-init reads / chip-id calls return not_initialized.
 */
static void test_pre_init_access(void) {
  TEST_BEGIN("test_pre_init_access");
  prep();
  uint8_t id = 0U;
  int32_t t = 0;
  int32_t h = 0;
  uint32_t p = 0U;
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_sensor_bme280_get_chip_id(&id));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_sensor_bme280_read(&t, &h, &p));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_sensor_bme280_deinit());
  TEST_END("test_pre_init_access");
}

/**
 * @brief Public API rejects NULL output pointers.
 */
static void test_null_outputs_rejected(void) {
  TEST_BEGIN("test_null_outputs_rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_init(&k_cfg_default));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sensor_bme280_get_chip_id(NULL));
  int32_t t = 0;
  int32_t h = 0;
  uint32_t p = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sensor_bme280_read(NULL, &h, &p));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sensor_bme280_read(&t, NULL, &p));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_sensor_bme280_read(&t, &h, NULL));
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_deinit());
  TEST_END("test_null_outputs_rejected");
}

/**
 * @brief get_chip_id is callable post-init (host returns 0 on the
 *        simulated bus, which is fine -- we just want a clean rc).
 */
static void test_get_chip_id_post_init(void) {
  TEST_BEGIN("test_get_chip_id_post_init");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_init(&k_cfg_default));
  uint8_t id = 0xFFU;
  /* Drain any sim-side flags then re-prime so the read transaction can
   * complete. */
  prime_iic_b();
  const ra_err_t rc = ra_sensor_bme280_get_chip_id(&id);
  /* Either k_ra_ok (transfer accepted by sim) or k_ra_err_i2c_error
   * (sim cleared the prime-flags). Both prove the not_initialized
   * branch isn't reachable post-init. */
  TEST_ASSERT(rc == k_ra_ok || rc == k_ra_err_i2c_error);
  TEST_ASSERT_EQ(k_ra_ok, ra_sensor_bme280_deinit());
  TEST_END("test_get_chip_id_post_init");
}

/* =============================================================================
 * Calibration decode and Bosch known-answer compensation
 * =============================================================================
 */

/**
 * @brief Pack a signed 16-bit value into LSB-first bytes.
 */
static void pack_s16le(int16_t v, uint8_t *p) {
  p[0] = (uint8_t)((uint16_t)v & 0xFFU);
  p[1] = (uint8_t)(((uint16_t)v >> 8U) & 0xFFU);
}

/**
 * @brief Pack an unsigned 16-bit value into LSB-first bytes.
 */
static void pack_u16le(uint16_t v, uint8_t *p) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

/**
 * @brief Build a 26-byte 0x88..0xA1 calibration block carrying just
 *        the T1/T2/T3 fields needed for the datasheet known-answer.
 */
static void build_calib_block(uint8_t buf_88[26]) {
  memset(buf_88, 0, 26U);
  pack_u16le((uint16_t)k_test_dig_T1, &buf_88[0]);
  pack_s16le((int16_t)k_test_dig_T2, &buf_88[2]);
  pack_s16le((int16_t)k_test_dig_T3, &buf_88[4]);
}

/**
 * @brief Decode pipeline reproduces dig_T1 / dig_T2 / dig_T3.
 */
static void test_decode_calib_temperature(void) {
  TEST_BEGIN("test_decode_calib_temperature");
  uint8_t buf_88[26];
  uint8_t buf_e1[7] = {};
  ra_sensor_bme280_calib_t calib = {};
  build_calib_block(buf_88);
  ra_sensor_bme280_test_decode_calib(buf_88, buf_e1, &calib);
  TEST_ASSERT_EQ((int64_t)k_test_dig_T1, (int64_t)calib.dig_T1);
  TEST_ASSERT_EQ((int64_t)k_test_dig_T2, (int64_t)calib.dig_T2);
  TEST_ASSERT_EQ((int64_t)k_test_dig_T3, (int64_t)calib.dig_T3);
  TEST_END("test_decode_calib_temperature");
}

/**
 * @brief Bosch datasheet rev 1.6 sec 4.2.3 known-answer:
 *        T=25.08 degC for the canonical (dig_T1, dig_T2, dig_T3, adc_T)
 *        worked example.
 *
 * @details
 * The reference function returns ``DegC * 100`` (== 2508), the public
 * API rescales that to millidegrees (== 25080). The compensation has
 * a few-LSB rounding spread across compilers, so we accept +-50 mdegC.
 */
static void test_compensate_T_known_answer(void) {
  TEST_BEGIN("test_compensate_T_known_answer");
  ra_sensor_bme280_calib_t calib = {};
  calib.dig_T1 = (uint16_t)k_test_dig_T1;
  calib.dig_T2 = (int16_t)k_test_dig_T2;
  calib.dig_T3 = (int16_t)k_test_dig_T3;

  int32_t t_mc = 0;
  int32_t h_mp = 0;
  uint32_t p_pa = 0U;
  /* adc_p / adc_h irrelevant for this assertion -- pass zeros. */
  ra_sensor_bme280_test_compensate(&calib, (int32_t)k_test_adc_T, 0, 0, &t_mc,
                                   &h_mp, &p_pa);

  /* Tolerance: +-50 mdegC around 25080 mdegC (==25.08 degC). */
  const int32_t lo =
      (int32_t)k_test_expected_T_mc - (int32_t)k_test_T_mc_tolerance;
  const int32_t hi =
      (int32_t)k_test_expected_T_mc + (int32_t)k_test_T_mc_tolerance;
  TEST_ASSERT(t_mc >= lo);
  TEST_ASSERT(t_mc <= hi);
  TEST_END("test_compensate_T_known_answer");
}

/* =============================================================================
 * Test runner
 * =============================================================================
 */

/**
 * @brief Test suite entry point.
 */
int32_t main(void) {
  test_init_deinit_happy();
  test_double_init_rejected();
  test_init_null_cfg();
  test_init_invalid_args();
  test_pre_init_access();
  test_null_outputs_rejected();
  test_get_chip_id_post_init();
  test_decode_calib_temperature();
  test_compensate_T_known_answer();
  return 0;
}
