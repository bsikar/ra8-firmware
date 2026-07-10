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

#include <stdint.h>

#include "ra8d2_adc_b_regs.h"
#include "ra_adc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Sim helper: ADACT0 idle-state (deterministic, no wall-clock timer)
 * --------------------------------------------------------------------------- */

/* The ADC busy-poll reads ADSR.ADACT0 straight from the RAM-backed register
 * window. After ra_sim_mmap_reset() ADACT0 reads 0 (idle), so the driver's
 * bounded busy-wait observes the conversion already complete on its first poll
 * and takes the success path -- deterministically, with no wall-clock timer
 * signal to race the poll. Timeout cases below leave ADACT0 set (1) so the
 * same loop runs to its budget and returns k_ra_err_hw_timeout. */

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
  k_ra_adc_test_default_group_mask = 0x00000001UL, /**< ADSGER bit for group 0.     */
  k_ra_adc_test_admd0_one_cycle    = 0x00000001UL, /**< Expected ADMDR.ADMD0 value. */
  k_ra_adc_test_admd0_continuous   = 0x00000002UL,
  k_ra_adc_test_mdr1_sentinel      = 0x5A5A5A5AUL, /**< Live-window read-back proof. */
} ra_adc_test_const_t;

/**
 * @brief Out-of-range / branch-select arguments for the header accessor guards.
 *
 * @details
 * Each value is one past (or otherwise outside) the accessor's valid domain so
 * the guard's ``nullptr`` leg is taken, plus the two valid ``which`` selectors
 * for ``ra_adc_b_adcmpmdr``. Named to avoid magic numbers in the edge test.
 */
typedef enum : uint8_t {
  k_ra_adc_test_cmpmdr_sel0 = 0U,    /**< ADCMPMDR0 selector (valid).             */
  k_ra_adc_test_cmpmdr_sel1 = 1U,    /**< ADCMPMDR1 selector (valid).             */
  k_ra_adc_test_cmpmdr_oor  = 2U,    /**< which != 0,1 -> nullptr.                */
  k_ra_adc_test_cmptbr_oor  = 8U,    /**< >= k_ra_adc_b_cmp_tables  -> nullptr.   */
  k_ra_adc_test_sgdcr_oor   = 9U,    /**< >= k_ra_adc_b_scan_groups -> nullptr.   */
  k_ra_adc_test_opcrc_oor   = 24U,   /**< >= k_ra_adc_b_max_channels -> nullptr.  */
  k_ra_adc_test_adexdr_oor  = 23U,   /**< >= k_ra_adc_b_ext_data_regs -> nullptr. */
  k_ra_adc_test_cnvcs_below = 0x5FU, /**< < k_ra_adc_b_ext_chan_base -> sentinel. */
} ra_adc_test_edge_t;

