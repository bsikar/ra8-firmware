/**
 * @file test_ra_adc.c
 * @brief Unit tests for adc.c (ADC_B polling driver)
 *
 * @details
 * The Wave 11 audit rewrote the driver to use the real RA8D2 ADC_B
 * register layout (ADCLKENR / ADMDR / ADCHCR[24] / ADDR[23]).
 * These tests verify the driver touches the right registers and
 * that the simulator-backed mmap window responds correctly.
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

static uint8_t s_alarm_ch;

static void sigalarm_handler(int sig)
{
  (void)sig;
  /* Mimic hardware: clear CVEN on the in-flight channel so the driver's
   * busy-wait can exit. */
  volatile uint32_t* chcr = ra_adc_b_adchcr(s_alarm_ch);
  if (chcr != nullptr) {
    *chcr = *chcr & ~(uint32_t)k_ra_adchcr_mask_cven;
  }
}

static void arm_cven_clear_alarm(uint8_t ch)
{
  s_alarm_ch = ch;
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = 100; /* 100 us -- well inside the driver poll. */
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

typedef enum : uint8_t {
  k_ra_adc_test_ch_zero  = 0U,
  k_ra_adc_test_ch_valid = 5U,
  k_ra_adc_test_ch_max   = 22U, /**< Highest ADDR[] slot (23 results). */
  k_ra_adc_test_ch_oor   = 24U,
  k_ra_adc_test_ch_huge  = 200U,
} ra_adc_test_ch_t;

typedef enum : uint16_t {
  k_ra_adc_test_result_a = 0x1234U,
  k_ra_adc_test_result_b = 0x0BEEU,
} ra_adc_test_val_t;

static void test_init_happy(void)
{
  TEST_BEGIN("adc init happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_init());
  TEST_ASSERT((*ra_adc_b_adclkenr() & (uint32_t)k_ra_adclkenr_mask_clken) != 0U);
  /* Default ADMDR is zero (single-shot, software trigger). */
  TEST_ASSERT_EQ((int)0, (int)*ra_adc_b_admdr());
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

/**
 * @brief Drive a read with the SIGALRM sim helper clearing CVEN
 *        mid-poll to mimic hardware auto-clear.
 */
static void test_read_channel_completes_via_alarm(void)
{
  TEST_BEGIN("adc read_channel: CVEN auto-clear (sim alarm)");
  ra_sim_mmap_reset();

  *ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_zero) = (uint32_t)k_ra_adc_test_result_a;

  arm_cven_clear_alarm((uint8_t)k_ra_adc_test_ch_zero);
  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_zero, &raw);
  disarm_alarm();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_adc_test_result_a, (int)raw);
  TEST_END("adc read_channel: CVEN auto-clear (sim alarm)");
}

/**
 * @brief Without the alarm the driver bounded-polls then reports
 *        k_ra_err_hw_timeout.
 */
static void test_read_channel_timeout(void)
{
  TEST_BEGIN("adc read_channel: poll timeout");
  ra_sim_mmap_reset();

  uint16_t       raw = 0xBEEFU;
  const ra_err_t err = ra_adc_read_channel((uint8_t)k_ra_adc_test_ch_valid, &raw);
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_ASSERT_EQ(0, (int)raw);
  TEST_END("adc read_channel: poll timeout");
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
  TEST_BEGIN("adc init configured: 14b right-aligned");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));

  /* Every ADCHCR slot should now have RESSEL programmed to 14-bit. */
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_adc_b_max_channels; ++ch) {
    volatile uint32_t* chcr = ra_adc_b_adchcr(ch);
    const uint32_t     ressel =
      (*chcr & (uint32_t)k_ra_adchcr_mask_ressel) >> (uint32_t)k_ra_adchcr_bit_ressel0;
    TEST_ASSERT_EQ((int32_t)k_ra_adc_res_14bit, (int32_t)ressel);
  }
  TEST_END("adc init configured: 14b right-aligned");
}

static void test_init_configured_null(void)
{
  TEST_BEGIN("adc init configured null");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_init_configured(nullptr));
  TEST_END("adc init configured null");
}

