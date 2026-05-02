/**
 * @file test_ra_adc.c
 * @brief Unit tests for adc.c (ADC_B polling driver, FSP-verified layout)
 *
 * @details
 * The ADC_B driver was re-derived against FSP ``R_ADC_B0_Type``
 * (R7KA8D2KF_core0.h lines 20460-24938) which corrected the
 * register layout away from the imaginary CVEN / RESSEL / ADTRGMD /
 * ADSCANMD / ADBUSY bit map. These tests verify:
 *   - ADCLKENR is set after init
 *   - ADMDR encodes ADMD0 unit mode (resolution / scan)
 *   - ADSGER enables the default scan group (group 0)
 *   - ``ra_adc_read_channel`` programmes ADCHCRn and kicks ADSTR[0]
 *   - The driver polls ADSR.ADACT0 and reads ADDR[ch].DATA on success
 *   - Out-of-range channels and the ADCHCR-valid-but-ADDR-OOB
 *     boundary (channel 23) return ``k_ra_err_out_of_range``
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

/* ---------------------------------------------------------------------------
 * Sim helpers
 * --------------------------------------------------------------------------- */

static void sigalarm_handler(int sig)
{
  (void)sig;
  /* Mimic hardware: clear ADSR.ADACT0 so the driver's busy-wait can exit. */
  *ra_adc_b_adsr() = *ra_adc_b_adsr() & ~k_ra_adsr_mask_adact0;
}

