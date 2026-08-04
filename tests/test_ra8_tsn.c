/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_tsn.c
 * @brief Unit tests for ra8_tsn.c (on-chip temperature sensor driver)
 *
 * @details
 * The TSN driver only owns TSCR (HUM Ch 55.2.1 p 3498) plus the
 * two-point calibration math (HUM Ch 55.3.1 p 3499-3500). These
 * tests stub the calibration words and the raw ADC code by
 * writing directly to the fake MMIO backing store.
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_tsn.h"
#include "ra8_tsn_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_tsn_t
 * @brief Calibration reference temperatures, in degrees Celsius.
 */
typedef enum : int16_t {
  k_t_high_ref_valid = 80,  /**< A high reference inside the sensor's range.   */
  k_t_high_ref_over  = 200, /**< One past it, which the validator must reject. */
} t_tsn_t;

/**
 * @enum ra8_tsn_test_const_t
 * @brief Magic-free constants used across the test cases.
 */
typedef enum : uint16_t {
  k_ra8_tsn_test_stab_us       = 30U,     /**< Minimum tTSTBL.            */
  k_ra8_tsn_test_stab_below    = 5U,      /**< Below tTSTBL floor.        */
  k_ra8_tsn_test_cal_hi_125    = 2400U,   /**< Plausible 125 degC code.   */
  k_ra8_tsn_test_cal_lo_n40    = 1500U,   /**< Plausible -40 degC code.   */
  k_ra8_tsn_test_raw_at_125    = 2400U,   /**< Same as cal_hi -> 125 mC.  */
  k_ra8_tsn_test_raw_at_n40    = 1500U,   /**< Same as cal_lo -> -40 mC.  */
  k_ra8_tsn_test_raw_above_12b = 0xF000U, /**< Mask-test value > 12 bits. */
} ra8_tsn_test_const_t;

/**
 * @brief Inject calibration words into the fake MRAM trim area.
 *
 * @details
 * ``r_tsn_cal_regs_t`` is read-only on hardware but the underlying
 * mmap region in ``ra8_fake_mmap`` is plain RAM, so casting away
 * const for the test injection is safe and confined to this file.
 */
static void inject_cal_words(uint32_t hi_code, uint32_t lo_code)
{
  volatile r_tsn_cal_regs_t* cal   = (volatile r_tsn_cal_regs_t*)(uintptr_t)k_ra8_tsn_cal_base_addr;
  *(volatile uint32_t*)&cal->TSCDR = hi_code;
  *(volatile uint32_t*)&cal->TSCDR2 = lo_code;
}

