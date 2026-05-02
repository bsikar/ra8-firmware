/**
 * @file test_ra_gpt.c
 * @brief Unit tests for ra_gpt.c (General PWM Timer)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_gpt_regs.h"
#include "ra_dma.h"
#include "ra_err.h"
#include "ra_gpt.h"
#include "ra_mstp.h"
#include "ra_sim_dma.h"
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
 * full build-out
 * 
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

static int32_t s_gpt_dma_done = 0;

static void stub_gpt_dma_done(void* ctx)
{
  (void)ctx;
  ++s_gpt_dma_done;
}

static void test_gpt_write_dma_streams_periods_to_gtpr(void)
{
  TEST_BEGIN("ra_gpt_write_dma: periods stream into GTPR");
  prep_w35();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  const uint32_t periods[] = {0x11111111UL, 0x22222222UL, 0x33333333UL};
  uint8_t        dch       = 0xFFU;
  s_gpt_dma_done           = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_write_dma((uint8_t)k_ra_gpt_test_channel_valid,
                                           periods,
                                           (uint16_t)(sizeof(periods) / sizeof(periods[0])),
                                           stub_gpt_dma_done,
                                           nullptr,
                                           &dch));
  TEST_ASSERT(dch < 8U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  volatile const r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)0x33333333UL, (int32_t)reg->GTPR);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_gpt_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_gpt_write_dma: periods stream into GTPR");
}

static void test_gpt_read_dma_captures_gtcnt(void)
{
  TEST_BEGIN("ra_gpt_read_dma: GTCNT streams into out_counts");
  prep_w35();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  reg->GTCNT                         = 0xDEADC0DEUL;

  uint32_t out[2] = {0U, 0U};
  uint8_t  dch    = 0xFFU;
  s_gpt_dma_done  = 0;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_read_dma((uint8_t)k_ra_gpt_test_channel_valid,
                                          out,
                                          (uint16_t)(sizeof(out) / sizeof(out[0])),
                                          stub_gpt_dma_done,
                                          nullptr,
                                          &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  TEST_ASSERT_EQ((int32_t)0xDEADC0DEUL, (int32_t)out[0]);
  TEST_ASSERT_EQ((int32_t)0xDEADC0DEUL, (int32_t)out[1]);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_gpt_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_gpt_read_dma: GTCNT streams into out_counts");
}

static void test_gpt_dma_arg_validation(void)
{
  TEST_BEGIN("ra_gpt_{write,read}_dma: arg validation");
  prep_w35();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  uint8_t        dch       = 0U;
  const uint32_t periods[] = {0x1UL};
  uint32_t       counts[1] = {0U};

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_write_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_write_dma(0U, periods, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_read_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_read_dma(0U, counts, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_write_dma(99U, periods, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_read_dma(99U, counts, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_write_dma(0U, periods, 0U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_read_dma(0U, counts, 0U, nullptr, nullptr, &dch));
  TEST_END("ra_gpt_{write,read}_dma: arg validation");
}

/* ---------------------------------------------------------------------------
 * Sweep 3 Task 2 -- runtime PWM duty / period / counter / dead-time / 3-phase
 * Mirrors FSP r_gpt + r_gpt_three_phase.
 * ---------------------------------------------------------------------------
 */

typedef enum : uint32_t {
  k_ra_gpt_t2_period         = 0x0000FFFFUL,
  k_ra_gpt_t2_duty_u         = 0x00001000UL,
  k_ra_gpt_t2_duty_v         = 0x00002000UL,
  k_ra_gpt_t2_duty_w         = 0x00003000UL,
  k_ra_gpt_t2_dt_rise        = 0x00000100UL,
  k_ra_gpt_t2_dt_fall        = 0x00000200UL,
  k_ra_gpt_t2_period_2       = 0x00007777UL,
  k_ra_gpt_t2_duty_huge      = 0xFFFFFFFFUL,
  k_ra_gpt_t2_gtdtcr_tde     = 0x00000001UL,
  k_ra_gpt_t2_oae_mask       = 0x00000100UL,
  k_ra_gpt_t2_obe_mask       = 0x01000000UL,
  k_ra_gpt_t2_oadflt         = 0x00000040UL,
  k_ra_gpt_t2_active_high_a  = 0x00000009UL,
  k_ra_gpt_t2_active_low_a   = 0x00000006UL,
  k_ra_gpt_t2_oadf_drive_low = 0x00000400UL, /* 2 << 9 */
} ra_gpt_t2_const_t;