static void arm_adact_clear_alarm(void)
{
  /* Pre-set ADACT0 high so the loop spins at least once. */
  *ra_adc_b_adsr() = k_ra_adsr_mask_adact0;
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

/* ---------------------------------------------------------------------------
 * Test fixtures
 * --------------------------------------------------------------------------- */

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

typedef enum : uint32_t {
  k_ra_adc_test_default_group_mask = 0x00000001UL, /**< ADSGER bit for group 0. */
  k_ra_adc_test_admd0_one_cycle    = 0x00000001UL, /**< Expected ADMDR.ADMD0 value. */
  k_ra_adc_test_admd0_continuous   = 0x00000002UL,
} ra_adc_test_const_t;

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------- */

static void test_init_happy(void)
{
  TEST_BEGIN("adc init happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_init());
  TEST_ASSERT((*ra_adc_b_adclkenr() & k_ra_adclkenr_mask_clken) != 0U);
  /* ADMDR carries the one-cycle mode for ADC0. */
  TEST_ASSERT_EQ((int)k_ra_adc_test_admd0_one_cycle,
                 (int)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  /* ADSGER enables the default scan group. */
  TEST_ASSERT_EQ((int)k_ra_adc_test_default_group_mask,
                 (int)(*ra_adc_b_adsger() & k_ra_adsger_mask_sgren));
  TEST_END("adc init happy");
}

static void test_read_channel_null_out(void)
{
  TEST_BEGIN("adc read_channel null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_adc_read_channel(k_ra_adc_test_ch_zero, nullptr));
  TEST_END("adc read_channel null out");
}

static void test_read_channel_out_of_range(void)
{
  TEST_BEGIN("adc read_channel out of range");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range, (int)ra_adc_read_channel(k_ra_adc_test_ch_oor, &raw));
  TEST_END("adc read_channel out of range");
}

static void test_read_channel_huge(void)
{
  TEST_BEGIN("adc read_channel huge ch");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range, (int)ra_adc_read_channel(k_ra_adc_test_ch_huge, &raw));
  TEST_END("adc read_channel huge ch");
}

/**
 * @brief Hits the adc.c branch where ADCHCR[ch] is valid but ADDR[ch]
 *        is out-of-range (channel 23: FSP has 24 ADCHCR slots but
 *        only 23 ADDR result slots).
 */
static void test_read_channel_hcr_but_no_result(void)
{
  TEST_BEGIN("adc read_channel: ch 23 (hcr valid, addr oob)");
  ra_sim_mmap_reset();
  uint16_t raw = 0U;
  /* k_ra_adc_b_max_channels = 24, k_ra_adc_b_result_regs = 23. */
  TEST_ASSERT_EQ((int)k_ra_err_out_of_range, (int)ra_adc_read_channel(23U, &raw));
  TEST_END("adc read_channel: ch 23 (hcr valid, addr oob)");
}

/**
 * @brief Drive a read with the SIGALRM sim helper clearing ADACT0
 *        mid-poll to mimic hardware auto-clear.
 */
static void test_read_channel_completes_via_alarm(void)
{
  TEST_BEGIN("adc read_channel: ADACT0 auto-clear (sim alarm)");
  ra_sim_mmap_reset();

  *ra_adc_b_addr(k_ra_adc_test_ch_zero) = (uint32_t)k_ra_adc_test_result_a;

  arm_adact_clear_alarm();
  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel(k_ra_adc_test_ch_zero, &raw);
  disarm_alarm();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_adc_test_result_a, (int)raw);
  /* ADCHCRn[0] should now have CNVCS == 0 (channel 0 mapped onto itself). */
  const uint32_t chcr = *ra_adc_b_adchcr(k_ra_adc_test_ch_zero);
  TEST_ASSERT_EQ(0, (int)((chcr & k_ra_adchcr_mask_cnvcs) >> (uint32_t)k_ra_adchcr_bit_cnvcs));
  TEST_END("adc read_channel: ADACT0 auto-clear (sim alarm)");
}

/**
 * @brief Without the alarm the driver bounded-polls then reports
 *        k_ra_err_hw_timeout.
 */
static void test_read_channel_timeout(void)
{
  TEST_BEGIN("adc read_channel: poll timeout");
  ra_sim_mmap_reset();

  /* Force ADACT0 stuck high so the busy poll never exits. */
  *ra_adc_b_adsr() = k_ra_adsr_mask_adact0;

  uint16_t       raw = 0xBEEFU;
  const ra_err_t err = ra_adc_read_channel(k_ra_adc_test_ch_valid, &raw);
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

  /* ADMDR.ADMD0 must encode one-cycle mode (single-shot). */
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_admd0_one_cycle,
                 (int32_t)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  /* All 24 ADCHCR slots should have been zeroed. */
  for (uint8_t ch = 0U; ch < k_ra_adc_b_max_channels; ++ch) {
    TEST_ASSERT_EQ(0, (int32_t)*ra_adc_b_adchcr(ch));
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

static void test_init_configured_scan(void)
{
  TEST_BEGIN("adc init configured: scan mode");
  ra_sim_mmap_reset();

  ra_adc_cfg_t cfg = make_cfg();
  cfg.scan_mode    = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  /* ADMDR.ADMD0 must encode the continuous-scan mode code. */
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_admd0_continuous,
                 (int32_t)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  TEST_END("adc init configured: scan mode");
}

static void test_deinit(void)
{
  TEST_BEGIN("adc deinit");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_admdr());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_adsger());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_adc_b_adclkenr());
  TEST_END("adc deinit");
}

static void test_set_resolution(void)
{
  TEST_BEGIN("adc set_resolution updates ADMD0");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_set_resolution(k_ra_adc_res_12bit));

  /* ADMD0 should still hold the one-cycle code (every public resolution
   * selector currently maps to one-cycle). */
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_admd0_one_cycle,
                 (int32_t)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  TEST_END("adc set_resolution updates ADMD0");
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

  /* Pre-set ADSR.ADACT0 (busy) and ADINTCR.ADIE0 (IE) to assert. */
  *ra_adc_b_adsr()    = k_ra_adsr_mask_adact0;
  *ra_adc_b_adintcr() = 1UL;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra_adc_status_busy) != 0U);
  TEST_ASSERT((mask & k_ra_adc_status_ie) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_clear_status());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra_adc_status_busy) == 0U);
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

  volatile uint32_t* addr = ra_adc_b_addr(k_ra_adc_test_ch_valid);
  *addr                   = (uint32_t)k_ra_adc_test_result_a;

  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_valid);
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
  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_valid);
  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_oor);
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

