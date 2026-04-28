/**
 * @file test_ra_tsn.c
 * @brief Unit tests for ra_tsn.c (on-chip temperature sensor driver)
 *
 * @details
 * The TSN driver only owns TSCR (HUM Ch 55.2.1 p 3498) plus the
 * two-point calibration math (HUM Ch 55.3.1 p 3499-3500). These
 * tests stub the calibration words and the raw ADC code by
 * writing directly to the simulated MMIO backing store.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_tsn_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_tsn.h"
#include "unity_minimal.h"

/**
 * @enum ra_tsn_test_const_t
 * @brief Magic-free constants used across the test cases.
 */
typedef enum : uint16_t {
  k_ra_tsn_test_stab_us       = 30U,     /**< Minimum tTSTBL.            */
  k_ra_tsn_test_stab_below    = 5U,      /**< Below tTSTBL floor.        */
  k_ra_tsn_test_cal_hi_125    = 2400U,   /**< Plausible 125 degC code.   */
  k_ra_tsn_test_cal_lo_n40    = 1500U,   /**< Plausible -40 degC code.   */
  k_ra_tsn_test_raw_at_125    = 2400U,   /**< Same as cal_hi -> 125 mC.  */
  k_ra_tsn_test_raw_at_n40    = 1500U,   /**< Same as cal_lo -> -40 mC.  */
  k_ra_tsn_test_raw_above_12b = 0xF000U, /**< Mask-test value > 12 bits. */
} ra_tsn_test_const_t;

/**
 * @brief Inject calibration words into the simulated MRAM trim area.
 *
 * @details
 * ``r_tsn_cal_regs_t`` is read-only on hardware but the underlying
 * mmap region in ``ra_sim_mmap`` is plain RAM, so casting away
 * const for the test injection is safe and confined to this file.
 */
static void inject_cal_words(uint32_t hi_code, uint32_t lo_code)
{
  volatile r_tsn_cal_regs_t* cal    = (volatile r_tsn_cal_regs_t*)(uintptr_t)k_ra_tsn_cal_base_addr;
  *(volatile uint32_t*)&cal->TSCDR  = hi_code;
  *(volatile uint32_t*)&cal->TSCDR2 = lo_code;
}

static ra_tsn_config_t make_cfg(void)
{
  const ra_tsn_config_t cfg = {
    .high_ref_degc = k_ra_tsn_cal_temp_high_125,
    .low_ref_degc  = k_ra_tsn_cal_temp_low_n40,
    .stab_us       = (uint16_t)k_ra_tsn_test_stab_us,
  };
  return cfg;
}

static void test_init_happy(void)
{
  TEST_BEGIN("tsn init happy");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  volatile r_tsn_ctrl_regs_t* reg = ra_tsn();
  /* Both TSEN and TSOE must be set after init. */
  TEST_ASSERT((reg->TSCR & (uint8_t)k_ra_tscr_mask_tsen) != 0U);
  TEST_ASSERT((reg->TSCR & (uint8_t)k_ra_tscr_mask_tsoe) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->TSCR);
  TEST_END("tsn init happy");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("tsn init null cfg");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tsn_init(nullptr));
  TEST_END("tsn init null cfg");
}

static void test_init_bad_stab(void)
{
  TEST_BEGIN("tsn init bad stab");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  ra_tsn_config_t cfg = make_cfg();
  cfg.stab_us         = (uint16_t)k_ra_tsn_test_stab_below;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tsn_init(&cfg));
  TEST_END("tsn init bad stab");
}

static void test_init_bad_high_ref(void)
{
  TEST_BEGIN("tsn init bad high ref");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  ra_tsn_config_t cfg = make_cfg();
  /* Pretend 80 degC reference -- not a supported part-number trim. */
  cfg.high_ref_degc = (ra_tsn_cal_temp_t)80;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tsn_init(&cfg));
  TEST_END("tsn init bad high ref");
}

