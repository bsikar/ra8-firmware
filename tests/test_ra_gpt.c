/**
 * @file test_ra_gpt.c
 * @brief Unit tests for ra_gpt.c (General PWM Timer)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_gpt_regs.h"
#include "ra_err.h"
#include "ra_gpt.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_gpt_test_channel_valid  = 0U,
  k_ra_gpt_test_channel_middle = 7U,
  k_ra_gpt_test_channel_last   = 13U,
  k_ra_gpt_test_channel_bad    = 14U,
  k_ra_gpt_test_channel_huge   = 250U,
} ra_gpt_test_channel_t;

typedef enum : uint32_t {
  k_ra_gpt_test_period   = 0xCAFEBABEUL,
  k_ra_gpt_test_count    = 0x01234567UL,
  k_ra_gpt_test_gtstp1   = 0x00000001UL,
  k_ra_gpt_test_gtstr1   = 0x00000001UL,
  k_ra_gpt_test_gtcr_saw = 0x00000001UL,
} ra_gpt_test_const_t;

static void test_start_happy(void)
{
  TEST_BEGIN("gpt start happy channel 0");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_gpt_start_free_run((uint8_t)k_ra_gpt_test_channel_valid,
                                            (uint32_t)k_ra_gpt_test_period));
  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_gpt_test_period, (int)reg->GTPR);
  TEST_ASSERT_EQ(0, (int)reg->GTCNT);
  TEST_ASSERT_EQ((int)k_ra_gpt_test_gtcr_saw, (int)reg->GTCR);
  TEST_ASSERT_EQ((int)k_ra_gpt_test_gtstr1, (int)reg->GTSTR);
  TEST_END("gpt start happy channel 0");
}

static void test_start_last_channel(void)
{
  TEST_BEGIN("gpt start last channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_gpt_start_free_run((uint8_t)k_ra_gpt_test_channel_last, 0U));
  TEST_END("gpt start last channel");
}

static void test_start_bad_channel(void)
{
  TEST_BEGIN("gpt start bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_gpt_start_free_run((uint8_t)k_ra_gpt_test_channel_bad, 0U));
  TEST_END("gpt start bad channel");
}

static void test_start_huge_channel(void)
{
  TEST_BEGIN("gpt start huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_gpt_start_free_run((uint8_t)k_ra_gpt_test_channel_huge, 0U));
  TEST_END("gpt start huge channel");
}

static void test_stop_happy(void)
{
  TEST_BEGIN("gpt stop happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_gpt_start_free_run((uint8_t)k_ra_gpt_test_channel_valid,
                                            (uint32_t)k_ra_gpt_test_period));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_gpt_stop((uint8_t)k_ra_gpt_test_channel_valid));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int)k_ra_gpt_test_gtstp1, (int)reg->GTSTP);
  TEST_END("gpt stop happy");
}

static void test_stop_bad_channel(void)
{
  TEST_BEGIN("gpt stop bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_gpt_stop((uint8_t)k_ra_gpt_test_channel_bad));
  TEST_END("gpt stop bad channel");
}

static void test_read_happy(void)
{
  TEST_BEGIN("gpt read happy");
  ra_sim_mmap_reset();

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  reg->GTCNT                         = (uint32_t)k_ra_gpt_test_count;

  uint32_t       out = 0U;
  const ra_err_t err = ra_gpt_read((uint8_t)k_ra_gpt_test_channel_valid, &out);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_gpt_test_count, (int)out);
  TEST_END("gpt read happy");
}

static void test_read_null_out(void)
{
  TEST_BEGIN("gpt read null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_gpt_read((uint8_t)k_ra_gpt_test_channel_valid, nullptr));
  TEST_END("gpt read null out");
}

static void test_read_bad_channel(void)
{
  TEST_BEGIN("gpt read bad channel");
  ra_sim_mmap_reset();

  uint32_t out = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_gpt_read((uint8_t)k_ra_gpt_test_channel_bad, &out));
  TEST_END("gpt read bad channel");
}

/* ---------------------------------------------------------------------------
 * Wave 3.5 -- full build-out
 * ---------------------------------------------------------------------------
 */