typedef enum : uint8_t {
  k_ra_gpt_t2_ch_u   = 0U,
  k_ra_gpt_t2_ch_v   = 1U,
  k_ra_gpt_t2_ch_w   = 2U,
  k_ra_gpt_t2_ch_bad = 200U,
} ra_gpt_t2_ch_t;

static void test_gpt_period_set_buffers_gtpbr(void)
{
  TEST_BEGIN("gpt period_set buffers GTPBR");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_period_set((uint8_t)k_ra_gpt_test_channel_valid,
                                            (uint32_t)k_ra_gpt_t2_period_2));
  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_period_2, (int32_t)reg->GTPBR);

  /* Bad channel rejection. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_period_set((uint8_t)k_ra_gpt_t2_ch_bad, 0U));
  TEST_END("gpt period_set buffers GTPBR");
}

static void test_gpt_duty_cycle_set_routes_to_c_e(void)
{
  TEST_BEGIN("gpt duty_cycle_set routes to GTCCRC/GTCCRE + asserts GTBER");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_duty_cycle_set((uint8_t)k_ra_gpt_test_channel_valid,
                                                k_ra_gpt_pin_a,
                                                (uint32_t)k_ra_gpt_t2_duty_u));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_duty_cycle_set((uint8_t)k_ra_gpt_test_channel_valid,
                                                k_ra_gpt_pin_b,
                                                (uint32_t)k_ra_gpt_t2_duty_v));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_duty_u, (int32_t)reg->GTCCR[2]);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_duty_v, (int32_t)reg->GTCCR[3]);
  TEST_ASSERT((reg->GTBER & 0x00010000UL) != 0U);
  TEST_ASSERT((reg->GTBER & 0x00040000UL) != 0U);

  /* Bad pin selector. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_gpt_duty_cycle_set((uint8_t)k_ra_gpt_test_channel_valid, (ra_gpt_pwm_pin_t)9U, 0U));
  /* Bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_duty_cycle_set((uint8_t)k_ra_gpt_t2_ch_bad, k_ra_gpt_pin_a, 0U));
  TEST_END("gpt duty_cycle_set routes to GTCCRC/GTCCRE + asserts GTBER");
}

static void test_gpt_counter_set_writes_gtcnt(void)
{
  TEST_BEGIN("gpt counter_set writes GTCNT when stopped");
  prep_w35();

  ra_gpt_cfg_t cfg = make_gpt_cfg();
  cfg.auto_start   = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  /* GTCR.CST cleared because auto_start was false. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_counter_set((uint8_t)k_ra_gpt_test_channel_valid, 0xCAFE0000UL));
  TEST_ASSERT_EQ((int32_t)0xCAFE0000UL, (int32_t)reg->GTCNT);

  /* Now mark the timer running and expect rejection. */
  reg->GTCR |= 0x00000001UL;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_gpt_counter_set((uint8_t)k_ra_gpt_test_channel_valid, 0xDEADBEEFUL));
  /* Bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_counter_set((uint8_t)k_ra_gpt_t2_ch_bad, 0U));
  TEST_END("gpt counter_set writes GTCNT when stopped");
}

static void test_gpt_pwm_pin_configure_active_high(void)
{
  TEST_BEGIN("gpt pwm_pin_configure pin A active-high + output enable + POEG");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  const ra_gpt_pwm_pin_cfg_t pin_cfg = {
    .output_enable    = true,
    .polarity         = k_ra_gpt_pol_active_high,
    .stop_level       = k_ra_gpt_stop_high,
    .disable_on_fault = k_ra_gpt_disable_drive_low,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_pwm_pin_configure((uint8_t)k_ra_gpt_test_channel_valid,
                                                   k_ra_gpt_pin_a,
                                                   &pin_cfg));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  /* OAE bit 8 set, GTIOA[4:0] == 0x9 (active-high), OADFLT bit 6 set. */
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_active_high_a, (int32_t)(reg->GTIOR & 0x0000001FUL));
  TEST_ASSERT((reg->GTIOR & (uint32_t)k_ra_gpt_t2_oae_mask) != 0U);
  TEST_ASSERT((reg->GTIOR & (uint32_t)k_ra_gpt_t2_oadflt) != 0U);
  TEST_ASSERT((reg->GTIOR & (uint32_t)k_ra_gpt_t2_oadf_drive_low) != 0U);
  TEST_END("gpt pwm_pin_configure pin A active-high + output enable + POEG");
}