/* ---------------------------------------------------------------------------
 * Tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

static void test_init_happy(void)
{
  TEST_BEGIN("adc init happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  TEST_ASSERT((*ra_adc_b_adclkenr() & k_ra_adclkenr_mask_clken) != 0U);
  /* ADMDR carries the one-cycle mode for ADC0. */
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_one_cycle, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  /* ADSGER enables the default scan group. */
  TEST_ASSERT_EQ(k_ra_adc_test_default_group_mask, (*ra_adc_b_adsger() & k_ra_adsger_mask_sgren));
  TEST_END("adc init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_channel_null_out(void)
{
  TEST_BEGIN("adc read_channel null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_read_channel(k_ra_adc_test_ch_zero, nullptr));
  TEST_END("adc read_channel null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_channel_out_of_range(void)
{
  TEST_BEGIN("adc read_channel out of range");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_read_channel(k_ra_adc_test_ch_oor, &raw));
  TEST_END("adc read_channel out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_channel_huge(void)
{
  TEST_BEGIN("adc read_channel huge ch");
  ra_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_read_channel(k_ra_adc_test_ch_huge, &raw));
  TEST_END("adc read_channel huge ch");
}

/**
 * @brief Hits the adc.c branch where ADCHCR[ch] is valid but ADDR[ch]
 *        is out-of-range (channel 23: FSP has 24 ADCHCR slots but
 *        only 23 ADDR result slots).
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_read_channel_hcr_but_no_result(void)
{
  TEST_BEGIN("adc read_channel: ch 23 (hcr valid, addr oob)");
  ra_sim_mmap_reset();
  uint16_t raw = 0U;
  /* k_ra_adc_b_max_channels = 24, k_ra_adc_b_result_regs = 23. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_read_channel(23U, &raw));
  TEST_END("adc read_channel: ch 23 (hcr valid, addr oob)");
}

/**
 * @brief Drive a read with ADACT0 already idle so the busy-poll completes
 *        on its first read (deterministic, no injected stimulus).
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_read_channel_completes(void)
{
  TEST_BEGIN("adc read_channel: ADACT0 idle -> completes");
  ra_sim_mmap_reset();

  *ra_adc_b_addr(k_ra_adc_test_ch_zero) = (uint32_t)k_ra_adc_test_result_a;

  uint16_t       raw = 0U;
  const ra_err_t err = ra_adc_read_channel(k_ra_adc_test_ch_zero, &raw);

  TEST_ASSERT_EQ(k_ra_ok, err);
  TEST_ASSERT_EQ(k_ra_adc_test_result_a, raw);
  /* ADCHCRn[0] should now have CNVCS == 0 (channel 0 mapped onto itself). */
  const uint32_t chcr = *ra_adc_b_adchcr(k_ra_adc_test_ch_zero);
  TEST_ASSERT_EQ(0, ((chcr & k_ra_adchcr_mask_cnvcs) >> (uint32_t)k_ra_adchcr_bit_cnvcs));
  TEST_END("adc read_channel: ADACT0 idle -> completes");
}

/**
 * @brief With ADACT0 stuck set the driver bounded-polls then reports
 *        k_ra_err_hw_timeout.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_read_channel_timeout(void)
{
  TEST_BEGIN("adc read_channel: poll timeout");
  ra_sim_mmap_reset();

  /* Force ADACT0 stuck high so the busy poll never exits. */
  *ra_adc_b_adsr() = k_ra_adsr_mask_adact0;

  uint16_t       raw = 0xBEEFU;
  const ra_err_t err = ra_adc_read_channel(k_ra_adc_test_ch_valid, &raw);
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, err);
  TEST_ASSERT_EQ(0, raw);
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_configured(void)
{
  TEST_BEGIN("adc init configured: 14b right-aligned");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init_configured(&cfg));

  /* ADMDR.ADMD0 must encode one-cycle mode (single-shot). */
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_one_cycle, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  /* All 24 ADCHCR slots should have been zeroed. */
  for (uint8_t ch = 0U; ch < k_ra_adc_b_max_channels; ++ch) {
    TEST_ASSERT_EQ(0, *ra_adc_b_adchcr(ch));
  }
  TEST_END("adc init configured: 14b right-aligned");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_configured_null(void)
{
  TEST_BEGIN("adc init configured null");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_init_configured(nullptr));
  TEST_END("adc init configured null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_configured_scan(void)
{
  TEST_BEGIN("adc init configured: scan mode");
  ra_sim_mmap_reset();

  ra_adc_cfg_t cfg = make_cfg();
  cfg.scan_mode    = true;
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init_configured(&cfg));
  /* ADMDR.ADMD0 must encode the continuous-scan mode code. */
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_continuous, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  TEST_END("adc init configured: scan mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("adc deinit");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_deinit());
  TEST_ASSERT_EQ(0, *ra_adc_b_admdr());
  TEST_ASSERT_EQ(0, *ra_adc_b_adsger());
  TEST_ASSERT_EQ(0, *ra_adc_b_adclkenr());
  TEST_END("adc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_resolution(void)
{
  TEST_BEGIN("adc set_resolution updates ADMD0");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_set_resolution(k_ra_adc_res_12bit));

  /* ADMD0 should still hold the one-cycle code (every public resolution
   * selector currently maps to one-cycle). */
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_one_cycle, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));
  TEST_END("adc set_resolution updates ADMD0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_resolution_bad(void)
{
  TEST_BEGIN("adc set_resolution bad");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_adc_set_resolution((ra_adc_resolution_t)9U));
  TEST_END("adc set_resolution bad");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("adc status read + clear");
  ra_sim_mmap_reset();

  /* Pre-set ADSR.ADACT0 (busy) and ADINTCR.ADIE0 (IE) to assert. */
  *ra_adc_b_adsr()    = k_ra_adsr_mask_adact0;
  *ra_adc_b_adintcr() = 1UL;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra_adc_status_busy) != 0U);
  TEST_ASSERT((mask & k_ra_adc_status_ie) != 0U);

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_clear_status());
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra_adc_status_busy) == 0U);
  TEST_END("adc status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_null(void)
{
  TEST_BEGIN("adc status null");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_get_status(nullptr));
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("adc attach + dispatch");
  ra_sim_mmap_reset();
  s_adc_cb_count       = 0U;
  s_adc_cb_last_result = 0U;
  s_adc_cb_last_ctx    = nullptr;

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_attach_handler(stub_adc_cb, (void*)(uintptr_t)0xA5A5U));

  volatile uint32_t* addr = ra_adc_b_addr(k_ra_adc_test_ch_valid);
  *addr                   = (uint32_t)k_ra_adc_test_result_a;

  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ(1, s_adc_cb_count);
  TEST_ASSERT_EQ(k_ra_adc_test_result_a, s_adc_cb_last_result);
  TEST_END("adc attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("adc dispatch no handler");
  ra_sim_mmap_reset();
  s_adc_cb_count = 0U;

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_attach_handler(nullptr, nullptr));
  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_valid);
  ra_adc_dispatch_cnv_end(k_ra_adc_test_ch_oor);
  TEST_ASSERT_EQ(0, s_adc_cb_count);
  TEST_END("adc dispatch no handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("adc power transition");
  ra_sim_mmap_reset();

  const ra_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_enter_stop());
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_exit_stop());
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_scan_group_happy(void)
{
  TEST_BEGIN("adc scan-group: configure happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));

  /* ADSGER must have group-1 bit set in addition to the default group-0 bit. */
  const uint32_t expected = (1UL << k_ra_adc_test_group_one) | 1UL;
  TEST_ASSERT((*ra_adc_b_adsger() & expected) == expected);

  /* Each listed ADCHCR slot must encode SGSEL == 1 and CNVCS == ch. */
  for (uint8_t i = 0U; i < cfg.num_channels; ++i) {
    const uint32_t chcr = *ra_adc_b_adchcr(cfg.channels[i]);
    const uint32_t sg   = (chcr & k_ra_adchcr_mask_sgsel) >> (uint32_t)k_ra_adchcr_bit_sgsel;
    const uint32_t cs   = (chcr & k_ra_adchcr_mask_cnvcs) >> (uint32_t)k_ra_adchcr_bit_cnvcs;
    TEST_ASSERT_EQ(k_ra_adc_test_group_one, sg);
    TEST_ASSERT_EQ(cfg.channels[i], cs);
  }
  /* Software trigger -> ADTRGENR group-1 bit is clear. */
  TEST_ASSERT((*ra_adc_b_adtrgenr() & (1UL << k_ra_adc_test_group_one)) == 0U);
  TEST_END("adc scan-group: configure happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_scan_group_elc_trigger(void)
{
  TEST_BEGIN("adc scan-group: ELC trigger arms ADTRGENR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  cfg.trigger                 = k_ra_adc_trig_src_elc;
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));
  TEST_ASSERT((*ra_adc_b_adtrgenr() & (1UL << k_ra_adc_test_group_one)) != 0U);
  TEST_END("adc scan-group: ELC trigger arms ADTRGENR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_scan_group_rejects_null(void)
{
  TEST_BEGIN("adc scan-group: null cfg");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_configure_scan_group(0U, nullptr));
  TEST_END("adc scan-group: null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_configure_scan_group_rejects_oor(void)
{
  TEST_BEGIN("adc scan-group: out-of-range group/channels");
  ra_sim_mmap_reset();
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_configure_scan_group(k_ra_adc_test_group_oor, &cfg));
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_configure_scan_group(k_ra_adc_test_group_huge, &cfg));

  ra_adc_scan_group_cfg_t bad_cfg = make_scan_cfg();
  bad_cfg.num_channels            = 0U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_configure_scan_group(0U, &bad_cfg));

  bad_cfg             = make_scan_cfg();
  bad_cfg.channels[0] = (uint8_t)k_ra_adc_b_max_channels; /* OOR. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_configure_scan_group(0U, &bad_cfg));
  TEST_END("adc scan-group: out-of-range group/channels");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_stop_group(void)
{
  TEST_BEGIN("adc scan-group: start/stop sets ADSTR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_start_group(k_ra_adc_test_group_one));
  TEST_ASSERT((*ra_adc_b_adstr(k_ra_adc_test_group_one) & k_ra_adstr_mask_adst) != 0U);
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_stop_group(k_ra_adc_test_group_one));
  TEST_ASSERT((*ra_adc_b_adstr(k_ra_adc_test_group_one) & k_ra_adstr_mask_adst) == 0U);
  TEST_ASSERT((*ra_adc_b_adstopr() & (k_ra_adstopr_mask_adstop0 | k_ra_adstopr_mask_adstop1)) !=
              0U);

  /* Out-of-range groups are rejected on both entry points. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_start_group(k_ra_adc_test_group_oor));
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_stop_group(k_ra_adc_test_group_huge));
  TEST_END("adc scan-group: start/stop sets ADSTR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_group_results_happy(void)
{
  TEST_BEGIN("adc scan-group: read multi-channel results");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  const ra_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_configure_scan_group(k_ra_adc_test_group_one, &cfg));

  /* Pre-load distinct ADDR slots so we can verify ordering. */
  *ra_adc_b_addr(cfg.channels[0]) = 0x0111U;
  *ra_adc_b_addr(cfg.channels[1]) = 0x0222U;
  *ra_adc_b_addr(cfg.channels[2]) = 0x0333U;

  uint16_t buf[8] = {};
  uint8_t  count  = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_read_group_results(k_ra_adc_test_group_one, buf, &count));
  TEST_ASSERT_EQ(cfg.num_channels, count);
  TEST_ASSERT_EQ(0x0111, buf[0]);
  TEST_ASSERT_EQ(0x0222, buf[1]);
  TEST_ASSERT_EQ(0x0333, buf[2]);
  TEST_END("adc scan-group: read multi-channel results");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_group_results_rejects(void)
{
  TEST_BEGIN("adc scan-group: read rejects null/oor/unconfigured");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  uint16_t buf   = 0U;
  uint8_t  count = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_read_group_results(0U, nullptr, &count));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_adc_read_group_results(0U, &buf, nullptr));
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_read_group_results(k_ra_adc_test_group_oor, &buf, &count));
  /* Group 1 is unconfigured at this point. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_read_group_results(k_ra_adc_test_group_one, &buf, &count));
  TEST_END("adc scan-group: read rejects null/oor/unconfigured");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_continuous_scan(void)
{
  TEST_BEGIN("adc scan-group: continuous-scan toggles ADMDR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_set_continuous_scan(k_ra_adc_test_group_one, true));
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_continuous, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_set_continuous_scan(k_ra_adc_test_group_one, false));
  TEST_ASSERT_EQ(k_ra_adc_test_admd0_one_cycle, (*ra_adc_b_admdr() & k_ra_admdr_mask_admd0));

  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_set_continuous_scan(k_ra_adc_test_group_oor, true));
  TEST_END("adc scan-group: continuous-scan toggles ADMDR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compare_window_happy(void)
{
  TEST_BEGIN("adc compare-window: programs ADCMPTBR/MDR/ENR");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_adc_set_compare_window(k_ra_adc_test_cmp_table,
                                           k_ra_adc_test_window_low,
                                           k_ra_adc_test_window_high));

  const uint32_t tbl  = *ra_adc_b_adcmptbr(k_ra_adc_test_cmp_table);
  const uint32_t low  = (tbl & k_ra_adcmptbr_mask_low) >> (uint32_t)k_ra_adcmptbr_bit_low;
  const uint32_t high = (tbl & k_ra_adcmptbr_mask_high) >> (uint32_t)k_ra_adcmptbr_bit_high;
  TEST_ASSERT_EQ(k_ra_adc_test_window_low, low);
  TEST_ASSERT_EQ(k_ra_adc_test_window_high, high);
  TEST_ASSERT((*ra_adc_b_adcmpenr() & (1UL << k_ra_adc_test_cmp_table)) != 0U);

  /* CMPMD field for table 3 lives in ADCMPMDR0 at bit (3 * 8) = 24. */
  const uint32_t mdr0  = *ra_adc_b_adcmpmdr(0U);
  const uint32_t shift = (uint32_t)k_ra_adc_test_cmp_table * (uint32_t)k_ra_adcmpmd_field_step;
  TEST_ASSERT_EQ(k_ra_adcmpmd_inside, ((mdr0 >> shift) & 0x3UL));

  /* Per-channel ADDOPCRB CMPTBLEm bit is set for our table. */
  const uint32_t opcrb = *ra_adc_b_addopcrb(k_ra_adc_test_cmp_table);
  TEST_ASSERT((opcrb & k_ra_addopcrb_mask_cmptblem) != 0U);
  TEST_END("adc compare-window: programs ADCMPTBR/MDR/ENR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compare_window_rejects(void)
{
  TEST_BEGIN("adc compare-window: rejects high<low and oor table");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_adc_set_compare_window(0U, k_ra_adc_test_window_high, k_ra_adc_test_window_low));
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_set_compare_window(k_ra_adc_test_cmp_oor,
                                           k_ra_adc_test_window_low,
                                           k_ra_adc_test_window_high));
  TEST_END("adc compare-window: rejects high<low and oor table");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_oversampling_encoding(void)
{
  TEST_BEGIN("adc oversampling: AVEMD/ADC encoding");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());

  TEST_ASSERT_EQ(k_ra_ok, ra_adc_set_oversampling(k_ra_adc_test_ch_valid, k_ra_adc_oversample_16x));
  const uint32_t opcrb = *ra_adc_b_addopcrb(k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ(k_ra_avemd_average,
                 ((opcrb & k_ra_addopcrb_mask_avemd) >> (uint32_t)k_ra_addopcrb_bit_avemd));
  TEST_ASSERT_EQ(k_ra_adc_avg_16x,
                 ((opcrb & k_ra_addopcrb_mask_adc) >> (uint32_t)k_ra_addopcrb_bit_adc));

  /* Switching back to off must clear AVEMD. */
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_set_oversampling(k_ra_adc_test_ch_valid, k_ra_adc_oversample_off));
  const uint32_t opcrb_after = *ra_adc_b_addopcrb(k_ra_adc_test_ch_valid);
  TEST_ASSERT_EQ(k_ra_avemd_off, (opcrb_after & k_ra_addopcrb_mask_avemd));
  TEST_END("adc oversampling: AVEMD/ADC encoding");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_oversampling_rejects(void)
{
  TEST_BEGIN("adc oversampling: rejects bad mode and oor channel");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_adc_set_oversampling(0U, (ra_adc_oversample_t)0xFFU));
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_set_oversampling(k_ra_adc_test_ch_oor, k_ra_adc_oversample_4x));
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
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  uint16_t raw = 0U;
  /* V1: channel=5 (both pointers valid) -> dec F.  ADACT0 reads idle
   * after reset, so read_channel completes without spinning. */
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_read_channel(k_ra_adc_test_ch_valid, &raw));
  /* V2: channel=23 (chcr!=NULL, addr==NULL) -> C2 alone forces dec T. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_read_channel(23U, &raw));
  /* V3: channel=24 (chcr==NULL short-circuit) -> C1 alone forces dec T. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_read_channel(k_ra_adc_test_ch_oor, &raw));

  /* --- Decision B: internal_validate_group_cfg line 612 ---------- */
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_init());
  ra_adc_scan_group_cfg_t good = make_scan_cfg();
  /* V1: num_channels=3 -> dec F, accepted. */
  TEST_ASSERT_EQ(k_ra_ok, ra_adc_configure_scan_group(k_ra_adc_test_group_one, &good));
  /* V2: num_channels=0 -> C1=T short-circuit -> dec T -> out_of_range. */
  ra_adc_scan_group_cfg_t zero = make_scan_cfg();
  zero.num_channels            = 0U;
  TEST_ASSERT_EQ(k_ra_err_out_of_range,
                 ra_adc_configure_scan_group(k_ra_adc_test_group_one, &zero));
  /* V3: num_channels=9 (> max=8) -> C1=F, C2=T -> dec T -> out_of_range.
   * num_channels is uint8_t, channels[] has 8 slots, but the validator
   * rejects on the count alone before walking the channel array. */
  ra_adc_scan_group_cfg_t big = make_scan_cfg();
  big.num_channels            = (uint8_t)(k_ra_adc_scan_group_max_channels + 1U);
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_adc_configure_scan_group(k_ra_adc_test_group_one, &big));

  TEST_END("adc MC/DC: read_channel OR + validate_group_cfg OR");
}

