/**
 * @file test_ra_pdg.c
 * @brief Unit tests for ra_pdg.c (PWM Delay Generation Circuit driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_gpt_regs.h"
#include "ra8d2_pdg_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_pdg.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_pdg_test_const_t
 * @brief Test-only constants -- channel indices and probe codes.
 */
typedef enum : uint8_t {
  k_ra_pdg_test_ch_first = 0U,    /**< Channel 0 (mapped to GPT320). */
  k_ra_pdg_test_ch_mid   = 2U,    /**< Channel 2 (mapped to GPT322). */
  k_ra_pdg_test_ch_last  = 3U,    /**< Channel 3 (mapped to GPT323). */
  k_ra_pdg_test_ch_bad   = 4U,    /**< First out-of-range channel index. */
  k_ra_pdg_test_ch_way   = 200U,  /**< Far out-of-range channel index. */
  k_ra_pdg_test_code_lo  = 0x05U, /**< Small probe code. */
  k_ra_pdg_test_code_hi  = 0x42U, /**< Larger probe code, still valid. */
  k_ra_pdg_test_code_max = 0x7FU, /**< Maximum legal DLY[6:0] code. */
  k_ra_pdg_test_code_bad = 0x80U, /**< First illegal code (DLY[6:0] only).*/
  k_ra_pdg_test_mask_all = 0x0FU, /**< Channel mask covering 0..3. */
  k_ra_pdg_test_mask_one = 0x01U, /**< Channel mask covering ch 0 only. */
  k_ra_pdg_test_mask_bad = 0x10U, /**< Channel mask with bit 4 set. */
} ra_pdg_test_const_t;

/**
 * @enum ra_pdg_test_freq_t
 * @brief Probe clock frequencies used by auto-tune / write-interval tests.
 */
typedef enum : uint32_t {
  k_ra_pdg_test_clk_low_band   = 100000000UL, /**< 100 MHz, in low band. */
  k_ra_pdg_test_clk_high_band  = 250000000UL, /**< 250 MHz, in high band. */
  k_ra_pdg_test_clk_overlap    = 158000000UL, /**< Overlap zone -> low. */
  k_ra_pdg_test_clk_too_low    = 50000000UL,  /**< Below 80 MHz. */
  k_ra_pdg_test_clk_too_high   = 400000000UL, /**< Above 300 MHz. */
  k_ra_pdg_test_clk_pclka      = 200000000UL, /**< Sample PCLKA = 200 MHz. */
  k_ra_pdg_test_delay_short_ns = 1U,          /**< Probe ns target. */
  k_ra_pdg_test_delay_long_ns  = 100U,        /**< Larger probe target. */
} ra_pdg_test_freq_t;

static uint32_t s_pdg_cb_count;
static void*    s_pdg_cb_last_ctx;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_pdg_cb_count    = 0U;
  s_pdg_cb_last_ctx = nullptr;
}

static ra_pdg_config_t make_cfg(void)
{
  const ra_pdg_config_t cfg = {
    .frange       = k_ra_pdg_frange_80_160_mhz,
    .channel_mask = (uint8_t)k_ra_pdg_test_mask_all,
    .auto_tune    = 0U,
    .gptclk_hz    = 0U,
  };
  return cfg;
}