/* ---------------------------------------------------------------------------
 * Sweep 3 Task 1: scan-group + ELC trigger + window comparator + oversample
 * --------------------------------------------------------------------------- */

typedef enum : uint8_t {
  k_ra_adc_test_group_one  = 1U,
  k_ra_adc_test_group_oor  = 9U,
  k_ra_adc_test_group_huge = 200U,
  k_ra_adc_test_cmp_table  = 3U,
  k_ra_adc_test_cmp_oor    = 8U,
} ra_adc_test_group_t;

typedef enum : uint16_t {
  k_ra_adc_test_window_low  = 0x0100U,
  k_ra_adc_test_window_high = 0x0FFFU,
} ra_adc_test_window_t;

typedef enum : uint32_t {
  k_ra_adc_test_admd0_disabled = 0x00000000UL,
} ra_adc_test_admd_t;

static ra_adc_scan_group_cfg_t make_scan_cfg(void)
{
  ra_adc_scan_group_cfg_t cfg = {};
  cfg.num_channels            = 3U;
  cfg.channels[0]             = 4U;
  cfg.channels[1]             = 5U;
  cfg.channels[2]             = 6U;
  cfg.trigger                 = k_ra_adc_trig_src_software;
  cfg.priority                = k_ra_adc_priority_high;
  return cfg;
}

static void test_configure_scan_group_happy(void)
{
  TEST_BEGIN("adc scan-group: configure happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));

  /* ADSGER must have group-1 bit set in addition to the default group-0 bit. */
  const uint32_t expected = (1UL << k_ra_adc_test_group_one) | 1UL;
  TEST_ASSERT((*ra_adc_b_adsger() & expected) == expected);

  /* Each listed ADCHCR slot must encode SGSEL == 1 and CNVCS == ch. */
  for (uint8_t i = 0U; i < cfg.num_channels; ++i) {
    const uint32_t chcr = *ra_adc_b_adchcr(cfg.channels[i]);
    const uint32_t sg   = (chcr & k_ra_adchcr_mask_sgsel) >> (uint32_t)k_ra_adchcr_bit_sgsel;
    const uint32_t cs   = (chcr & k_ra_adchcr_mask_cnvcs) >> (uint32_t)k_ra_adchcr_bit_cnvcs;
    TEST_ASSERT_EQ((int32_t)k_ra_adc_test_group_one, (int32_t)sg);
    TEST_ASSERT_EQ((int32_t)cfg.channels[i], (int32_t)cs);
  }
  /* Software trigger -> ADTRGENR group-1 bit is clear. */
  TEST_ASSERT((*ra_adc_b_adtrgenr() & (1UL << k_ra_adc_test_group_one)) == 0U);
  TEST_END("adc scan-group: configure happy");
}

static void test_configure_scan_group_elc_trigger(void)
{
  TEST_BEGIN("adc scan-group: ELC trigger arms ADTRGENR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  cfg.trigger                 = k_ra_adc_trig_src_elc;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));
  TEST_ASSERT((*ra_adc_b_adtrgenr() & (1UL << k_ra_adc_test_group_one)) != 0U);
  TEST_END("adc scan-group: ELC trigger arms ADTRGENR");
}

static void test_configure_scan_group_rejects_null(void)
{
  TEST_BEGIN("adc scan-group: null cfg");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_configure_scan_group(0U, nullptr));
  TEST_END("adc scan-group: null cfg");
}

static void test_configure_scan_group_rejects_oor(void)
{
  TEST_BEGIN("adc scan-group: out-of-range group/channels");
  ra_sim_mmap_reset();
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_oor, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_huge, &cfg));

  ra_adc_scan_group_cfg_t bad_cfg = make_scan_cfg();
  bad_cfg.num_channels            = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(0U, &bad_cfg));

  bad_cfg             = make_scan_cfg();
  bad_cfg.channels[0] = (uint8_t)k_ra_adc_b_max_channels; /* OOR. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(0U, &bad_cfg));
  TEST_END("adc scan-group: out-of-range group/channels");
}

static void test_start_stop_group(void)
{
  TEST_BEGIN("adc scan-group: start/stop sets ADSTR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_start_group(k_ra_adc_test_group_one));
  TEST_ASSERT((*ra_adc_b_adstr(k_ra_adc_test_group_one) & k_ra_adstr_mask_adst) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_stop_group(k_ra_adc_test_group_one));
  TEST_ASSERT((*ra_adc_b_adstr(k_ra_adc_test_group_one) & k_ra_adstr_mask_adst) == 0U);
  TEST_ASSERT((*ra_adc_b_adstopr() & (k_ra_adstopr_mask_adstop0 | k_ra_adstopr_mask_adstop1)) !=
              0U);

  /* Out-of-range groups are rejected on both entry points. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_start_group(k_ra_adc_test_group_oor));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_stop_group(k_ra_adc_test_group_huge));
  TEST_END("adc scan-group: start/stop sets ADSTR");
}

static void test_read_group_results_happy(void)
{
  TEST_BEGIN("adc scan-group: read multi-channel results");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));

  /* Pre-load distinct ADDR slots so we can verify ordering. */
  *ra_adc_b_addr(cfg.channels[0]) = 0x0111U;
  *ra_adc_b_addr(cfg.channels[1]) = 0x0222U;
  *ra_adc_b_addr(cfg.channels[2]) = 0x0333U;

  uint16_t buf[8] = {};
  uint8_t  count  = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_read_group_results(k_ra_adc_test_group_one, buf, &count));
  TEST_ASSERT_EQ((int32_t)cfg.num_channels, (int32_t)count);
  TEST_ASSERT_EQ((int32_t)0x0111, (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)0x0222, (int32_t)buf[1]);
  TEST_ASSERT_EQ((int32_t)0x0333, (int32_t)buf[2]);
  TEST_END("adc scan-group: read multi-channel results");
}

