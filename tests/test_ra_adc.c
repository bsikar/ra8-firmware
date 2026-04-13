/**
 * @file test_ra_adc.c
 * @brief Unit tests for adc.c (ADC_B polling driver)
 *
 * @details
 * The driver busy-waits for the ADCSR.ADST bit to clear, which on real
 * hardware is cleared automatically when a conversion finishes. On the
 * host MMIO mock the same memory is ordinary RAM, so we bridge the gap
 * with an interval timer + SIGALRM handler that zeros ADCSR during the
 * busy-wait, letting the success path return.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#include "ra8d2_adc_b_regs.h"
#include "ra_adc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_adc_test_ch_t
 * @brief Channel numbers used by ADC tests.
 */
typedef enum : uint8_t {
  k_ra_adc_test_ch_zero  = 0U,
  k_ra_adc_test_ch_valid = 5U,
  k_ra_adc_test_ch_max   = 47U,
  k_ra_adc_test_ch_oor   = 48U,
  k_ra_adc_test_ch_huge  = 200U,
} ra_adc_test_ch_t;

/**
 * @enum ra_adc_test_val_t
 * @brief Sample values used for ADC tests.
 */
typedef enum : uint16_t {
  k_ra_adc_test_result_a        = 0x1234U,
  k_ra_adc_test_result_b        = 0x0BEEU,
  k_ra_adc_test_adcer_expected  = (uint16_t)((1U << 15U) | (2U << 1U)),
  k_ra_adc_test_adcsr_busy_mask = (uint16_t)(1U << 15U),
} ra_adc_test_val_t;

static void sigalarm_handler(int sig)
{
  (void)sig;
  /* Mimic hardware: clear ADST when the conversion "finishes" so the
   * busy-wait in ra_adc_read_channel can exit. */
  *ra_adc_b_adcsr() = 0U;
}

/**
 * @enum ra_adc_test_timer_t
 * @brief Interval-timer delays used to simulate ADC conversion completion.
 */
typedef enum : uint32_t {
  k_ra_adc_test_timer_usec = 100U, /**< 100us -- well inside the busy-wait. */
} ra_adc_test_timer_t;

/**
 * @brief Arm a one-shot interval timer that clears ADCSR.
 */
static void arm_adst_clear_alarm(void)
{
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = (long)k_ra_adc_test_timer_usec;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = 0;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void test_init_happy(void)
{
  TEST_BEGIN("adc init happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_init());
  TEST_ASSERT_EQ(0, (int)*ra_adc_b_adcsr());
  TEST_ASSERT_EQ((int)k_ra_adc_test_adcer_expected, (int)*ra_adc_b_adcer());
  TEST_END("adc init happy");
}

static void test_read_channel_null_out(void)
{
  TEST_BEGIN("adc read_channel null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_zero, nullptr));
  TEST_END("adc read_channel null out");
}

static void test_read_channel_out_of_range(void)
{
  TEST_BEGIN("adc read_channel out of range");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_oor, &raw));
  TEST_END("adc read_channel out of range");
}

static void test_read_channel_huge(void)
{
  TEST_BEGIN("adc read_channel huge ch");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range,
                 (int)ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_huge, &raw));
  TEST_END("adc read_channel huge ch");
}

static void test_read_channel_happy_ch0(void)
{
  TEST_BEGIN("adc read_channel happy ch0");
  ra_sim_mmap_reset();

  /* Pre-seed the result register so the success path can verify it. */
  *ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_zero) = (uint16_t)k_ra_adc_test_result_a;

  arm_adst_clear_alarm();
  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_zero, &raw);
  disarm_alarm();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_adc_test_result_a, (int)raw);
  TEST_END("adc read_channel happy ch0");
}

static void test_read_channel_happy_mid(void)
{
  TEST_BEGIN("adc read_channel happy mid");
  ra_sim_mmap_reset();

  *ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_valid) = (uint16_t)k_ra_adc_test_result_b;

  arm_adst_clear_alarm();
  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_valid, &raw);
  disarm_alarm();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_adc_test_result_b, (int)raw);
  TEST_END("adc read_channel happy mid");
}

static void test_read_channel_happy_max(void)
{
  TEST_BEGIN("adc read_channel happy max");
  ra_sim_mmap_reset();

  *ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_max) = (uint16_t)k_ra_adc_test_result_a;

  arm_adst_clear_alarm();
  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_max, &raw);
  disarm_alarm();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("adc read_channel happy max");
}

static void test_read_channel_timeout(void)
{
  TEST_BEGIN("adc read_channel timeout");
  ra_sim_mmap_reset();

  /* NO alarm armed: the busy-wait never sees ADST clear so the loop
   * runs to completion and returns k_ra_err_hw_timeout. */
  uint16_t       raw = 0xBEEFU;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_valid, &raw);
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_ASSERT_EQ(0, (int)raw);
  TEST_END("adc read_channel timeout");
}

/* ---------------------------------------------------------------------------
 * Wave 4.1 -- full build-out
 * ---------------------------------------------------------------------------
 */