static void stub_pdg_cb(void* ctx)
{
  ++s_pdg_cb_count;
  s_pdg_cb_last_ctx = ctx;
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

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  volatile r_pdg_regs_t* reg = ra_pdg();
  /* DLLEN must be set, DLYRST must be cleared after step 4. */
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dllen) != 0U);
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dlyrst) == 0U);
  /* DLYBS bits in GTDLYCR2 should match the channel mask. */
  TEST_ASSERT_EQ(
    (int32_t)((uint16_t)k_ra_pdg_test_mask_all << (uint16_t)k_ra_pdg_gtdlycr2_shift_dlybs),
    (int32_t)(reg->GTDLYCR2 & (uint16_t)k_ra_pdg_gtdlycr2_mask_dlybs));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_init(nullptr));
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.frange          = (ra_pdg_frange_t)0x3U; /* 0b11 is "Setting prohibited" */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_init(&cfg));
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.channel_mask    = (uint8_t)k_ra_pdg_test_mask_bad;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_init(&cfg));
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.auto_tune       = 1U;
  cfg.gptclk_hz       = (uint32_t)k_ra_pdg_test_clk_low_band;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* FRANGE field should be 00b after init. */
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         f   = (uint16_t)((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_frange) >>
                                          (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_80_160_mhz, (int32_t)f);
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.auto_tune       = 1U;
  cfg.gptclk_hz       = (uint32_t)k_ra_pdg_test_clk_high_band;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         f   = (uint16_t)((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_frange) >>
                                          (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_155_300_mhz, (int32_t)f);
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.auto_tune       = 1U;
  cfg.gptclk_hz       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_init(&cfg));
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

  ra_pdg_config_t cfg = make_cfg();
  cfg.auto_tune       = 1U;
  cfg.gptclk_hz       = (uint32_t)k_ra_pdg_test_clk_too_low;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range, (int32_t)ra_pdg_init(&cfg));

  cfg.gptclk_hz = (uint32_t)k_ra_pdg_test_clk_too_high;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range, (int32_t)ra_pdg_init(&cfg));
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

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));

  volatile r_pdg_regs_t* reg = ra_pdg();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_pdg_test_code_lo,
    (int32_t)(reg->GTDLYR[(uint8_t)k_ra_pdg_test_ch_first].A & (uint16_t)k_ra_pdg_dly_mask));
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

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_mid,
                                           k_ra_pdg_pin_b,
                                           k_ra_pdg_edge_falling,
                                           (uint8_t)k_ra_pdg_test_code_hi));

  volatile r_pdg_regs_t* reg = ra_pdg();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_pdg_test_code_hi,
    (int32_t)(reg->GTDLYF[(uint8_t)k_ra_pdg_test_ch_mid].B & (uint16_t)k_ra_pdg_dly_mask));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_bad,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_way,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           (ra_pdg_pin_t)0xFFU,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           (ra_pdg_edge_t)0xFFU,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_bad));
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

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_last,
                                           k_ra_pdg_pin_b,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_hi));
  uint8_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_get_delay((uint8_t)k_ra_pdg_test_ch_last,
                                           k_ra_pdg_pin_b,
                                           k_ra_pdg_edge_rising,
                                           &got));
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_test_code_hi, (int32_t)got);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_pdg_get_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_get_delay((uint8_t)k_ra_pdg_test_ch_bad,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
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

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  const ra_pdg_delay_entry_t batch[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_first,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_rising,
     .code    = (uint8_t)k_ra_pdg_test_code_lo},
    {.channel = (uint8_t)k_ra_pdg_test_ch_first,
     .pin     = k_ra_pdg_pin_b,
     .edge    = k_ra_pdg_edge_falling,
     .code    = (uint8_t)k_ra_pdg_test_code_hi},
    {.channel = (uint8_t)k_ra_pdg_test_ch_last,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_falling,
     .code    = (uint8_t)k_ra_pdg_test_code_max},
  };
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_pdg_set_delay_batch(batch, (uint8_t)(sizeof(batch) / sizeof(batch[0]))));

  volatile r_pdg_regs_t* reg = ra_pdg();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_pdg_test_code_lo,
    (int32_t)(reg->GTDLYR[(uint8_t)k_ra_pdg_test_ch_first].A & (uint16_t)k_ra_pdg_dly_mask));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_pdg_test_code_hi,
    (int32_t)(reg->GTDLYF[(uint8_t)k_ra_pdg_test_ch_first].B & (uint16_t)k_ra_pdg_dly_mask));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_pdg_test_code_max,
    (int32_t)(reg->GTDLYF[(uint8_t)k_ra_pdg_test_ch_last].A & (uint16_t)k_ra_pdg_dly_mask));
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
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_set_delay_batch(nullptr, 1U));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  const ra_pdg_delay_entry_t one[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_bad,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_rising,
     .code    = (uint8_t)k_ra_pdg_test_code_lo},
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_set_delay_batch(one, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_set_delay_batch(one, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay_batch(one, (uint8_t)(k_ra_pdg_slot_count + 1U)));
  TEST_END("pdg set_delay batch bad inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_delay_not_initialised(void)
{
  TEST_BEGIN("pdg set_delay before init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  const ra_pdg_delay_entry_t one[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_first,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_rising,
     .code    = (uint8_t)k_ra_pdg_test_code_lo},
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_pdg_set_delay_batch(one, 1U));
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
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_delay_ns_to_code((uint32_t)k_ra_pdg_test_delay_long_ns,
                                                  (uint32_t)k_ra_pdg_test_clk_low_band,
                                                  k_ra_pdg_frange_80_160_mhz,
                                                  &code));
  /* 100 ns -> 100 / (10/128) = 1280 -> clamped to 0x7F. */
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_test_code_max, (int32_t)code);

  /* small delay: 1 ns at 100 MHz low band -> 1 / (10/128) = 12.8 -> 13 */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_delay_ns_to_code((uint32_t)k_ra_pdg_test_delay_short_ns,
                                                  (uint32_t)k_ra_pdg_test_clk_low_band,
                                                  k_ra_pdg_frange_80_160_mhz,
                                                  &code));
  TEST_ASSERT_EQ((int32_t)13, (int32_t)code);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_pdg_delay_ns_to_code(1U, 1U, k_ra_pdg_frange_80_160_mhz, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_delay_ns_to_code(1U, 0U, k_ra_pdg_frange_80_160_mhz, &code));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_delay_ns_to_code(1U, 1U, (ra_pdg_frange_t)0xFFU, &code));
  TEST_END("pdg delay_ns_to_code low band");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("pdg power transition");
  prep();

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_enter_stop((uint8_t)k_ra_pdg_test_ch_first));
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         bit =
    (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlyen + (uint16_t)k_ra_pdg_test_ch_first));
  TEST_ASSERT((reg->GTDLYCR2 & bit) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_exit_stop((uint8_t)k_ra_pdg_test_ch_first));
  TEST_ASSERT((reg->GTDLYCR2 & bit) == 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_enter_stop((uint8_t)k_ra_pdg_test_ch_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_exit_stop((uint8_t)k_ra_pdg_test_ch_bad));
  TEST_END("pdg power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_bypass_set(void)
{
  TEST_BEGIN("pdg channel_bypass_set");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_channel_bypass_set((uint8_t)k_ra_pdg_test_ch_first, 1U));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* Force bypass-on (clear DLYBSn) for ch first. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_channel_bypass_set((uint8_t)k_ra_pdg_test_ch_first, 0U));
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         bs_bit =
    (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlybs + (uint16_t)k_ra_pdg_test_ch_first));
  TEST_ASSERT((reg->GTDLYCR2 & bs_bit) == 0U);

  /* Restore bypass-off (delay applied). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_channel_bypass_set((uint8_t)k_ra_pdg_test_ch_first, 1U));
  TEST_ASSERT((reg->GTDLYCR2 & bs_bit) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_channel_bypass_set((uint8_t)k_ra_pdg_test_ch_bad, 1U));
  TEST_END("pdg channel_bypass_set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pin_disable(void)
{
  TEST_BEGIN("pdg pin_disable");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, k_ra_pdg_pin_a));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* Pre-load both rising + falling delays for ch_first pin A. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_lo));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_falling,
                                           (uint8_t)k_ra_pdg_test_code_hi));
  /* Disable pin A -- both cells must drop to zero. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, k_ra_pdg_pin_a));
  uint8_t got = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_get_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           &got));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)got);
  got = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_get_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_falling,
                                           &got));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)got);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_bad, k_ra_pdg_pin_a));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, (ra_pdg_pin_t)0xFFU));
  TEST_END("pdg pin_disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("pdg status read + clear");
  prep();

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  uint16_t st = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_get_status(&st));
  TEST_ASSERT((st & (uint16_t)k_ra_pdg_status_dll_locked) != 0U);

  /* Force DLYRST = 1, then ask the driver to clear it. */
  volatile r_pdg_regs_t* reg = ra_pdg();
  reg->GTDLYCR               = (uint16_t)(reg->GTDLYCR | (uint16_t)k_ra_pdg_gtdlycr_mask_dlyrst);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_clear_status((uint16_t)k_ra_pdg_status_in_reset));
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dlyrst) == 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_get_status(nullptr));
  TEST_END("pdg status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_full(void)
{
  TEST_BEGIN("pdg status_full snapshot");
  prep();

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* Drop ch 1 into stop. */
  const uint8_t ch_one = 1U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_enter_stop(ch_one));

  ra_pdg_status_full_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_get_status_full(&st));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)st.dll_enabled);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)st.in_reset);
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_80_160_mhz, (int32_t)st.frange);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)st.per_channel_bypass_off[(uint8_t)k_ra_pdg_test_ch_first]);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)st.per_channel_powered[(uint8_t)k_ra_pdg_test_ch_first]);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)st.per_channel_powered[ch_one]);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_get_status_full(nullptr));
  TEST_END("pdg status_full snapshot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pick_frange(void)
{
  TEST_BEGIN("pdg pick_frange");
  prep();

  ra_pdg_frange_t f = (ra_pdg_frange_t)0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pick_frange((uint32_t)k_ra_pdg_test_clk_low_band, &f));
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_80_160_mhz, (int32_t)f);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pick_frange((uint32_t)k_ra_pdg_test_clk_high_band, &f));
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_155_300_mhz, (int32_t)f);

  /* Overlap zone: prefer the low band for finer resolution. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pick_frange((uint32_t)k_ra_pdg_test_clk_overlap, &f));
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_80_160_mhz, (int32_t)f);

  /* Bad inputs. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_pick_frange(1U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_pick_frange(0U, &f));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_pdg_pick_frange((uint32_t)k_ra_pdg_test_clk_too_low, &f));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_pdg_pick_frange((uint32_t)k_ra_pdg_test_clk_too_high, &f));
  TEST_END("pdg pick_frange");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_frange_runtime(void)
{
  TEST_BEGIN("pdg set_frange runtime switch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_set_frange(k_ra_pdg_frange_155_300_mhz));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* Capture GTDLYCR2 then run the switch. */
  volatile r_pdg_regs_t* reg     = ra_pdg();
  const uint16_t         expect2 = reg->GTDLYCR2;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_set_frange(k_ra_pdg_frange_155_300_mhz));

  /* New FRANGE value must be live, DLLEN re-asserted, DLYRST cleared. */
  const uint16_t f = (uint16_t)((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_frange) >>
                                (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_frange_155_300_mhz, (int32_t)f);
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dllen) != 0U);
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dlyrst) == 0U);
  TEST_ASSERT_EQ((int32_t)expect2, (int32_t)reg->GTDLYCR2);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_set_frange((ra_pdg_frange_t)0xFFU));
  TEST_END("pdg set_frange runtime switch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bind_unbind_gpt_channel(void)
{
  TEST_BEGIN("pdg bind / unbind gpt channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_pdg_bind_gpt_channel((uint8_t)k_ra_pdg_test_ch_first));

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  volatile r_gpt_channel_regs_t* gpt = ra_gpt((uint8_t)k_ra_pdg_test_ch_first);
  TEST_ASSERT_NOT_NULL(gpt);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_bind_gpt_channel((uint8_t)k_ra_pdg_test_ch_first));
  TEST_ASSERT_EQ((int32_t)0xA500U, (int32_t)gpt->GTWP);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_unbind_gpt_channel((uint8_t)k_ra_pdg_test_ch_first));
  TEST_ASSERT_EQ((int32_t)0xA501U, (int32_t)gpt->GTWP);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_bind_gpt_channel((uint8_t)k_ra_pdg_test_ch_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_unbind_gpt_channel((uint8_t)k_ra_pdg_test_ch_bad));
  TEST_END("pdg bind / unbind gpt channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_check_constraints(void)
{
  TEST_BEGIN("pdg check_constraints (HUM Ch 23.4.2)");
  prep();

  /* Saw + Up: forbidden when compare_match >= GTPR - 2. */
  const uint32_t gtpr = 100U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw,
                                                   k_ra_pdg_dir_up,
                                                   /* compare_match */ 99U,
                                                   gtpr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw,
                                                   k_ra_pdg_dir_up,
                                                   /* compare_match */ 50U,
                                                   gtpr));

  /* Saw + Down: forbidden when compare_match <= 2. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw,
                                                   k_ra_pdg_dir_down,
                                                   /* compare_match */ 1U,
                                                   gtpr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw,
                                                   k_ra_pdg_dir_down,
                                                   /* compare_match */ 50U,
                                                   gtpr));

  /* Triangle + Down: forbidden when compare_match <= 2. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_triangle,
                                                   k_ra_pdg_dir_down,
                                                   /* compare_match */ 2U,
                                                   gtpr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_triangle,
                                                   k_ra_pdg_dir_up,
                                                   /* compare_match */ 1U,
                                                   gtpr));

  /* Bad arguments. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_pdg_check_constraints((ra_pdg_wave_mode_t)0xFFU, k_ra_pdg_dir_up, 1U, gtpr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw, (ra_pdg_count_dir_t)0xFFU, 1U, gtpr));
  /* gtpr too small for saw-up (boundary underflow). */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw, k_ra_pdg_dir_up, 0U, 1U));
  TEST_END("pdg check_constraints (HUM Ch 23.4.2)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_required_write_ns(void)
{
  TEST_BEGIN("pdg required_write_ns (HUM Ch 23.4.3)");
  prep();

  uint32_t ns = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_required_write_ns((uint32_t)k_ra_pdg_test_clk_pclka,
                                                   (uint32_t)k_ra_pdg_test_clk_low_band,
                                                   &ns));
  /* PCLKA = 200 MHz -> period 5 ns; GPTCLK = 100 MHz -> period 10 ns.
 * Interval = 5 * 6 + 10 * 4 = 30 + 40 = 70 ns. */
  TEST_ASSERT_EQ((int32_t)70, (int32_t)ns);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_required_write_ns(1U, 1U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_required_write_ns(0U, 1U, &ns));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_required_write_ns(1U, 0U, &ns));
  TEST_END("pdg required_write_ns (HUM Ch 23.4.3)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("pdg attach + dispatch");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_attach_handler(stub_pdg_cb, (void*)(uintptr_t)0xCAFEU));
  ra_pdg_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_pdg_cb_count);
  TEST_ASSERT_EQ((int32_t)(uintptr_t)0xCAFEU, (int32_t)(uintptr_t)s_pdg_cb_last_ctx);

  /* Detach (NULL handler) -- subsequent dispatch must be a no-op. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_attach_handler(nullptr, nullptr));
  ra_pdg_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_pdg_cb_count);
  TEST_END("pdg attach + dispatch");
}

/* ----------------------------------------------------------------------------
 * Sweep 17: ra_pdg_capture_start/stop (buffered delay-code apply)
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------*/