static void test_read_group_results_rejects(void)
{
  TEST_BEGIN("adc scan-group: read rejects null/oor/unconfigured");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  uint16_t buf   = 0U;
  uint8_t  count = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_adc_read_group_results(0U, nullptr, &count));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_adc_read_group_results(0U, &buf, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_read_group_results(k_ra_adc_test_group_oor, &buf, &count));
  /* Group 1 is unconfigured at this point. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_read_group_results(k_ra_adc_test_group_one, &buf, &count));
  TEST_END("adc scan-group: read rejects null/oor/unconfigured");
}

static void test_continuous_scan(void)
{
  TEST_BEGIN("adc scan-group: continuous-scan toggles ADMDR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_set_continuous_scan(k_ra_adc_test_group_one, true));
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_admd0_continuous,
                 (int32_t)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_set_continuous_scan(k_ra_adc_test_group_one, false));
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_admd0_one_cycle,
                 (int32_t)(*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_set_continuous_scan(k_ra_adc_test_group_oor, true));
  TEST_END("adc scan-group: continuous-scan toggles ADMDR");
}

static void test_compare_window_happy(void)
{
  TEST_BEGIN("adc compare-window: programs ADCMPTBR/MDR/ENR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_set_compare_window(k_ra_adc_test_cmp_table,
                                                    k_ra_adc_test_window_low,
                                                    k_ra_adc_test_window_high));

  const uint32_t tbl  = *ra_adc_b_adcmptbr(k_ra_adc_test_cmp_table);
  const uint32_t low  = (tbl & k_ra_adcmptbr_mask_low) >> (uint32_t)k_ra_adcmptbr_bit_low;
  const uint32_t high = (tbl & k_ra_adcmptbr_mask_high) >> (uint32_t)k_ra_adcmptbr_bit_high;
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_window_low, (int32_t)low);
  TEST_ASSERT_EQ((int32_t)k_ra_adc_test_window_high, (int32_t)high);
  TEST_ASSERT((*ra_adc_b_adcmpenr() & (1UL << k_ra_adc_test_cmp_table)) != 0U);

  /* CMPMD field for table 3 lives in ADCMPMDR0 at bit (3 * 8) = 24. */
  const uint32_t mdr0  = *ra_adc_b_adcmpmdr(0U);
  const uint32_t shift = (uint32_t)k_ra_adc_test_cmp_table * (uint32_t)k_ra_adcmpmd_field_step;
  TEST_ASSERT_EQ((int32_t)k_ra_adcmpmd_inside, (int32_t)((mdr0 >> shift) & 0x3UL));

  /* Per-channel ADDOPCRB CMPTBLEm bit is set for our table. */
  const uint32_t opcrb = *ra_adc_b_addopcrb(k_ra_adc_test_cmp_table);
  TEST_ASSERT((opcrb & k_ra_addopcrb_mask_cmptblem) != 0U);
  TEST_END("adc compare-window: programs ADCMPTBR/MDR/ENR");
}

