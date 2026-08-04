/**
 * @file test_ra8_pdg.c
 * @brief Unit tests for ra8_pdg.c (PWM Delay Generation Circuit driver)
 *
 * @details
 * This sibling owns the init / auto-tune / delay setter contract tests.
 * The runtime-control, capture, dispatch, and MC/DC vector tests live in
 * test_ra8_pdg_ctrl.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt_regs.h"
#include "ra8_mstp.h"
#include "ra8_pdg.h"
#include "ra8_pdg_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_pdg_test_const_t
 * @brief Test-only constants -- channel indices and probe codes.
 */
typedef enum : uint8_t {
  k_ra8_pdg_test_ch_first = 0U,    /**< Channel 0 (mapped to GPT320).       */
  k_ra8_pdg_test_ch_mid   = 2U,    /**< Channel 2 (mapped to GPT322).       */
  k_ra8_pdg_test_ch_last  = 3U,    /**< Channel 3 (mapped to GPT323).       */
  k_ra8_pdg_test_ch_bad   = 4U,    /**< First out-of-range channel index.   */
  k_ra8_pdg_test_ch_way   = 200U,  /**< Far out-of-range channel index.     */
  k_ra8_pdg_test_code_lo  = 0x05U, /**< Small probe code.                   */
  k_ra8_pdg_test_code_hi  = 0x42U, /**< Larger probe code, still valid.     */
  k_ra8_pdg_test_code_max = 0x7FU, /**< Maximum legal DLY[6:0] code.        */
  k_ra8_pdg_test_code_bad = 0x80U, /**< First illegal code (DLY[6:0] only). */
  k_ra8_pdg_test_mask_all = 0x0FU, /**< Channel mask covering 0..3.         */
  k_ra8_pdg_test_mask_one = 0x01U, /**< Channel mask covering ch 0 only.    */
  k_ra8_pdg_test_mask_bad = 0x10U, /**< Channel mask with bit 4 set.        */
} ra8_pdg_test_const_t;

/**
 * @enum ra8_pdg_test_freq_t
 * @brief Probe clock frequencies used by auto-tune / write-interval tests.
 */
typedef enum : uint32_t {
  k_ra8_pdg_test_clk_low_band   = 100000000UL, /**< 100 MHz, in low band.   */
  k_ra8_pdg_test_clk_high_band  = 250000000UL, /**< 250 MHz, in high band.  */
  k_ra8_pdg_test_clk_overlap    = 158000000UL, /**< Overlap zone -> low.    */
  k_ra8_pdg_test_clk_too_low    = 50000000UL,  /**< Below 80 MHz.           */
  k_ra8_pdg_test_clk_too_high   = 400000000UL, /**< Above 300 MHz.          */
  k_ra8_pdg_test_clk_pclka      = 200000000UL, /**< Sample PCLKA = 200 MHz. */
  k_ra8_pdg_test_delay_short_ns = 1U,          /**< Probe ns target.        */
  k_ra8_pdg_test_delay_long_ns  = 100U,        /**< Larger probe target.    */
} ra8_pdg_test_freq_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