static void test_capture_start_happy(void)
{
  TEST_BEGIN("pdg capture_start happy");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  s_pdg_cb_count = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_attach_handler(stub_pdg_cb, (void*)(uintptr_t)0xBEEFU));

  const ra_pdg_delay_entry_t entries[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_first,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_rising,
     .code    = (uint8_t)k_ra_pdg_test_code_hi},
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_capture_start(entries, 1U));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_pdg_cb_count);
  volatile r_pdg_regs_t* reg = ra_pdg();
  TEST_ASSERT_EQ((int32_t)k_ra_pdg_test_code_hi,
                 (int32_t)reg->GTDLYR[(uint8_t)k_ra_pdg_test_ch_first].A);
  TEST_END("pdg capture_start happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_null(void)
{
  TEST_BEGIN("pdg capture_start NULL buf");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdg_capture_start(nullptr, 1U));
  TEST_END("pdg capture_start NULL buf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_zero_len(void)
{
  TEST_BEGIN("pdg capture_start zero len");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  const ra_pdg_delay_entry_t entries[1] = {
    {.channel = 0U, .pin = k_ra_pdg_pin_a, .edge = k_ra_pdg_edge_rising, .code = 0U},
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_capture_start(entries, 0U));
  TEST_END("pdg capture_start zero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_bad_entry(void)
{
  TEST_BEGIN("pdg capture_start bad entry");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  const ra_pdg_delay_entry_t bad[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_bad,
     .pin     = k_ra_pdg_pin_a,
     .edge    = k_ra_pdg_edge_rising,
     .code    = 0U},
  };
  const ra_err_t err = ra_pdg_capture_start(bad, 1U);
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("pdg capture_start bad entry");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_stop_happy(void)
{
  TEST_BEGIN("pdg capture_stop");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_capture_stop());
  TEST_END("pdg capture_stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_capture_start_no_handler(void)
{
  TEST_BEGIN("pdg capture_start without handler");
  prep();
  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_attach_handler(nullptr, nullptr));
  s_pdg_cb_count                       = 0U;
  const ra_pdg_delay_entry_t entries[] = {
    {.channel = (uint8_t)k_ra_pdg_test_ch_first,
     .pin     = k_ra_pdg_pin_b,
     .edge    = k_ra_pdg_edge_falling,
     .code    = 0x10U},
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_capture_start(entries, 1U));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_pdg_cb_count);
  TEST_END("pdg capture_start without handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("pdg deinit");
  prep();

  const ra_pdg_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));

  /* Dirty a delay register so we can verify it gets wiped. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_set_delay((uint8_t)k_ra_pdg_test_ch_first,
                                           k_ra_pdg_pin_a,
                                           k_ra_pdg_edge_rising,
                                           (uint8_t)k_ra_pdg_test_code_hi));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_deinit());

  volatile r_pdg_regs_t* reg = ra_pdg();
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dllen) == 0U);
  TEST_ASSERT((reg->GTDLYCR & (uint16_t)k_ra_pdg_gtdlycr_mask_dlyrst) != 0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->GTDLYCR2);
  /* Every delay cell wiped to 0. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->GTDLYR[(uint8_t)k_ra_pdg_test_ch_first].A);
  TEST_END("pdg deinit");
}

/**
 * @test test_mcdc_pdg
 *
 * @par MC/DC:
 * Decision A: ``ra_pdg_check_constraints`` line 805,
 * libs/ra_hal/src/ra_pdg.c:
 * ``if ((mode != k_ra_pdg_wave_saw) && (mode != k_ra_pdg_wave_triangle))``
 * (2 conditions). N+1 = 3 vectors:
 * - V1: mode=saw      -> C1=F (short-circuits)        -> dec F (->ok)
 * - V2: mode=triangle -> C1=T,C2=F                    -> dec F (->ok)
 * - V3: mode=bogus    -> C1=T,C2=T                    -> dec T (->invalid_arg)
 * (V1,V3) flips C1 with C2 fixed; (V2,V3) flips C2 with C1 fixed.
 *
 * Decision B: ``ra_pdg_check_constraints`` line 808,
 * ``if ((dir != k_ra_pdg_dir_up) && (dir != k_ra_pdg_dir_down))``
 * (2 conditions). Same N+1 = 3 vector shape on ``dir``; mode pinned
 * to k_ra_pdg_wave_saw to bypass decision A.
 */
static void test_mcdc_pdg(void)
{
  TEST_BEGIN("pdg MC/DC: check_constraints mode + dir 2-cond rejects");
  /* Decision A: mode validation. Use a benign dir + gtpr/cm so the
   * downstream constraint checks do not poison the A-result. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw, k_ra_pdg_dir_down, 100U, 200U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_triangle, k_ra_pdg_dir_up, 100U, 200U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_pdg_check_constraints((ra_pdg_wave_mode_t)0xFFU, k_ra_pdg_dir_up, 100U, 200U));

  /* Decision B: dir validation, with mode held at saw (passes A). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw, k_ra_pdg_dir_up, 100U, 200U));
  /* dir=down already exercised in V1 of decision A above. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_pdg_check_constraints(k_ra_pdg_wave_saw, (ra_pdg_count_dir_t)0xFFU, 100U, 200U));
  TEST_END("pdg MC/DC: check_constraints mode + dir 2-cond rejects");
}

/**
 * @test test_mcdc_set_delay_batch_count
 *
 * @par MC/DC:
 * Decision: ``if ((count == 0U) || (count > k_ra_pdg_slot_count))``
 * (2 conditions, libs/ra_hal/src/ra_pdg.c ra_pdg_set_delay_batch). N+1 = 3.
 * - V1: count=1 (in range)               -> C1=F short-circuits   -> F (proceeds)
 * - V2: count=k_ra_pdg_slot_count (max)  -> C1=F, C2=F            -> F (proceeds)
 * - V3: count=k_ra_pdg_slot_count+1 (oor) -> C1=F, C2=T           -> T -> invalid_arg
 * - V4: count=0                          -> C1=T short-circuits   -> T -> invalid_arg
 * (V1,V4) flips C1 with C2 masked F; (V2,V3) flips C2 with C1 fixed F. N+1=3.
 */
static void test_mcdc_set_delay_batch_count(void)
{
  TEST_BEGIN("pdg MC/DC: set_delay_batch count == 0 || count > slot_count");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_init(&(const ra_pdg_config_t){
                   .frange       = k_ra_pdg_frange_80_160_mhz,
                   .channel_mask = (uint8_t)k_ra_pdg_test_mask_all,
                   .auto_tune    = 0U,
                   .gptclk_hz    = 0U,
                 }));
  const ra_pdg_delay_entry_t one[1] = {{
    .channel = (uint8_t)k_ra_pdg_test_ch_first,
    .pin     = k_ra_pdg_pin_a,
    .edge    = k_ra_pdg_edge_rising,
    .code    = (uint8_t)k_ra_pdg_test_code_lo,
  }};
  /* V1: count=1 in range -> proceeds */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_set_delay_batch(one, 1U));
  /* V4: count=0 -> C1=T -> invalid_arg */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdg_set_delay_batch(one, 0U));
  /* V3: count > slot_count -> C1=F, C2=T -> invalid_arg */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_set_delay_batch(one, (uint8_t)(k_ra_pdg_slot_count + 1U)));
  TEST_END("pdg MC/DC: set_delay_batch count == 0 || count > slot_count");
}