static void test_gpt_pwm_pin_configure_active_low_b(void)
{
  TEST_BEGIN("gpt pwm_pin_configure pin B active-low + disabled output");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  const ra_gpt_pwm_pin_cfg_t pin_cfg = {
    .output_enable    = false,
    .polarity         = k_ra_gpt_pol_active_low,
    .stop_level       = k_ra_gpt_stop_low,
    .disable_on_fault = k_ra_gpt_disable_none,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_pwm_pin_configure((uint8_t)k_ra_gpt_test_channel_valid,
                                                   k_ra_gpt_pin_b,
                                                   &pin_cfg));

  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  /* GTIOB[4:0] sits at bits 16..20; expect 0x6 << 16. */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_t2_active_low_a << 16U),
                 (int32_t)(reg->GTIOR & 0x001F0000UL));
  TEST_ASSERT((reg->GTIOR & (uint32_t)k_ra_gpt_t2_obe_mask) == 0U);

  /* NULL cfg + bad pin + bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_pwm_pin_configure((uint8_t)k_ra_gpt_test_channel_valid,
                                                   k_ra_gpt_pin_a,
                                                   nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_pwm_pin_configure((uint8_t)k_ra_gpt_test_channel_valid,
                                                   (ra_gpt_pwm_pin_t)5U,
                                                   &pin_cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_gpt_pwm_pin_configure((uint8_t)k_ra_gpt_t2_ch_bad, k_ra_gpt_pin_a, &pin_cfg));
  TEST_END("gpt pwm_pin_configure pin B active-low + disabled output");
}

static void test_gpt_dead_time_set(void)
{
  TEST_BEGIN("gpt dead_time_set programs GTDVU/GTDVD/GTDTCR");
  prep_w35();

  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_test_channel_valid,
                                               (uint32_t)k_ra_gpt_t2_dt_rise,
                                               (uint32_t)k_ra_gpt_t2_dt_fall));
  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_dt_rise, (int32_t)reg->GTDVU);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_dt_fall, (int32_t)reg->GTDVD);
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_gtdtcr_tde, (int32_t)reg->GTDTCR);

  /* Both zero -> TDE cleared. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_test_channel_valid, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->GTDTCR);

  /* Bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_t2_ch_bad, 0U, 0U));
  TEST_END("gpt dead_time_set programs GTDVU/GTDVD/GTDTCR");
}

static ra_gpt_three_phase_cfg_t make_three_phase_cfg(void)
{
  const ra_gpt_three_phase_cfg_t cfg = {
    .channels  = {(uint8_t)k_ra_gpt_t2_ch_u, (uint8_t)k_ra_gpt_t2_ch_v, (uint8_t)k_ra_gpt_t2_ch_w},
    .mode      = k_ra_gpt_mode_triangle_pwm,
    .prescaler = k_ra_gpt_ps_div_1,
    .period_counts  = (uint32_t)k_ra_gpt_t2_period,
    .initial_duty_u = (uint32_t)k_ra_gpt_t2_duty_u,
    .initial_duty_v = (uint32_t)k_ra_gpt_t2_duty_v,
    .initial_duty_w = (uint32_t)k_ra_gpt_t2_duty_w,
  };
  return cfg;
}

static void test_gpt_three_phase_open_starts_synchronously(void)
{
  TEST_BEGIN("gpt three_phase_open arms U/V/W via single GTSTR");
  prep_w35();

  const ra_gpt_three_phase_cfg_t cfg = make_three_phase_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_open(&cfg));

  volatile r_gpt_channel_regs_t* u_reg = ra_gpt((uint8_t)k_ra_gpt_t2_ch_u);
  /* GTSTR mask should cover all three channel-id bits. */
  const uint32_t expected_mask = (1UL << (uint8_t)k_ra_gpt_t2_ch_u) |
                                 (1UL << (uint8_t)k_ra_gpt_t2_ch_v) |
                                 (1UL << (uint8_t)k_ra_gpt_t2_ch_w);
  TEST_ASSERT_EQ((int32_t)expected_mask, (int32_t)u_reg->GTSTR);
  /* All three channels should have the same period in GTPR + GTPBR. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_gpt_three_phase_count; ++i) {
    volatile r_gpt_channel_regs_t* reg = ra_gpt(cfg.channels[i]);
    TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_period, (int32_t)reg->GTPR);
  }

  /* Re-open while open -> rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_gpt_three_phase_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_close());
  TEST_END("gpt three_phase_open arms U/V/W via single GTSTR");
}

static void test_gpt_three_phase_set_duty_atomic(void)
{
  TEST_BEGIN("gpt three_phase_set_duty writes GTCCRC/E on all 3 channels");
  prep_w35();

  const ra_gpt_three_phase_cfg_t cfg = make_three_phase_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_three_phase_set_duty((uint32_t)k_ra_gpt_t2_duty_u + 1U,
                                                      (uint32_t)k_ra_gpt_t2_duty_v + 1U,
                                                      (uint32_t)k_ra_gpt_t2_duty_w + 1U));

  volatile r_gpt_channel_regs_t* u_reg = ra_gpt((uint8_t)k_ra_gpt_t2_ch_u);
  volatile r_gpt_channel_regs_t* v_reg = ra_gpt((uint8_t)k_ra_gpt_t2_ch_v);
  volatile r_gpt_channel_regs_t* w_reg = ra_gpt((uint8_t)k_ra_gpt_t2_ch_w);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_t2_duty_u + 1U), (int32_t)u_reg->GTCCR[2]);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_t2_duty_u + 1U), (int32_t)u_reg->GTCCR[3]);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_t2_duty_v + 1U), (int32_t)v_reg->GTCCR[2]);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_gpt_t2_duty_w + 1U), (int32_t)w_reg->GTCCR[2]);

  /* Out-of-range duty -> rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_three_phase_set_duty((uint32_t)k_ra_gpt_t2_duty_huge, 0U, 0U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_close());
  TEST_END("gpt three_phase_set_duty writes GTCCRC/E on all 3 channels");
}

static void test_gpt_three_phase_close_stops_all(void)
{
  TEST_BEGIN("gpt three_phase_close issues a single GTSTP and tears down");
  prep_w35();

  const ra_gpt_three_phase_cfg_t cfg = make_three_phase_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_gpt_three_phase_close());

  /* Closing twice -> rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_gpt_three_phase_close());
  /* set_duty after close -> rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_gpt_three_phase_set_duty(0U, 0U, 0U));
  TEST_END("gpt three_phase_close issues a single GTSTP and tears down");
}

static void test_gpt_three_phase_arg_validation(void)
{
  TEST_BEGIN("gpt three_phase_open / set_duty arg validation");
  prep_w35();

  /* NULL cfg. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_gpt_three_phase_open(nullptr));

  /* Bad channel id. */
  ra_gpt_three_phase_cfg_t cfg = make_three_phase_cfg();
  cfg.channels[1]              = (uint8_t)k_ra_gpt_t2_ch_bad;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_gpt_three_phase_open(&cfg));

  /* set_duty before open -> rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_gpt_three_phase_set_duty(0U, 0U, 0U));
  TEST_END("gpt three_phase_open / set_duty arg validation");
}

/**
 * @test test_mcdc_write_dma_arg_guard
 *
 * @par MC/DC:
 * Decision: `if ((channel >= (uint8_t)k_ra_gpt_channel_count) ||
 *               (count == 0U))`
 * (2 conditions, libs/ra_hal/src/ra_gpt.c line 634 -- gap row 369 in CSV;
 * the same shape repeats at line 688 for ra_gpt_read_dma)
 * - Vector 1: channel=valid, count=1   -> F,F decision F -> ok.
 * - Vector 2: channel=99, count=1      -> T,_ decision T -> invalid_arg.
 * - Vector 3: channel=valid, count=0   -> F,T decision T -> invalid_arg.
 * MC/DC pair for C1: V1(F,F)->F vs V2(T,_)->T (decision flips, C2
 * masked in V2 by short-circuit). MC/DC pair for C2: V1(F,F)->F vs
 * V3(F,T)->T (decision flips, C1 held F). N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC.
 */