static void test_read_raw_masks_to_12_bits(void)
{
  TEST_BEGIN("tsn read_raw masks to 12 bits");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  uint16_t code = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_tsn_read_raw((uint16_t)k_ra_tsn_test_raw_above_12b, &code));
  /* 0xF000 -> masked to 0x000 (top 4 bits dropped). */
  TEST_ASSERT_EQ((int32_t)0x000U, (int32_t)code);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_read_raw(0x0FFFU, &code));
  TEST_ASSERT_EQ((int32_t)0x0FFFU, (int32_t)code);
  TEST_END("tsn read_raw masks to 12 bits");
}

static void test_read_raw_null_out(void)
{
  TEST_BEGIN("tsn read_raw null out");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tsn_read_raw(0x100U, nullptr));
  TEST_END("tsn read_raw null out");
}

static void test_read_raw_before_init(void)
{
  TEST_BEGIN("tsn read_raw before init");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  /* deinit first to make sure the global-init flag is cleared
   * regardless of test ordering. */
  (void)ra_tsn_deinit();

  uint16_t code = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_tsn_read_raw(0x100U, &code));
  TEST_END("tsn read_raw before init");
}

static void test_convert_at_high_ref(void)
{
  TEST_BEGIN("tsn convert at high ref returns 125000 mC");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  inject_cal_words((uint32_t)k_ra_tsn_test_cal_hi_125, (uint32_t)k_ra_tsn_test_cal_lo_n40);
  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_tsn_convert_to_milli_c((uint16_t)k_ra_tsn_test_raw_at_125, &milli));
  /* raw == cal_hi -> T = T1 = 125 degC = 125000 milli-C. */
  TEST_ASSERT_EQ((int32_t)125000, milli);
  TEST_END("tsn convert at high ref returns 125000 mC");
}

static void test_convert_at_low_ref(void)
{
  TEST_BEGIN("tsn convert at low ref returns -40000 mC");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  inject_cal_words((uint32_t)k_ra_tsn_test_cal_hi_125, (uint32_t)k_ra_tsn_test_cal_lo_n40);
  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_tsn_convert_to_milli_c((uint16_t)k_ra_tsn_test_raw_at_n40, &milli));
  /* raw == cal_lo -> T = T2 = -40 degC = -40000 milli-C. */
  TEST_ASSERT_EQ((int32_t)-40000, milli);
  TEST_END("tsn convert at low ref returns -40000 mC");
}

static void test_convert_unprogrammed_cal_rejected(void)
{
  TEST_BEGIN("tsn convert refuses unprogrammed calibration");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  inject_cal_words(0U, 0U); /* both 0 -> no slope */
  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_tsn_convert_to_milli_c(0x100U, &milli));
  TEST_END("tsn convert refuses unprogrammed calibration");
}

static void test_convert_null_out(void)
{
  TEST_BEGIN("tsn convert null out");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  inject_cal_words((uint32_t)k_ra_tsn_test_cal_hi_125, (uint32_t)k_ra_tsn_test_cal_lo_n40);
  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tsn_convert_to_milli_c(0x100U, nullptr));
  TEST_END("tsn convert null out");
}

static void test_get_and_clear_status(void)
{
  TEST_BEGIN("tsn get + clear status");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)k_ra_tscr_mask_all, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_clear_status());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tsn_get_status(nullptr));
  TEST_END("tsn get + clear status");
}

static void test_power_transition(void)
{
  TEST_BEGIN("tsn power transition");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  const ra_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_exit_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tsn_deinit());
  TEST_END("tsn power transition");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_bad_stab();
  test_init_bad_high_ref();
  test_read_raw_masks_to_12_bits();
  test_read_raw_null_out();
  test_read_raw_before_init();
  test_convert_at_high_ref();
  test_convert_at_low_ref();
  test_convert_unprogrammed_cal_rejected();
  test_convert_null_out();
  test_get_and_clear_status();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_tsn.c\n");
  return 0;
}
