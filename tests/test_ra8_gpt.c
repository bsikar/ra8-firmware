/**
 * @file test_ra8_gpt.c
 * @brief Unit tests for ra8_gpt.c (General PWM Timer)
 *
 * @details
 * This sibling owns the basic start / stop / read / init / dispatch contract
 * tests. The DMA streaming, runtime-PWM, dead-time, three-phase, and MC/DC
 * vector tests live in test_ra8_gpt_pwm_dma.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dma.h"
#include "ra8_err.h"
#include "ra8_fake_dma.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_gpt_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra8_gpt_test_channel_valid  = 0U,   /**< RA8 GPT test channel valid.  */
  k_ra8_gpt_test_channel_middle = 7U,   /**< RA8 GPT test channel middle. */
  k_ra8_gpt_test_channel_last   = 13U,  /**< RA8 GPT test channel last.   */
  k_ra8_gpt_test_channel_bad    = 14U,  /**< RA8 GPT test channel bad.    */
  k_ra8_gpt_test_channel_huge   = 250U, /**< RA8 GPT test channel huge.   */
} ra8_gpt_test_channel_t;

typedef enum : uint32_t {
  k_ra8_gpt_test_period   = 0xCAFEBABEUL, /**< RA8 GPT test period.   */
  k_ra8_gpt_test_count    = 0x01234567UL, /**< RA8 GPT test count.    */
  k_ra8_gpt_test_gtstp1   = 0x00000001UL, /**< RA8 GPT test gtstp1.   */
  k_ra8_gpt_test_gtstr1   = 0x00000001UL, /**< RA8 GPT test gtstr1.   */
  k_ra8_gpt_test_gtcr_saw = 0x00000001UL, /**< RA8 GPT test gtcr saw. */
} ra8_gpt_test_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_happy(void)
{
  TEST_BEGIN("gpt start happy channel 0");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_start_free_run((uint8_t)k_ra8_gpt_test_channel_valid, (uint32_t)k_ra8_gpt_test_period));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_gpt_test_period, reg->GTPR);
  TEST_ASSERT_EQ(0, reg->GTCNT);
  TEST_ASSERT_EQ(k_ra8_gpt_test_gtcr_saw, reg->GTCR);
  TEST_ASSERT_EQ(k_ra8_gpt_test_gtstr1, reg->GTSTR);
  TEST_END("gpt start happy channel 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_last_channel(void)
{
  TEST_BEGIN("gpt start last channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_start_free_run((uint8_t)k_ra8_gpt_test_channel_last, 0U));
  TEST_END("gpt start last channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_bad_channel(void)
{
  TEST_BEGIN("gpt start bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_gpt_start_free_run((uint8_t)k_ra8_gpt_test_channel_bad, 0U));
  TEST_END("gpt start bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_huge_channel(void)
{
  TEST_BEGIN("gpt start huge channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_gpt_start_free_run((uint8_t)k_ra8_gpt_test_channel_huge, 0U));
  TEST_END("gpt start huge channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_happy(void)
{
  TEST_BEGIN("gpt stop happy");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_start_free_run((uint8_t)k_ra8_gpt_test_channel_valid, (uint32_t)k_ra8_gpt_test_period));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_stop((uint8_t)k_ra8_gpt_test_channel_valid));

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_EQ(k_ra8_gpt_test_gtstp1, reg->GTSTP);
  TEST_END("gpt stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("gpt stop bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_stop((uint8_t)k_ra8_gpt_test_channel_bad));
  TEST_END("gpt stop bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_happy(void)
{
  TEST_BEGIN("gpt read happy");
  ra8_fake_mmap_reset();

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  reg->GTCNT                         = (uint32_t)k_ra8_gpt_test_count;

  uint32_t        out = 0U;
  const ra8_err_t err = ra8_gpt_read((uint8_t)k_ra8_gpt_test_channel_valid, &out);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_ra8_gpt_test_count, out);
  TEST_END("gpt read happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_null_out(void)
{
  TEST_BEGIN("gpt read null out");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_read((uint8_t)k_ra8_gpt_test_channel_valid, nullptr));
  TEST_END("gpt read null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_bad_channel(void)
{
  TEST_BEGIN("gpt read bad channel");
  ra8_fake_mmap_reset();

  uint32_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_read((uint8_t)k_ra8_gpt_test_channel_bad, &out));
  TEST_END("gpt read bad channel");
}

/* ---------------------------------------------------------------------------
 * full build-out
 * 
 */

typedef enum : uint32_t {
  k_ra8_gpt_test_duty_a = 0x00010000UL, /**< RA8 GPT test duty a. */
  k_ra8_gpt_test_duty_b = 0x00008000UL, /**< RA8 GPT test duty b. */
} ra8_gpt_test_duty_t;

static uint32_t s_gpt_cb_count;
static uint32_t s_gpt_cb_last_mask;
static void*    s_gpt_cb_last_ctx;

static void stub_gpt_cb(void* ctx, uint32_t mask)
{
  ++s_gpt_cb_count;
  s_gpt_cb_last_mask = mask;
  s_gpt_cb_last_ctx  = ctx;
}

static void prep_w35(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_gpt_cb_count     = 0U;
  s_gpt_cb_last_mask = 0U;
  s_gpt_cb_last_ctx  = nullptr;
}

static ra8_gpt_cfg_t make_gpt_cfg(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_ra8_gpt_test_period,
    .duty_a     = (uint32_t)k_ra8_gpt_test_duty_a,
    .duty_b     = (uint32_t)k_ra8_gpt_test_duty_b,
    .auto_start = true,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_init_configured(void)
{
  TEST_BEGIN("gpt init configured");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_gpt_test_period, reg->GTPR);
  TEST_ASSERT_EQ(k_ra8_gpt_test_period, reg->GTPBR);
  TEST_ASSERT_EQ(k_ra8_gpt_test_duty_a, reg->GTCCR[0]);
  TEST_ASSERT_EQ(k_ra8_gpt_test_duty_b, reg->GTCCR[1]);
  TEST_END("gpt init configured");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_init_null_cfg(void)
{
  TEST_BEGIN("gpt init null cfg");
  prep_w35();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, nullptr));
  TEST_END("gpt init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_init_bad_channel(void)
{
  TEST_BEGIN("gpt init bad channel");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_bad, &cfg));
  TEST_END("gpt init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_init_no_autostart(void)
{
  TEST_BEGIN("gpt init no autostart");
  prep_w35();

  ra8_gpt_cfg_t cfg = make_gpt_cfg();
  cfg.auto_start    = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));
  TEST_END("gpt init no autostart");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_deinit(void)
{
  TEST_BEGIN("gpt deinit");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_deinit((uint8_t)k_ra8_gpt_test_channel_valid));
  TEST_END("gpt deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_deinit_bad_channel(void)
{
  TEST_BEGIN("gpt deinit bad channel");
  prep_w35();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gpt_deinit((uint8_t)k_ra8_gpt_test_channel_bad));
  TEST_END("gpt deinit bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_set_period(void)
{
  TEST_BEGIN("gpt set_period");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_set_period((uint8_t)k_ra8_gpt_test_channel_valid, 0xAABBCCDDUL));

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_EQ(0xAABBCCDDUL, reg->GTPR);
  TEST_END("gpt set_period");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_set_duty_a_and_b(void)
{
  TEST_BEGIN("gpt set_duty A/B");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_set_duty((uint8_t)k_ra8_gpt_test_channel_valid, k_ra8_gpt_ccr_a, 0x1234U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_set_duty((uint8_t)k_ra8_gpt_test_channel_valid, k_ra8_gpt_ccr_b, 0x5678U));

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_EQ(0x1234U, reg->GTCCR[0]);
  TEST_ASSERT_EQ(0x5678U, reg->GTCCR[1]);
  TEST_END("gpt set_duty A/B");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_set_duty_bad_sel(void)
{
  TEST_BEGIN("gpt set_duty bad sel");
  prep_w35();

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_gpt_set_duty((uint8_t)k_ra8_gpt_test_channel_valid, (ra8_gpt_ccr_sel_t)9U, 0U));
  TEST_END("gpt set_duty bad sel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_status_read_and_clear(void)
{
  TEST_BEGIN("gpt status read + clear");
  prep_w35();

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  reg->GTST = (uint32_t)k_ra8_gpt_status_overflow | (uint32_t)k_ra8_gpt_status_ccra;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_get_status((uint8_t)k_ra8_gpt_test_channel_valid, &mask));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_gpt_status_overflow | (uint32_t)k_ra8_gpt_status_ccra), mask);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_clear_status((uint8_t)k_ra8_gpt_test_channel_valid,
                                      (uint32_t)k_ra8_gpt_status_overflow));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_get_status((uint8_t)k_ra8_gpt_test_channel_valid, &mask));
  TEST_ASSERT_EQ(k_ra8_gpt_status_ccra, mask);
  TEST_END("gpt status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_attach_and_dispatch_ovf(void)
{
  TEST_BEGIN("gpt attach + dispatch ovf");
  prep_w35();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_attach_handler((uint8_t)k_ra8_gpt_test_channel_valid,
                                        stub_gpt_cb,
                                        (void*)(uintptr_t)0xBEEFU));

  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_ra8_gpt_test_channel_valid);
  reg->GTST                          = (uint32_t)k_ra8_gpt_status_overflow;
  ra8_gpt_dispatch_ovf((uint8_t)k_ra8_gpt_test_channel_valid);

  TEST_ASSERT_EQ(1, s_gpt_cb_count);
  TEST_ASSERT_EQ(k_ra8_gpt_status_overflow, s_gpt_cb_last_mask);
  TEST_ASSERT_EQ(0, reg->GTST);
  TEST_END("gpt attach + dispatch ovf");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_dispatch_und_ccra_ccrb(void)
{
  TEST_BEGIN("gpt dispatch und + ccra + ccrb");
  prep_w35();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_attach_handler((uint8_t)k_ra8_gpt_test_channel_valid, stub_gpt_cb, nullptr));

  ra8_gpt_dispatch_und((uint8_t)k_ra8_gpt_test_channel_valid);
  ra8_gpt_dispatch_ccra((uint8_t)k_ra8_gpt_test_channel_valid);
  ra8_gpt_dispatch_ccrb((uint8_t)k_ra8_gpt_test_channel_valid);
  TEST_ASSERT_EQ(3, s_gpt_cb_count);
  TEST_END("gpt dispatch und + ccra + ccrb");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_dispatch_no_handler(void)
{
  TEST_BEGIN("gpt dispatch no handler");
  prep_w35();

  /* Detach any handler on the channels we touched. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_attach_handler((uint8_t)k_ra8_gpt_test_channel_valid, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_attach_handler((uint8_t)k_ra8_gpt_test_channel_middle, nullptr, nullptr));

  ra8_gpt_dispatch_ovf((uint8_t)k_ra8_gpt_test_channel_valid);
  ra8_gpt_dispatch_ovf((uint8_t)k_ra8_gpt_test_channel_bad);
  ra8_gpt_dispatch_ovf((uint8_t)k_ra8_gpt_test_channel_middle);
  TEST_ASSERT_EQ(0, s_gpt_cb_count);
  TEST_END("gpt dispatch no handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_attach_bad_channel(void)
{
  TEST_BEGIN("gpt attach bad channel");
  prep_w35();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gpt_attach_handler((uint8_t)k_ra8_gpt_test_channel_bad, stub_gpt_cb, nullptr));
  TEST_END("gpt attach bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_gpt_power_transition(void)
{
  TEST_BEGIN("gpt power transition");
  prep_w35();

  const ra8_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_init((uint8_t)k_ra8_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_enter_stop((uint8_t)k_ra8_gpt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_exit_stop((uint8_t)k_ra8_gpt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gpt_enter_stop((uint8_t)k_ra8_gpt_test_channel_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gpt_exit_stop((uint8_t)k_ra8_gpt_test_channel_bad));
  TEST_END("gpt power transition");
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
  test_start_happy,
  test_start_last_channel,
  test_start_bad_channel,
  test_start_huge_channel,
  test_stop_happy,
  test_stop_bad_channel,
  test_read_happy,
  test_read_null_out,
  test_read_bad_channel,
  test_gpt_init_configured,
  test_gpt_init_null_cfg,
  test_gpt_init_bad_channel,
  test_gpt_init_no_autostart,
  test_gpt_deinit,
  test_gpt_deinit_bad_channel,
  test_gpt_set_period,
  test_gpt_set_duty_a_and_b,
  test_gpt_set_duty_bad_sel,
  test_gpt_status_read_and_clear,
  test_gpt_attach_and_dispatch_ovf,
  test_gpt_dispatch_und_ccra_ccrb,
  test_gpt_dispatch_no_handler,
  test_gpt_attach_bad_channel,
  test_gpt_power_transition,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_gpt.c\n");
  return 0;
}