static ra8_tsn_config_t make_cfg(void)
{
  const ra8_tsn_config_t cfg = {
    .high_ref_degc = k_ra8_tsn_cal_temp_high_125,
    .low_ref_degc  = k_ra8_tsn_cal_temp_low_n40,
    .stab_us       = (uint16_t)k_ra8_tsn_test_stab_us,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("tsn init happy");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  volatile r_tsn_ctrl_regs_t* reg = ra8_tsn();
  /* Both TSEN and TSOE must be set after init. */
  TEST_ASSERT((reg->TSCR & (uint8_t)k_ra8_tscr_mask_tsen) != 0U);
  TEST_ASSERT((reg->TSCR & (uint8_t)k_ra8_tscr_mask_tsoe) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  TEST_ASSERT_EQ(0, reg->TSCR);
  TEST_END("tsn init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("tsn init null cfg");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tsn_init(nullptr));
  TEST_END("tsn init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_stab(void)
{
  TEST_BEGIN("tsn init bad stab");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  ra8_tsn_config_t cfg = make_cfg();
  cfg.stab_us          = (uint16_t)k_ra8_tsn_test_stab_below;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tsn_init(&cfg));
  TEST_END("tsn init bad stab");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_high_ref(void)
{
  TEST_BEGIN("tsn init bad high ref");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  ra8_tsn_config_t cfg = make_cfg();
  /* Pretend 80 degC reference -- not a supported part-number trim. */
  cfg.high_ref_degc = (ra8_tsn_cal_temp_t)k_t_high_ref_valid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tsn_init(&cfg));
  TEST_END("tsn init bad high ref");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_raw_masks_to_12_bits(void)
{
  TEST_BEGIN("tsn read_raw masks to 12 bits");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  uint16_t code = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_read_raw((uint16_t)k_ra8_tsn_test_raw_above_12b, &code));
  /* 0xF000 -> masked to 0x000 (top 4 bits dropped). */
  TEST_ASSERT_EQ(0x000U, code);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_read_raw(0x0FFFU, &code));
  TEST_ASSERT_EQ(0x0FFFU, code);
  TEST_END("tsn read_raw masks to 12 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_raw_null_out(void)
{
  TEST_BEGIN("tsn read_raw null out");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tsn_read_raw(0x100U, nullptr));
  TEST_END("tsn read_raw null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_raw_before_init(void)
{
  TEST_BEGIN("tsn read_raw before init");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  /* deinit first to make sure the global-init flag is cleared
   * regardless of test ordering. */
  (void)ra8_tsn_deinit();

  uint16_t code = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_tsn_read_raw(0x100U, &code));
  TEST_END("tsn read_raw before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_convert_at_high_ref(void)
{
  TEST_BEGIN("tsn convert at high ref returns 125000 mC");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  inject_cal_words((uint32_t)k_ra8_tsn_test_cal_hi_125, (uint32_t)k_ra8_tsn_test_cal_lo_n40);
  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_convert_to_milli_c((uint16_t)k_ra8_tsn_test_raw_at_125, &milli));
  /* raw == cal_hi -> T = T1 = 125 degC = 125000 milli-C. */
  TEST_ASSERT_EQ(125000, milli);
  TEST_END("tsn convert at high ref returns 125000 mC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_convert_at_low_ref(void)
{
  TEST_BEGIN("tsn convert at low ref returns -40000 mC");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  inject_cal_words((uint32_t)k_ra8_tsn_test_cal_hi_125, (uint32_t)k_ra8_tsn_test_cal_lo_n40);
  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_convert_to_milli_c((uint16_t)k_ra8_tsn_test_raw_at_n40, &milli));
  /* raw == cal_lo -> T = T2 = -40 degC = -40000 milli-C. */
  TEST_ASSERT_EQ(-40000, milli);
  TEST_END("tsn convert at low ref returns -40000 mC");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_convert_unprogrammed_cal_rejected(void)
{
  TEST_BEGIN("tsn convert refuses unprogrammed calibration");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  inject_cal_words(0U, 0U); /* both 0 -> no slope */
  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  int32_t milli = 0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_tsn_convert_to_milli_c(0x100U, &milli));
  TEST_END("tsn convert refuses unprogrammed calibration");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_convert_null_out(void)
{
  TEST_BEGIN("tsn convert null out");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  inject_cal_words((uint32_t)k_ra8_tsn_test_cal_hi_125, (uint32_t)k_ra8_tsn_test_cal_lo_n40);
  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tsn_convert_to_milli_c(0x100U, nullptr));
  TEST_END("tsn convert null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_and_clear_status(void)
{
  TEST_BEGIN("tsn get + clear status");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_get_status(&mask));
  TEST_ASSERT_EQ(k_ra8_tscr_mask_all, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_clear_status());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_get_status(&mask));
  TEST_ASSERT_EQ(0, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tsn_get_status(nullptr));
  TEST_END("tsn get + clear status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("tsn power transition");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  const ra8_tsn_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_exit_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  TEST_END("tsn power transition");
}

/**
 * @test test_mcdc_ra8_tsn
 *
 * @par MC/DC:
 * Decision A: ``internal_validate_cfg`` line 120,
 * libs/ra8_hal/src/ra8_tsn.c:
 * ``if ((cfg->high_ref_degc == 125) || (cfg->high_ref_degc == 105))``
 * (2 conditions, ``||``). N+1 = 3:
 * - V1: 125 -> dec T (high_ok)
 * - V2: 105 -> dec T (high_ok)
 * - V3: 200 -> dec F (reject)
 *
 * Decision B: ``internal_validate_cfg`` line 128,
 * ``if (!high_ok || !low_ok)`` (2 conditions, ``||``). N+1 = 3:
 * - V1: high=T,low=T -> dec F (accept)
 * - V2: high=F,low=T -> dec T (reject)
 * - V3: high=T,low=F -> dec T (reject)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_tsn(void)
{
  TEST_BEGIN("tsn MC/DC: validate_cfg high/low ref decisions");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  ra8_tsn_config_t cfg = make_cfg();
  cfg.high_ref_degc    = k_ra8_tsn_cal_temp_high_125;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  cfg.high_ref_degc = k_ra8_tsn_cal_temp_high_105;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  cfg.high_ref_degc = (ra8_tsn_cal_temp_t)k_t_high_ref_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tsn_init(&cfg));
  cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  cfg.high_ref_degc = (ra8_tsn_cal_temp_t)k_t_high_ref_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tsn_init(&cfg));
  cfg              = make_cfg();
  cfg.low_ref_degc = (ra8_tsn_cal_temp_t)0;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tsn_init(&cfg));
  TEST_END("tsn MC/DC: validate_cfg high/low ref decisions");
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
  test_mcdc_ra8_tsn();
  (void)fprintf(stderr, "[OK  ] test_ra8_tsn.c\n");
  return 0;
}