static void test_init_configured_scan_and_ext(void)
{
  TEST_BEGIN("adc init configured scan/ext");
  ra_sim_mmap_reset();

  ra_adc_cfg_t cfg = make_cfg();
  cfg.scan_mode    = true;
  cfg.trigger      = k_ra_adc_trig_elc;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  /* ADMDR bit 0 (ADTRGMD) set for external/ELC trigger, bit 1 (ADSCANMD)
   * set for scan mode. */
  const uint32_t admdr = *ra_adc_b_admdr();
  TEST_ASSERT((admdr & (uint32_t)k_ra_admdr_mask_adtrgmd) != 0U);
  TEST_ASSERT((admdr & (uint32_t)k_ra_admdr_mask_adscanmd) != 0U);
  TEST_END("adc init configured scan/ext");
}

static void test_deinit(void)
{
  TEST_BEGIN("adc deinit");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_admdr());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_adclkenr());
  TEST_END("adc deinit");
}

static void test_set_resolution(void)
{
  TEST_BEGIN("adc set_resolution applies to all channels");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_set_resolution(k_ra_adc_res_12bit));

  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_adc_b_max_channels; ++ch) {
    volatile uint32_t* chcr = ra_adc_b_adchcr(ch);
    const uint32_t     ressel =
      (*chcr & (uint32_t)k_ra_adchcr_mask_ressel) >> (uint32_t)k_ra_adchcr_bit_ressel0;
    TEST_ASSERT_EQ((int32_t)k_ra_adc_res_12bit, (int32_t)ressel);
  }
  TEST_END("adc set_resolution applies to all channels");
}

static void test_set_resolution_bad(void)
{
  TEST_BEGIN("adc set_resolution bad");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_adc_set_resolution((ra_adc_resolution_t)9U));
  TEST_END("adc set_resolution bad");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("adc status read + clear");
  ra_sim_mmap_reset();

  *ra_adc_b_admdr()   = (uint32_t)k_ra_admdr_mask_adbusy;
  *ra_adc_b_adintcr() = 1UL;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT((mask & (uint16_t)k_ra_adc_status_busy) != 0U);
  TEST_ASSERT((mask & (uint16_t)k_ra_adc_status_ie) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_clear_status());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT((mask & (uint16_t)k_ra_adc_status_busy) == 0U);
  TEST_END("adc status read + clear");
}

static void test_status_null(void)
{
  TEST_BEGIN("adc status null");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_get_status(nullptr));
  TEST_END("adc status null");
}

static uint32_t s_adc_cb_count;
static uint16_t s_adc_cb_last_result;
static void*    s_adc_cb_last_ctx;

static void stub_adc_cb(void* ctx, uint16_t result)
{
  ++s_adc_cb_count;
  s_adc_cb_last_result = result;
  s_adc_cb_last_ctx    = ctx;
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("adc attach + dispatch");
  ra_sim_mmap_reset();
  s_adc_cb_count       = 0U;
  s_adc_cb_last_result = 0U;
  s_adc_cb_last_ctx    = nullptr;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_attach_handler(stub_adc_cb, (void*)(uintptr_t)0xA5A5U));

  volatile uint32_t* addr = ra_adc_b_addr((uint8_t)k_ra_adc_test_ch_valid);
  *addr                   = (uint32_t)k_ra_adc_test_result_a;

  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_adc_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_result_a, (int32_t)s_adc_cb_last_result);
  TEST_END("adc attach + dispatch");
}

static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("adc dispatch no handler");
  ra_sim_mmap_reset();
  s_adc_cb_count = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_attach_handler(nullptr, nullptr));
  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_valid);
  ra_adc_dispatch_cnv_end((uint8_t)k_ra_adc_test_ch_oor);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_adc_cb_count);
  TEST_END("adc dispatch no handler");
}

static void test_power_transition(void)
{
  TEST_BEGIN("adc power transition");
  ra_sim_mmap_reset();

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
  test_read_channel_completes_via_alarm();
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