/**
 * @brief Exercise the out-of-range and branch-select legs of the header
 *        accessor guards (``ra8d2_adc_b_regs.h``).
 *
 * @details
 * The register-header inline accessors each guard their index argument and
 * return ``nullptr`` when it is outside the modelled register array. The
 * happy-path (in-range) legs are covered by the driver tests above; this case
 * drives the still-uncovered ``nullptr`` legs plus the ``ra_adc_b_adcmpmdr``
 * which-selector branches so the header clears the >= 90% per-file floor under
 * gcc. Every input is a compile-time constant, so the case is deterministic
 * and needs no interrupt or injected stimulus.
 *
 * @par MC/DC:
 * Every accessor guard touched here is a single-condition decision (e.g.
 * ``if (n >= k_ra_adc_b_ext_data_regs)``), so N+1 reduces to the two-outcome
 * pair: the FALSE (in-range) leg is exercised by the driver tests above, and
 * this case supplies the TRUE (out-of-range -> nullptr) leg. ``ra_adc_b_adcmpmdr``
 * is a two-way selector (which==0 / which==1 / else) whose three legs are all
 * driven here (sel0 valid above, sel1 + else here). No ``&&`` / ``||`` compound
 * decisions are present in the code under test.
 */
static void test_accessor_edge_pointers(void)
{
  TEST_BEGIN("adc header accessors: out-of-range guard legs");
  ra_sim_mmap_reset();

  /* ra_adc_b_adcmpmdr: which==1 selects ADCMPMDR1 (valid, live window). */
  volatile uint32_t* mdr0 = ra_adc_b_adcmpmdr(k_ra_adc_test_cmpmdr_sel0);
  volatile uint32_t* mdr1 = ra_adc_b_adcmpmdr(k_ra_adc_test_cmpmdr_sel1);
  TEST_ASSERT_NOT_NULL(mdr0);
  TEST_ASSERT_NOT_NULL(mdr1);
  /* ADCMPMDR1 sits one 32-bit register (offset delta) above ADCMPMDR0. */
  TEST_ASSERT_EQ((uintptr_t)k_ra_adc_b_off_adcmpmdr1 - (uintptr_t)k_ra_adc_b_off_adcmpmdr0,
                 (uintptr_t)mdr1 - (uintptr_t)mdr0);
  *mdr1 = (uint32_t)k_ra_adc_test_mdr1_sentinel;
  TEST_ASSERT_EQ(k_ra_adc_test_mdr1_sentinel, *mdr1);
  /* which != 0,1 -> nullptr. */
  TEST_ASSERT_NULL(ra_adc_b_adcmpmdr(k_ra_adc_test_cmpmdr_oor));

  /* Remaining index guards: one-past-the-end argument -> nullptr leg. */
  TEST_ASSERT_NULL(ra_adc_b_adcmptbr(k_ra_adc_test_cmptbr_oor));
  TEST_ASSERT_NULL(ra_adc_b_adsgdcr(k_ra_adc_test_sgdcr_oor));
  TEST_ASSERT_NULL(ra_adc_b_addopcrc(k_ra_adc_test_opcrc_oor));
  TEST_ASSERT_NULL(ra_adc_b_adexdr(k_ra_adc_test_adexdr_oor));

  /* ra_adc_b_adexdr_index_for_chan: CNVCS below the extended-channel base
   * returns the no-index sentinel (the >= base leg is covered by selfdiag). */
  TEST_ASSERT_EQ(k_ra_adc_b_adexdr_no_index,
                 ra_adc_b_adexdr_index_for_chan(k_ra_adc_test_cnvcs_below));

  TEST_END("adc header accessors: out-of-range guard legs");
}

int32_t main(void)
{
  test_init_happy();
  test_read_channel_null_out();
  test_read_channel_out_of_range();
  test_read_channel_huge();
  test_read_channel_hcr_but_no_result();
  test_read_channel_completes();
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
  test_accessor_edge_pointers();
  (void)fprintf(stderr, "[OK ] test_ra_adc.c\n");
  return 0;
}