/**
 * @test test_mcdc_pin_disable_pin_neither
 *
 * @par MC/DC:
 * Decision: ``if ((pin != k_ra_pdg_pin_a) && (pin != k_ra_pdg_pin_b))``
 * (2 conditions, libs/ra_hal/src/ra_pdg.c ra_pdg_pin_disable). N+1 = 3.
 * - V1: pin=pin_a   -> C1=F short-circuits          -> F (proceeds, ok)
 * - V2: pin=pin_b   -> C1=T, C2=F                   -> F (proceeds, ok)
 * - V3: pin=0xFE    -> C1=T, C2=T                   -> T -> invalid_arg
 * (V1,V3) flips C1 with C2 set T; (V2,V3) flips C2 with C1 set T.
 */
static void test_mcdc_pin_disable_pin_neither(void)
{
  TEST_BEGIN("pdg MC/DC: pin_disable pin != A && pin != B");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_init(&(const ra_pdg_config_t){
                   .frange       = k_ra_pdg_frange_80_160_mhz,
                   .channel_mask = (uint8_t)k_ra_pdg_test_mask_all,
                   .auto_tune    = 0U,
                   .gptclk_hz    = 0U,
                 }));
  /* V1: pin_a */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, k_ra_pdg_pin_a));
  /* V2: pin_b */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, k_ra_pdg_pin_b));
  /* V3: bogus */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_pdg_pin_disable((uint8_t)k_ra_pdg_test_ch_first, (ra_pdg_pin_t)0xFEU));
  TEST_END("pdg MC/DC: pin_disable pin != A && pin != B");
}