static void test_compare_window_rejects(void)
{
  TEST_BEGIN("adc compare-window: rejects high<low and oor table");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_adc_set_compare_window(0U, k_ra_adc_test_window_high, k_ra_adc_test_window_low));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_set_compare_window(k_ra_adc_test_cmp_oor,
                                                    k_ra_adc_test_window_low,
                                                    k_ra_adc_test_window_high));
  TEST_END("adc compare-window: rejects high<low and oor table");
}

static void test_oversampling_encoding(void)
{
  TEST_BEGIN("adc oversampling: AVEMD/ADC encoding");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_set_oversampling(k_ra_adc_test_ch_valid, k_ra_adc_oversample_16x));
  const uint32_t opcrb = *ra_adc_b_addopcrb(k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_avemd_average,
    (int32_t)((opcrb & k_ra_addopcrb_mask_avemd) >> (uint32_t)k_ra_addopcrb_bit_avemd));
  TEST_ASSERT_EQ((int32_t)k_ra_adc_avg_16x,
                 (int32_t)((opcrb & k_ra_addopcrb_mask_adc) >> (uint32_t)k_ra_addopcrb_bit_adc));

  /* Switching back to off must clear AVEMD. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_set_oversampling(k_ra_adc_test_ch_valid, k_ra_adc_oversample_off));
  const uint32_t opcrb_after = *ra_adc_b_addopcrb(k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ((int32_t)k_ra_avemd_off, (int32_t)(opcrb_after & k_ra_addopcrb_mask_avemd));
  TEST_END("adc oversampling: AVEMD/ADC encoding");
}

static void test_oversampling_rejects(void)
{
  TEST_BEGIN("adc oversampling: rejects bad mode and oor channel");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_adc_set_oversampling(0U, (ra_adc_oversample_t)0xFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_set_oversampling(k_ra_adc_test_ch_oor, k_ra_adc_oversample_4x));
  TEST_END("adc oversampling: rejects bad mode and oor channel");
}

/**
 * @test test_mcdc_adc
 *
 * @par MC/DC:
 * Two 2-condition decisions inside ``adc.c``.  Each uses an N+1 = 3
 * vector subset (DO-178C 6.4.4.3 representative-subset MC/DC).
 *
 * Decision A: ``ra_adc_read_channel`` line 311
 * (libs/ra_hal/src/adc.c): ``if ((chcr == nullptr) || (addr == nullptr))``.
 * The two pointers come from inline accessors with subtly different
 * range checks (``adchcr`` rejects ch >= 24, ``addr`` rejects ch >= 23),
 * which makes channel 23 the only input that exercises the
 * ``addr == NULL`` arm with ``chcr != NULL``.
 * - V1: channel=5  -> chcr!=NULL, addr!=NULL -> dec F (proceeds)
 * - V2: channel=23 -> chcr!=NULL, addr=NULL  -> C1=F, C2=T, dec T
 *                                               (returns out_of_range)
 * - V3: channel=24 -> chcr=NULL  (short-circuits) -> dec T
 *                                               (returns out_of_range)
 * V1+V2 vary C2 only (decision flips); V2+V3 vary C1 with C2 held T.
 *
 * Decision B: ``internal_validate_group_cfg`` line 612
 * ``if ((cfg->num_channels == 0U) ||
 *      (cfg->num_channels > k_ra_adc_scan_group_max_channels))``.
 * - V1: num_channels=3 -> C1=F, C2=F -> dec F (cfg accepted)
 * - V2: num_channels=0 -> C1=T (short-circuits) -> dec T
 * - V3: num_channels=9 (> 8 max) -> C1=F, C2=T -> dec T
 * V1+V3 vary C2 only; V1+V2 vary C1 only.
 */