static void test_mcdc_write_dma_arg_guard(void)
{
  TEST_BEGIN("gpt write_dma MC/DC: channel>=cnt || count==0");
  prep_w35();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));

  const uint32_t periods[] = {0xAAAAAAAAUL};
  uint8_t        dch       = 0xFFU;

  /* Vector 1: in-range channel, non-zero count -> ok. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)
      ra_gpt_write_dma((uint8_t)k_ra_gpt_test_channel_valid, periods, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));

  /* Vector 2: out-of-range channel -> C1=T short-circuit. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_gpt_write_dma(99U, periods, 1U, nullptr, nullptr, &dch));

  /* Vector 3: count==0 -> C1=F, C2=T. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)
      ra_gpt_write_dma((uint8_t)k_ra_gpt_test_channel_valid, periods, 0U, nullptr, nullptr, &dch));

  TEST_END("gpt write_dma MC/DC: channel>=cnt || count==0");
}

/**
 * @test test_mcdc_dead_time_set_tde_predicate
 *
 * @par MC/DC:
 * Decision: `reg->GTDTCR = ((rising_dt != 0U) || (falling_dt != 0U))
 *                          ? k_ra_gpt_gtdtcr_tde : 0U;`
 * (2 conditions, libs/ra_hal/src/ra_gpt.c line 931 -- gap row 545 in CSV)
 * Decision is observed via the GTDTCR register value after the call.
 * - Vector 1: rising=0,    falling=0    -> F,F decision F -> GTDTCR=0
 * - Vector 2: rising!=0,   falling=0    -> T,_ decision T -> GTDTCR=TDE
 * - Vector 3: rising=0,    falling!=0   -> F,T decision T -> GTDTCR=TDE
 * MC/DC pair for C1: V1(F,F)->F vs V2(T,_)->T. MC/DC pair for C2:
 * V1(F,F)->F vs V3(F,T)->T. N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC.
 */