/**
 * @test test_mcdc_validate_cfg_gptclk_range
 *
 * @par MC/DC:
 * Decision: ``if ((cfg->gptclk_hz < freq_low_min) || (cfg->gptclk_hz > freq_high_max))``
 * (2 conditions, internal_validate_cfg auto-tune branch). N+1 = 3.
 * Exercised through ra_pdg_init since internal_validate_cfg is private.
 * - V1: gptclk in band (e.g. 100 MHz)  -> C1=F, C2=F       -> dec F (->ok)
 * - V2: gptclk too low (50 MHz)        -> C1=T short-circuits -> dec T -> out_of_range
 * - V3: gptclk too high (400 MHz)      -> C1=F, C2=T       -> dec T -> out_of_range
 * (V1,V2) flips C1 with C2 masked; (V1,V3) flips C2 with C1 fixed F.
 */
static void test_mcdc_validate_cfg_gptclk_range(void)
{
  TEST_BEGIN("pdg MC/DC: validate_cfg auto-tune gptclk band");
  prep();
  ra_pdg_config_t cfg = {
    .frange       = k_ra_pdg_frange_80_160_mhz,
    .channel_mask = (uint8_t)k_ra_pdg_test_mask_one,
    .auto_tune    = 1U,
    .gptclk_hz    = (uint32_t)k_ra_pdg_test_clk_low_band,
  };
  /* V1: 100 MHz in band -> ok. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdg_deinit());
  /* V2: 50 MHz too low -> out_of_range. */
  cfg.gptclk_hz = (uint32_t)k_ra_pdg_test_clk_too_low;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range, (int32_t)ra_pdg_init(&cfg));
  /* V3: 400 MHz too high -> out_of_range. */
  cfg.gptclk_hz = (uint32_t)k_ra_pdg_test_clk_too_high;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range, (int32_t)ra_pdg_init(&cfg));
  TEST_END("pdg MC/DC: validate_cfg auto-tune gptclk band");
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
  test_set_delay_not_initialised();
  test_delay_ns_to_code_low_band();
  test_power_transition();
  test_channel_bypass_set();
  test_pin_disable();
  test_status_read_and_clear();
  test_status_full();
  test_pick_frange();
  test_set_frange_runtime();
  test_bind_unbind_gpt_channel();
  test_check_constraints();
  test_required_write_ns();
  test_attach_and_dispatch();
  test_capture_start_happy();
  test_capture_start_null();
  test_capture_start_zero_len();
  test_capture_start_bad_entry();
  test_capture_stop_happy();
  test_capture_start_no_handler();
  test_deinit();
  test_mcdc_pdg();
  test_mcdc_set_delay_batch_count();
  test_mcdc_pin_disable_pin_neither();
  test_mcdc_validate_cfg_gptclk_range();
  (void)fprintf(stderr, "[OK ] test_ra_pdg.c\n");
  return 0;
}