static void test_mcdc_adc(void)
{
  TEST_BEGIN("adc MC/DC: read_channel OR + validate_group_cfg OR");

  /* --- Decision A: ra_adc_read_channel line 311 ------------------ */
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  uint16_t raw = 0U;
  /* V1: channel=5 (both pointers valid) -> dec F.  read_channel needs
   * an alarm to clear ADACT0; arm it so the call does not spin. */
  arm_adact_clear_alarm();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_read_channel(k_ra_adc_test_ch_valid, &raw));
  disarm_alarm();
  /* V2: channel=23 (chcr!=NULL, addr==NULL) -> C2 alone forces dec T. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range, (int32_t)ra_adc_read_channel(23U, &raw));
  /* V3: channel=24 (chcr==NULL short-circuit) -> C1 alone forces dec T. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_read_channel(k_ra_adc_test_ch_oor, &raw));

  /* --- Decision B: internal_validate_group_cfg line 612 ---------- */
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_adc_init());
  ra_adc_scan_group_cfg_t good = make_scan_cfg();
  /* V1: num_channels=3 -> dec F, accepted. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &good));
  /* V2: num_channels=0 -> C1=T short-circuit -> dec T -> out_of_range. */
  ra_adc_scan_group_cfg_t zero = make_scan_cfg();
  zero.num_channels            = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &zero));
  /* V3: num_channels=9 (> max=8) -> C1=F, C2=T -> dec T -> out_of_range.
   * num_channels is uint8_t, channels[] has 8 slots, but the validator
   * rejects on the count alone before walking the channel array. */
  ra_adc_scan_group_cfg_t big = make_scan_cfg();
  big.num_channels            = (uint8_t)(k_ra_adc_scan_group_max_channels + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_adc_configure_scan_group(k_ra_adc_test_group_one, &big));

  TEST_END("adc MC/DC: read_channel OR + validate_group_cfg OR");
}

int32_t main(void)
{
  test_init_happy();
  test_read_channel_null_out();
  test_read_channel_out_of_range();
  test_read_channel_huge();
  test_read_channel_hcr_but_no_result();
  test_read_channel_completes_via_alarm();
  test_read_channel_timeout();
  test_init_configured();
  test_init_configured_null();
  test_init_configured_scan();
  test_deinit();
  test_set_resolution();
  test_set_resolution_bad();
  test_status_read_and_clear();
  test_status_null();
  test_attach_and_dispatch();
  test_dispatch_no_handler();
  test_power_transition();
  test_configure_scan_group_happy();
  test_configure_scan_group_elc_trigger();
  test_configure_scan_group_rejects_null();
  test_configure_scan_group_rejects_oor();
  test_start_stop_group();
  test_read_group_results_happy();
  test_read_group_results_rejects();
  test_continuous_scan();
  test_compare_window_happy();
  test_compare_window_rejects();
  test_oversampling_encoding();
  test_oversampling_rejects();
  test_mcdc_adc();
  (void)fprintf(stderr, "[OK ] test_ra_adc.c\n");
  return 0;
}