typedef enum : uint32_t {
  k_ra_gpt_test_duty_a = 0x00010000UL,
  k_ra_gpt_test_duty_b = 0x00008000UL,
} ra_gpt_test_duty_t;

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
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_gpt_cb_count     = 0U;
  s_gpt_cb_last_mask = 0U;
  s_gpt_cb_last_ctx  = nullptr;
}

static ra_gpt_cfg_t make_gpt_cfg(void)
{
  const ra_gpt_cfg_t cfg = {
    .mode       = k_ra_gpt_mode_saw_pwm,
    .prescaler  = k_ra_gpt_ps_div_4,
    .period     = (uint32_t)k_ra_gpt_test_period,
    .duty_a     = (uint32_t)k_ra_gpt_test_duty_a,
    .duty_b     = (uint32_t)k_ra_gpt_test_duty_b,
    .auto_start = true,
  };
  return cfg;
}

static void test_gpt_init_configured(void)
{
  TEST_BEGIN("gpt init configured");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_test_period, (int32_t)reg->GTPR);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_test_period, (int32_t)reg->GTPBR);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_test_duty_a, (int32_t)reg->GTCCR[0]);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_test_duty_b, (int32_t)reg->GTCCR[1]);
  TEST_END("gpt init configured");
}

static void test_gpt_init_null_cfg(void)
{
  TEST_BEGIN("gpt init null cfg");
  prep_w35();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, nullptr));
  TEST_END("gpt init null cfg");
}

static void test_gpt_init_bad_channel(void)
{
  TEST_BEGIN("gpt init bad channel");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_bad, &cfg));
  TEST_END("gpt init bad channel");
}

static void test_gpt_init_no_autostart(void)
{
  TEST_BEGIN("gpt init no autostart");
  prep_w35();

  ra_gpt_cfg_t cfg = make_gpt_cfg();
  cfg.auto_start   = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_END("gpt init no autostart");
}

static void test_gpt_deinit(void)
{
  TEST_BEGIN("gpt deinit");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_deinit((uint8_t)k_ra_gpt_test_channel_valid));
  TEST_END("gpt deinit");
}

static void test_gpt_deinit_bad_channel(void)
{
  TEST_BEGIN("gpt deinit bad channel");
  prep_w35();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_deinit((uint8_t)k_ra_gpt_test_channel_bad));
  TEST_END("gpt deinit bad channel");
}

static void test_gpt_set_period(void)
{
  TEST_BEGIN("gpt set_period");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_set_period((uint8_t)k_ra_gpt_test_channel_valid, 0xAABBCCDDUL));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)0xAABBCCDDUL, (int32_t)reg->GTPR);
  TEST_END("gpt set_period");
}

static void test_gpt_set_duty_a_and_b(void)
{
  TEST_BEGIN("gpt set_duty A/B");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_gpt_set_duty((uint8_t)k_ra_gpt_test_channel_valid, k_ra_gpt_ccr_a, 0x1234U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_gpt_set_duty((uint8_t)k_ra_gpt_test_channel_valid, k_ra_gpt_ccr_b, 0x5678U));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)0x1234U, (int32_t)reg->GTCCR[0]);
  TEST_ASSERT_EQ((int32_t)0x5678U, (int32_t)reg->GTCCR[1]);
  TEST_END("gpt set_duty A/B");
}

static void test_gpt_set_duty_bad_sel(void)
{
  TEST_BEGIN("gpt set_duty bad sel");
  prep_w35();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_gpt_set_duty((uint8_t)k_ra_gpt_test_channel_valid, (ra_gpt_ccr_sel_t)9U, 0U));
  TEST_END("gpt set_duty bad sel");
}

static void test_gpt_status_read_and_clear(void)
{
  TEST_BEGIN("gpt status read + clear");
  prep_w35();

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  reg->GTST = (uint32_t)k_ra_gpt_status_overflow | (uint32_t)k_ra_gpt_status_ccra;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_get_status((uint8_t)k_ra_gpt_test_channel_valid, &mask));
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_status_overflow | (uint32_t)k_ra_gpt_status_ccra),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_clear_status((uint8_t)k_ra_gpt_test_channel_valid,
                                              (uint32_t)k_ra_gpt_status_overflow));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_get_status((uint8_t)k_ra_gpt_test_channel_valid, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_status_ccra, (int32_t)mask);
  TEST_END("gpt status read + clear");
}