static ra8_pdg_config_t make_cfg(void)
{
  const ra8_pdg_config_t cfg = {
    .frange       = k_ra8_pdg_frange_80_160_mhz,
    .channel_mask = (uint8_t)k_ra8_pdg_test_mask_all,
    .auto_tune    = 0U,
    .gptclk_hz    = 0U,
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
  TEST_BEGIN("pdg init happy");
  prep();

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  volatile r_pdg_regs_t* reg = ra8_pdg();
  /* DLLEN must be set, DLYRST must be cleared after step 4. */
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra8_pdg_gtdlycr_mask_dllen) != 0U);
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra8_pdg_gtdlycr_mask_dlyrst) == 0U);
  /* DLYBS bits in GTDLYCR2 should match the channel mask. */
  TEST_ASSERT_EQ(((uint16_t)k_ra8_pdg_test_mask_all << (uint16_t)k_ra8_pdg_gtdlycr2_shift_dlybs),
                 (reg->GTDLYCR2 & (uint16_t)k_ra8_pdg_gtdlycr2_mask_dlybs));
  TEST_END("pdg init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("pdg init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdg_init(nullptr));
  TEST_END("pdg init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_frange(void)
{
  TEST_BEGIN("pdg init bad frange");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.frange           = (ra8_pdg_frange_t)0x3U; /* 0b11 is "Setting prohibited" */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdg_init(&cfg));
  TEST_END("pdg init bad frange");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_mask(void)
{
  TEST_BEGIN("pdg init bad channel mask");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.channel_mask     = (uint8_t)k_ra8_pdg_test_mask_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdg_init(&cfg));
  TEST_END("pdg init bad channel mask");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_auto_tune_low_band(void)
{
  TEST_BEGIN("pdg init auto-tune low band");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.auto_tune        = 1U;
  cfg.gptclk_hz        = (uint32_t)k_ra8_pdg_test_clk_low_band;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  /* FRANGE field should be 00b after init. */
  volatile r_pdg_regs_t* reg = ra8_pdg();
  const uint16_t         f = (uint16_t)((reg->GTDLYCR & (uint16_t)k_ra8_pdg_gtdlycr_mask_frange) >>
                                        (uint16_t)k_ra8_pdg_gtdlycr_shift_frange);
  TEST_ASSERT_EQ(k_ra8_pdg_frange_80_160_mhz, f);
  TEST_END("pdg init auto-tune low band");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_auto_tune_high_band(void)
{
  TEST_BEGIN("pdg init auto-tune high band");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.auto_tune        = 1U;
  cfg.gptclk_hz        = (uint32_t)k_ra8_pdg_test_clk_high_band;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  volatile r_pdg_regs_t* reg = ra8_pdg();
  const uint16_t         f = (uint16_t)((reg->GTDLYCR & (uint16_t)k_ra8_pdg_gtdlycr_mask_frange) >>
                                        (uint16_t)k_ra8_pdg_gtdlycr_shift_frange);
  TEST_ASSERT_EQ(k_ra8_pdg_frange_155_300_mhz, f);
  TEST_END("pdg init auto-tune high band");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_auto_tune_zero_hz(void)
{
  TEST_BEGIN("pdg init auto-tune zero hz");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.auto_tune        = 1U;
  cfg.gptclk_hz        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdg_init(&cfg));
  TEST_END("pdg init auto-tune zero hz");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_auto_tune_out_of_range(void)
{
  TEST_BEGIN("pdg init auto-tune out of range");
  prep();

  ra8_pdg_config_t cfg = make_cfg();
  cfg.auto_tune        = 1U;
  cfg.gptclk_hz        = (uint32_t)k_ra8_pdg_test_clk_too_low;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_pdg_init(&cfg));

  cfg.gptclk_hz = (uint32_t)k_ra8_pdg_test_clk_too_high;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_pdg_init(&cfg));
  TEST_END("pdg init auto-tune out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_rising_pin_a(void)
{
  TEST_BEGIN("pdg set_delay rising pin A");
  prep();

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));

  volatile r_pdg_regs_t* reg = ra8_pdg();
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_lo,
                 (reg->GTDLYR[(uint8_t)k_ra8_pdg_test_ch_first].A & (uint16_t)k_ra8_pdg_dly_mask));
  TEST_END("pdg set_delay rising pin A");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_falling_pin_b(void)
{
  TEST_BEGIN("pdg set_delay falling pin B");
  prep();

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_mid,
                                   k_ra8_pdg_pin_b,
                                   k_ra8_pdg_edge_falling,
                                   (uint8_t)k_ra8_pdg_test_code_hi));

  volatile r_pdg_regs_t* reg = ra8_pdg();
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_hi,
                 (reg->GTDLYF[(uint8_t)k_ra8_pdg_test_ch_mid].B & (uint16_t)k_ra8_pdg_dly_mask));
  TEST_END("pdg set_delay falling pin B");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_bad_inputs(void)
{
  TEST_BEGIN("pdg set_delay bad inputs");
  prep();
  /* Even when cfg has not run, the not_initialized check fires first. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_bad,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_way,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   (ra8_pdg_pin_t)0xFFU,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   (ra8_pdg_edge_t)0xFFU,
                                   (uint8_t)k_ra8_pdg_test_code_lo));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_bad));
  TEST_END("pdg set_delay bad inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_delay_round_trip(void)
{
  TEST_BEGIN("pdg get_delay round-trip");
  prep();

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_last,
                                   k_ra8_pdg_pin_b,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_hi));
  uint8_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_get_delay((uint8_t)k_ra8_pdg_test_ch_last,
                                   k_ra8_pdg_pin_b,
                                   k_ra8_pdg_edge_rising,
                                   &got));
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_hi, got);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_pdg_get_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_get_delay((uint8_t)k_ra8_pdg_test_ch_bad,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   &got));
  TEST_END("pdg get_delay round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_batch_happy(void)
{
  TEST_BEGIN("pdg set_delay batch happy");
  prep();

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  const ra8_pdg_delay_entry_t batch[] = {
    {.channel = (uint8_t)k_ra8_pdg_test_ch_first,
     .pin     = k_ra8_pdg_pin_a,
     .edge    = k_ra8_pdg_edge_rising,
     .code    = (uint8_t)k_ra8_pdg_test_code_lo},
    {.channel = (uint8_t)k_ra8_pdg_test_ch_first,
     .pin     = k_ra8_pdg_pin_b,
     .edge    = k_ra8_pdg_edge_falling,
     .code    = (uint8_t)k_ra8_pdg_test_code_hi},
    {.channel = (uint8_t)k_ra8_pdg_test_ch_last,
     .pin     = k_ra8_pdg_pin_a,
     .edge    = k_ra8_pdg_edge_falling,
     .code    = (uint8_t)k_ra8_pdg_test_code_max},
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_set_delay_batch(batch, (uint8_t)(sizeof(batch) / sizeof(batch[0]))));

  volatile r_pdg_regs_t* reg = ra8_pdg();
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_lo,
                 (reg->GTDLYR[(uint8_t)k_ra8_pdg_test_ch_first].A & (uint16_t)k_ra8_pdg_dly_mask));
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_hi,
                 (reg->GTDLYF[(uint8_t)k_ra8_pdg_test_ch_first].B & (uint16_t)k_ra8_pdg_dly_mask));
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_max,
                 (reg->GTDLYF[(uint8_t)k_ra8_pdg_test_ch_last].A & (uint16_t)k_ra8_pdg_dly_mask));
  TEST_END("pdg set_delay batch happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_batch_bad_inputs(void)
{
  TEST_BEGIN("pdg set_delay batch bad inputs");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdg_set_delay_batch(nullptr, 1U));

  const ra8_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdg_init(&cfg));

  const ra8_pdg_delay_entry_t one[] = {
    {.channel = (uint8_t)k_ra8_pdg_test_ch_bad,
     .pin     = k_ra8_pdg_pin_a,
     .edge    = k_ra8_pdg_edge_rising,
     .code    = (uint8_t)k_ra8_pdg_test_code_lo},
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdg_set_delay_batch(one, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdg_set_delay_batch(one, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_set_delay_batch(one, (uint8_t)(k_ra8_pdg_slot_count + 1U)));
  TEST_END("pdg set_delay batch bad inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_not_initialized(void)
{
  TEST_BEGIN("pdg set_delay before init");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_pdg_set_delay((uint8_t)k_ra8_pdg_test_ch_first,
                                   k_ra8_pdg_pin_a,
                                   k_ra8_pdg_edge_rising,
                                   (uint8_t)k_ra8_pdg_test_code_lo));
  const ra8_pdg_delay_entry_t one[] = {
    {.channel = (uint8_t)k_ra8_pdg_test_ch_first,
     .pin     = k_ra8_pdg_pin_a,
     .edge    = k_ra8_pdg_edge_rising,
     .code    = (uint8_t)k_ra8_pdg_test_code_lo},
  };
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_pdg_set_delay_batch(one, 1U));
  TEST_END("pdg set_delay before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_delay_ns_to_code_low_band(void)
{
  TEST_BEGIN("pdg delay_ns_to_code low band");
  prep();

  /* low band, GTCLK = 100 MHz -> period = 10 ns; one DLY step = 10/128 ns. */
  uint8_t code = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_delay_ns_to_code((uint32_t)k_ra8_pdg_test_delay_long_ns,
                                          (uint32_t)k_ra8_pdg_test_clk_low_band,
                                          k_ra8_pdg_frange_80_160_mhz,
                                          &code));
  /* 100 ns -> 100 / (10/128) = 1280 -> clamped to 0x7F. */
  TEST_ASSERT_EQ(k_ra8_pdg_test_code_max, code);

  /* small delay: 1 ns at 100 MHz low band -> 1 / (10/128) = 12.8 -> 13 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pdg_delay_ns_to_code((uint32_t)k_ra8_pdg_test_delay_short_ns,
                                          (uint32_t)k_ra8_pdg_test_clk_low_band,
                                          k_ra8_pdg_frange_80_160_mhz,
                                          &code));
  TEST_ASSERT_EQ(13, code);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_pdg_delay_ns_to_code(1U, 1U, k_ra8_pdg_frange_80_160_mhz, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_delay_ns_to_code(1U, 0U, k_ra8_pdg_frange_80_160_mhz, &code));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_pdg_delay_ns_to_code(1U, 1U, (ra8_pdg_frange_t)0xFFU, &code));
  TEST_END("pdg delay_ns_to_code low band");
}
int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_bad_frange();
  test_init_bad_mask();
  test_init_auto_tune_low_band();
  test_init_auto_tune_high_band();
  test_init_auto_tune_zero_hz();
  test_init_auto_tune_out_of_range();
  test_set_delay_rising_pin_a();
  test_set_delay_falling_pin_b();
  test_set_delay_bad_inputs();
  test_get_delay_round_trip();
  test_set_delay_batch_happy();
  test_set_delay_batch_bad_inputs();
  test_set_delay_not_initialized();
  test_delay_ns_to_code_low_band();
  (void)fprintf(stderr, "[OK ] test_ra8_pdg.c\n");
  return 0;
}