static uint32_t s_adc_cb_count;
static uint16_t s_adc_cb_last_result;
static void*    s_adc_cb_last_ctx;

static void stub_adc_cb(void* ctx, uint16_t result)
{
  ++s_adc_cb_count;
  s_adc_cb_last_result = result;
  s_adc_cb_last_ctx    = ctx;
}

static void prep_w41(void)
{
  ra_sim_mmap_reset();
  s_adc_cb_count       = 0U;
  s_adc_cb_last_result = 0U;
  s_adc_cb_last_ctx    = nullptr;
}

static ra_adc_cfg_t make_cfg(void)
{
  const ra_adc_cfg_t cfg = {
    .resolution    = k_ra_adc_res_14bit,
    .trigger       = k_ra_adc_trig_software,
    .right_aligned = true,
    .scan_mode     = false,
  };
  return cfg;
}

static void test_init_configured(void)
{
  TEST_BEGIN("adc init configured");
  prep_w41();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));

  const uint16_t adcer_expect =
    (uint16_t)((uint16_t)k_ra_adc_res_14bit << 1U) | (uint16_t)(1U << 15U);
  TEST_ASSERT_EQ((int32_t)adcer_expect, (int32_t)*ra_adc_b_adcer());
  TEST_END("adc init configured");
}

static void test_init_configured_null(void)
{
  TEST_BEGIN("adc init configured null");
  prep_w41();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_init_configured(nullptr));
  TEST_END("adc init configured null");
}

static void test_init_configured_scan_and_ext(void)
{
  TEST_BEGIN("adc init configured scan/ext");
  prep_w41();

  ra_adc_cfg_t cfg = make_cfg();
  cfg.scan_mode    = true;
  cfg.trigger      = k_ra_adc_trig_elc;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT((*ra_adc_b_adcsr() & (uint16_t)(1U << 13U)) != 0U); /* ADCS scan */
  TEST_ASSERT((*ra_adc_b_adcsr() & (uint16_t)(1U << 9U)) != 0U);  /* TRGE */
  TEST_ASSERT((*ra_adc_b_adcsr() & (uint16_t)(1U << 10U)) != 0U); /* EXCE */
  TEST_END("adc init configured scan/ext");
}

static void test_deinit(void)
{
  TEST_BEGIN("adc deinit");
  prep_w41();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_adcsr());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_adcer());
  TEST_END("adc deinit");
}

static void test_set_resolution(void)
{
  TEST_BEGIN("adc set_resolution");
  prep_w41();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_set_resolution(k_ra_adc_res_12bit));

  const uint16_t adprc_mask = (uint16_t)(3U << 1U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)((uint16_t)*ra_adc_b_adcer() & adprc_mask));
  TEST_END("adc set_resolution");
}

static void test_set_resolution_bad(void)
{
  TEST_BEGIN("adc set_resolution bad");
  prep_w41();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_adc_set_resolution((ra_adc_resolution_t)9U));
  TEST_END("adc set_resolution bad");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("adc status read + clear");
  prep_w41();

  *ra_adc_b_adcsr() = (uint16_t)((uint16_t)k_ra_adc_status_busy | (uint16_t)k_ra_adc_status_ie);

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)((uint16_t)k_ra_adc_status_busy | (uint16_t)k_ra_adc_status_ie),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_clear_status());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)k_ra_adc_status_ie, (int32_t)mask);
  TEST_END("adc status read + clear");
}

static void test_status_null(void)
{
  TEST_BEGIN("adc status null");
  prep_w41();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_get_status(nullptr));
  TEST_END("adc status null");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("adc attach + dispatch");
  prep_w41();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_attach_handler(stub_adc_cb, (void*)(uintptr_t)0xA5A5U));

  volatile uint16_t* addr = ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_valid);
  *addr                   = (uint16_t)k_ra_adc_test_result_a;

  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_adc_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_result_a, (int32_t)s_adc_cb_last_result);
  TEST_END("adc attach + dispatch");
}

static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("adc dispatch no handler");
  prep_w41();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_attach_handler(nullptr, nullptr));
  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_valid);
  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_oor);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_adc_cb_count);
  TEST_END("adc dispatch no handler");
}

static void test_power_transition(void)
{
  TEST_BEGIN("adc power transition");
  prep_w41();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_exit_stop());
  TEST_END("adc power transition");
}

int32_t main(void)
{
  test_init_happy();
  test_read_channel_null_out();
  test_read_channel_out_of_range();
  test_read_channel_huge();
  test_read_channel_happy_ch0();
  test_read_channel_happy_mid();
  test_read_channel_happy_max();
  test_read_channel_timeout();
  test_init_configured();
  test_init_configured_null();
  test_init_configured_scan_and_ext();
  test_deinit();
  test_set_resolution();
  test_set_resolution_bad();
  test_status_read_and_clear();
  test_status_null();
  test_attach_and_dispatch();
  test_dispatch_no_handler();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_adc.c\n");
  return 0;
}