static void test_gpt_attach_and_dispatch_ovf(void)
{
  TEST_BEGIN("gpt attach + dispatch ovf");
  prep_w35();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_attach_handler((uint8_t)k_ra_gpt_test_channel_valid,
                                                stub_gpt_cb,
                                                (void*)(uintptr_t)0xBEEFU));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  reg->GTST                          = (uint32_t)k_ra_gpt_status_overflow;
  ra_gpt_dispatch_ovf((uint8_t)k_ra_gpt_test_channel_valid);

  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_gpt_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_status_overflow, (int32_t)s_gpt_cb_last_mask);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->GTST);
  TEST_END("gpt attach + dispatch ovf");
}

static void test_gpt_dispatch_und_ccra_ccrb(void)
{
  TEST_BEGIN("gpt dispatch und + ccra + ccrb");
  prep_w35();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_gpt_attach_handler((uint8_t)k_ra_gpt_test_channel_valid, stub_gpt_cb, nullptr));

  ra_gpt_dispatch_und((uint8_t)k_ra_gpt_test_channel_valid);
  ra_gpt_dispatch_ccra((uint8_t)k_ra_gpt_test_channel_valid);
  ra_gpt_dispatch_ccrb((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)3, (int32_t)s_gpt_cb_count);
  TEST_END("gpt dispatch und + ccra + ccrb");
}

static void test_gpt_dispatch_no_handler(void)
{
  TEST_BEGIN("gpt dispatch no handler");
  prep_w35();

  /* Detach any handler on the channels we touched. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_gpt_attach_handler((uint8_t)k_ra_gpt_test_channel_valid, nullptr, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_gpt_attach_handler((uint8_t)k_ra_gpt_test_channel_middle, nullptr, nullptr));

  ra_gpt_dispatch_ovf((uint8_t)k_ra_gpt_test_channel_valid);
  ra_gpt_dispatch_ovf((uint8_t)k_ra_gpt_test_channel_bad);
  ra_gpt_dispatch_ovf((uint8_t)k_ra_gpt_test_channel_middle);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_gpt_cb_count);
  TEST_END("gpt dispatch no handler");
}

static void test_gpt_attach_bad_channel(void)
{
  TEST_BEGIN("gpt attach bad channel");
  prep_w35();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_gpt_attach_handler((uint8_t)k_ra_gpt_test_channel_bad, stub_gpt_cb, nullptr));
  TEST_END("gpt attach bad channel");
}

static void test_gpt_power_transition(void)
{
  TEST_BEGIN("gpt power transition");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_enter_stop((uint8_t)k_ra_gpt_test_channel_valid));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_exit_stop((uint8_t)k_ra_gpt_test_channel_valid));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_enter_stop((uint8_t)k_ra_gpt_test_channel_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_exit_stop((uint8_t)k_ra_gpt_test_channel_bad));
  TEST_END("gpt power transition");
}

int32_t main(void)
{
  test_start_happy();
  test_start_last_channel();
  test_start_bad_channel();
  test_start_huge_channel();
  test_stop_happy();
  test_stop_bad_channel();
  test_read_happy();
  test_read_null_out();
  test_read_bad_channel();
  test_gpt_init_configured();
  test_gpt_init_null_cfg();
  test_gpt_init_bad_channel();
  test_gpt_init_no_autostart();
  test_gpt_deinit();
  test_gpt_deinit_bad_channel();
  test_gpt_set_period();
  test_gpt_set_duty_a_and_b();
  test_gpt_set_duty_bad_sel();
  test_gpt_status_read_and_clear();
  test_gpt_attach_and_dispatch_ovf();
  test_gpt_dispatch_und_ccra_ccrb();
  test_gpt_dispatch_no_handler();
  test_gpt_attach_bad_channel();
  test_gpt_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_gpt.c\n");
  return 0;
}