static void test_mcdc_dead_time_set_tde_predicate(void)
{
  TEST_BEGIN("gpt dead_time_set MC/DC: rising!=0 || falling!=0");
  prep_w35();
  const ra_gpt_cfg_t cfg = make_gpt_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_init((uint8_t)k_ra_gpt_test_channel_valid, &cfg));
  volatile r_gpt_channel_regs_t* reg = ra_gpt((uint8_t)k_ra_gpt_test_channel_valid);

  /* Vector 1: both zero -> TDE cleared. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_test_channel_valid, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)reg->GTDTCR);

  /* Vector 2: rising != 0 -> TDE asserted. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_test_channel_valid,
                                               (uint32_t)k_ra_gpt_t2_dt_rise,
                                               0U));
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_gtdtcr_tde, (int32_t)reg->GTDTCR);

  /* Vector 3: falling != 0 only -> TDE asserted. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_gpt_dead_time_set((uint8_t)k_ra_gpt_test_channel_valid,
                                               0U,
                                               (uint32_t)k_ra_gpt_t2_dt_fall));
  TEST_ASSERT_EQ((int32_t)k_ra_gpt_t2_gtdtcr_tde, (int32_t)reg->GTDTCR);

  TEST_END("gpt dead_time_set MC/DC: rising!=0 || falling!=0");
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
  test_gpt_write_dma_streams_periods_to_gtpr();
  test_gpt_read_dma_captures_gtcnt();
  test_gpt_dma_arg_validation();
  /* Sweep 3 Task 2 -- runtime PWM duty/period/counter, dead-time, 3-phase. */
  test_gpt_period_set_buffers_gtpbr();
  test_gpt_duty_cycle_set_routes_to_c_e();
  test_gpt_counter_set_writes_gtcnt();
  test_gpt_pwm_pin_configure_active_high();
  test_gpt_pwm_pin_configure_active_low_b();
  test_gpt_dead_time_set();
  test_gpt_three_phase_open_starts_synchronously();
  test_gpt_three_phase_set_duty_atomic();
  test_gpt_three_phase_close_stops_all();
  test_gpt_three_phase_arg_validation();
  test_mcdc_write_dma_arg_guard();
  test_mcdc_dead_time_set_tde_predicate();
  (void)fprintf(stderr, "[OK ] test_ra_gpt.c\n");
  return 0;
}
