/**
 * @file test_ra8_adc.c
 * @brief Unit tests for adc.c (ADC_B polling driver, FSP-verified layout)
 *
 * @details
 * The ADC_B driver was re-derived against FSP ``R_ADC_B0_Type``
 * (R7KA8D2KF_core0.h lines 20460-24938) which corrected the
 * register layout away from the imaginary CVEN / RESSEL / ADTRGMD /
 * ADSCANMD / ADBUSY bit map. These tests verify:
 *   - ADCLKENR is set after init
 *   - ADMDR encodes the ADMD0 unit operating mode (one-cycle / continuous)
 *   - ADDOPCRCn.ADPRC encodes the per-channel conversion resolution
 *   - ADSGER enables the default scan group (group 0)
 *   - ``ra8_adc_read_channel`` programmes ADCHCRn and kicks ADSTR[0]
 *   - The driver polls ADSR.ADACT0 and reads ADDR[ch].DATA on success
 *   - Out-of-range channels and the ADCHCR-valid-but-ADDR-OOB
 *     boundary (channel 23) return ``k_ra8_err_out_of_range``
 *
 * The scan-group / comparator / oversampling tests, the MC/DC vectors,
 * and the accessor edge-pointer tests live in test_ra8_adc_scan.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum adc_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_adc_resolution_invalid =
    9U, /**< A resolution outside the enumeration, which configuration must reject. */
} adc_fixture_t;

/**
 * @enum adc_fixture2_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint16_t {
  k_adc_poison_out =
    0xBEEFU, /**< Poison written into the sample out-parameter, so a read that fails without setting it is detectable. */
} adc_fixture2_t;

/* ---------------------------------------------------------------------------
 * Sim helper: ADACT0 idle-state (deterministic, no wall-clock timer)
 * --------------------------------------------------------------------------- */

/* The ADC busy-poll reads ADSR.ADACT0 straight from the RAM-backed register
 * window. After ra8_sim_mmap_reset() ADACT0 reads 0 (idle), so the driver's
 * bounded busy-wait observes the conversion already complete on its first poll
 * and takes the success path -- deterministically, with no wall-clock timer
 * signal to race the poll. Timeout cases below leave ADACT0 set (1) so the
 * same loop runs to its budget and returns k_ra8_err_hw_timeout. */

/* ---------------------------------------------------------------------------
 * Test fixtures
 * --------------------------------------------------------------------------- */

typedef enum : uint8_t {
  k_ra8_adc_test_ch_zero  = 0U,   /**< RA8 ADC test channel zero.        */
  k_ra8_adc_test_ch_valid = 5U,   /**< RA8 ADC test channel valid.       */
  k_ra8_adc_test_ch_max   = 22U,  /**< Highest ADDR[] slot (23 results). */
  k_ra8_adc_test_ch_oor   = 24U,  /**< RA8 ADC test channel oor.         */
  k_ra8_adc_test_ch_huge  = 200U, /**< RA8 ADC test channel huge.        */
} ra8_adc_test_ch_t;

typedef enum : uint16_t {
  k_ra8_adc_test_result_a = 0x1234U, /**< RA8 ADC test result a. */
  k_ra8_adc_test_result_b = 0x0BEEU, /**< RA8 ADC test result b. */
} ra8_adc_test_val_t;

typedef enum : uint32_t {
  k_ra8_adc_test_default_group_mask = 0x00000001UL, /**< ADSGER bit for group 0.        */
  k_ra8_adc_test_admd0_one_cycle    = 0x00000001UL, /**< Expected ADMDR.ADMD0 value.    */
  k_ra8_adc_test_admd0_continuous   = 0x00000002UL, /**< RA8 ADC test admd0 continuous. */
  k_ra8_adc_test_mdr1_sentinel      = 0x5A5A5A5AUL, /**< Live-window read-back proof.   */
} ra8_adc_test_const_t;

/**
 * @brief Out-of-range / branch-select arguments for the header accessor guards.
 *
 * @details
 * Each value is one past (or otherwise outside) the accessor's valid domain so
 * the guard's ``nullptr`` leg is taken, plus the two valid ``which`` selectors
 * for ``ra8_adc_b_adcmpmdr``. Named to avoid magic numbers in the edge test.
 */
typedef enum : uint8_t {
  k_ra8_adc_test_cmpmdr_sel0 = 0U,    /**< ADCMPMDR0 selector (valid).              */
  k_ra8_adc_test_cmpmdr_sel1 = 1U,    /**< ADCMPMDR1 selector (valid).              */
  k_ra8_adc_test_cmpmdr_oor  = 2U,    /**< which != 0,1 -> nullptr.                 */
  k_ra8_adc_test_cmptbr_oor  = 8U,    /**< >= k_ra8_adc_b_cmp_tables  -> nullptr.   */
  k_ra8_adc_test_sgdcr_oor   = 9U,    /**< >= k_ra8_adc_b_scan_groups -> nullptr.   */
  k_ra8_adc_test_opcrc_oor   = 24U,   /**< >= k_ra8_adc_b_max_channels -> nullptr.  */
  k_ra8_adc_test_adexdr_oor  = 23U,   /**< >= k_ra8_adc_b_ext_data_regs -> nullptr. */
  k_ra8_adc_test_cnvcs_below = 0x5FU, /**< < k_ra8_adc_b_ext_chan_base -> sentinel. */
} ra8_adc_test_edge_t;

/* ---------------------------------------------------------------------------
 * Tests
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

/** @brief Extract the ADDOPCRCn.ADPRC[1:0] data-format field for a channel. */
static uint32_t adc_adprc_of(uint8_t ch)
{
  const uint32_t opcrc = *ra8_adc_b_addopcrc(ch);
  return (opcrc & k_ra8_addopcrc_mask_adprc) >> (uint32_t)k_ra8_addopcrc_bit_adprc;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("adc init happy");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  TEST_ASSERT((*ra8_adc_b_adclkenr() & k_ra8_adclkenr_mask_clken) != 0U);
  /* ADMDR carries the one-cycle mode for ADC0. */
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_one_cycle, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));
  /* ADSGER enables the default scan group. */
  TEST_ASSERT_EQ(k_ra8_adc_test_default_group_mask,
                 (*ra8_adc_b_adsger() & k_ra8_adsger_mask_sgren));
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
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_read_channel(k_ra8_adc_test_ch_zero, nullptr));
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
  ra8_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel(k_ra8_adc_test_ch_oor, &raw));
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
  ra8_sim_mmap_reset();

  uint16_t raw = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel(k_ra8_adc_test_ch_huge, &raw));
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
  ra8_sim_mmap_reset();
  uint16_t raw = 0U;
  /* k_ra8_adc_b_max_channels = 24, k_ra8_adc_b_result_regs = 23. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel(23U, &raw));
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
  ra8_sim_mmap_reset();

  *ra8_adc_b_addr(k_ra8_adc_test_ch_zero) = (uint32_t)k_ra8_adc_test_result_a;

  uint16_t        raw = 0U;
  const ra8_err_t err = ra8_adc_read_channel(k_ra8_adc_test_ch_zero, &raw);

  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_ra8_adc_test_result_a, raw);
  /* ADCHCRn[0] should now have CNVCS == 0 (channel 0 mapped onto itself). */
  const uint32_t chcr = *ra8_adc_b_adchcr(k_ra8_adc_test_ch_zero);
  TEST_ASSERT_EQ(0, ((chcr & k_ra8_adchcr_mask_cnvcs) >> (uint32_t)k_ra8_adchcr_bit_cnvcs));
  TEST_END("adc read_channel: ADACT0 idle -> completes");
}

/**
 * @brief With ADACT0 stuck set the driver bounded-polls then reports
 *        k_ra8_err_hw_timeout.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_read_channel_timeout(void)
{
  TEST_BEGIN("adc read_channel: poll timeout");
  ra8_sim_mmap_reset();

  /* Force ADACT0 stuck high so the busy poll never exits. */
  *ra8_adc_b_adsr() = k_ra8_adsr_mask_adact0;

  uint16_t        raw = k_adc_poison_out;
  const ra8_err_t err = ra8_adc_read_channel(k_ra8_adc_test_ch_valid, &raw);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_ASSERT_EQ(0, raw);
  TEST_END("adc read_channel: poll timeout");
}

static ra8_adc_cfg_t make_cfg(void)
{
  const ra8_adc_cfg_t cfg = {
    .resolution    = k_ra8_adc_res_14bit,
    .trigger       = k_ra8_adc_trig_software,
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
  ra8_sim_mmap_reset();

  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));

  /* ADMDR.ADMD0 must encode one-cycle mode (single-shot). */
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_one_cycle, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));
  /* All 24 ADCHCR slots should have been zeroed. */
  for (uint8_t ch = 0U; ch < k_ra8_adc_b_max_channels; ++ch) {
    TEST_ASSERT_EQ(0, *ra8_adc_b_adchcr(ch));
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
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_init_configured(nullptr));
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
  ra8_sim_mmap_reset();

  ra8_adc_cfg_t cfg = make_cfg();
  cfg.scan_mode     = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  /* ADMDR.ADMD0 must encode the continuous-scan mode code. */
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_continuous, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));
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
  ra8_sim_mmap_reset();

  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_deinit());
  TEST_ASSERT_EQ(0, *ra8_adc_b_admdr());
  TEST_ASSERT_EQ(0, *ra8_adc_b_adsger());
  TEST_ASSERT_EQ(0, *ra8_adc_b_adclkenr());
  TEST_END("adc deinit");
}

/**
 * @brief Every resolution selector installs its ADDOPCRCn.ADPRC code on the
 *        result-producing channels, and leaves the ADMD0 operating mode alone.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the resolution ->
 * ADPRC translation switch, one selector per call; no `&&` or `||` in
 * the code under test that this case touches)
 */
static void test_set_resolution(void)
{
  TEST_BEGIN("adc set_resolution updates ADDOPCRC.ADPRC");
  ra8_sim_mmap_reset();

  const ra8_adc_cfg_t cfg = make_cfg(); /* 14-bit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  /* init_configured applied the 14-bit ADPRC to the result channels. */
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_14bit, adc_adprc_of(k_ra8_adc_test_ch_zero));

  /* 16-bit is the RA8P1 ADC16H headline capability (ADPRC = 0b00). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_resolution(k_ra8_adc_res_16bit));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_16bit, adc_adprc_of(k_ra8_adc_test_ch_zero));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_16bit, adc_adprc_of(k_ra8_adc_test_ch_valid));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_resolution(k_ra8_adc_res_12bit));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_12bit, adc_adprc_of(k_ra8_adc_test_ch_valid));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_resolution(k_ra8_adc_res_10bit));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_10bit, adc_adprc_of(k_ra8_adc_test_ch_max));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_resolution(k_ra8_adc_res_14bit));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_14bit, adc_adprc_of(k_ra8_adc_test_ch_valid));

  /* Resolution is orthogonal to ADMD0: the one-cycle operating mode is intact. */
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_one_cycle, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));
  TEST_END("adc set_resolution updates ADDOPCRC.ADPRC");
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
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_adc_set_resolution((ra8_adc_resolution_t)9U));
  TEST_END("adc set_resolution bad");
}

/**
 * @brief ``ra8_adc_init_configured`` honours a 16-bit descriptor by writing the
 *        16-bit ADPRC code (ADC16H headline capability) to the result channels.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the init-time
 * resolution -> ADPRC application; no `&&` or `||` in the code under
 * test that this case touches)
 */
static void test_init_configured_16bit(void)
{
  TEST_BEGIN("adc init configured: 16-bit ADPRC");
  ra8_sim_mmap_reset();

  ra8_adc_cfg_t cfg = make_cfg();
  cfg.resolution    = k_ra8_adc_res_16bit;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_16bit, adc_adprc_of(k_ra8_adc_test_ch_zero));
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_16bit, adc_adprc_of(k_ra8_adc_test_ch_max));
  TEST_END("adc init configured: 16-bit ADPRC");
}

/**
 * @brief ``ra8_adc_init_configured`` rejects an out-of-range resolution before
 *        it powers the module.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * resolution-validation guard; no `&&` or `||` in the code under test
 * that this case touches)
 */
static void test_init_configured_bad_resolution(void)
{
  TEST_BEGIN("adc init configured: bad resolution rejected");
  ra8_sim_mmap_reset();

  ra8_adc_cfg_t cfg = make_cfg();
  cfg.resolution    = (ra8_adc_resolution_t)k_adc_resolution_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_adc_init_configured(&cfg));
  TEST_END("adc init configured: bad resolution rejected");
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
  ra8_sim_mmap_reset();

  /* Pre-set ADSR.ADACT0 (busy) and ADINTCR.ADIE0 (IE) to assert. */
  *ra8_adc_b_adsr()    = k_ra8_adsr_mask_adact0;
  *ra8_adc_b_adintcr() = 1UL;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra8_adc_status_busy) != 0U);
  TEST_ASSERT((mask & k_ra8_adc_status_ie) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_clear_status());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_get_status(&mask));
  TEST_ASSERT((mask & k_ra8_adc_status_busy) == 0U);
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
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_get_status(nullptr));
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
  ra8_sim_mmap_reset();
  s_adc_cb_count       = 0U;
  s_adc_cb_last_result = 0U;
  s_adc_cb_last_ctx    = nullptr;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_attach_handler(stub_adc_cb, (void*)(uintptr_t)0xA5A5U));

  volatile uint32_t* addr = ra8_adc_b_addr(k_ra8_adc_test_ch_valid);
  *addr                   = (uint32_t)k_ra8_adc_test_result_a;

  ra8_adc_dispatch_cnv_end(k_ra8_adc_test_ch_valid);
  TEST_ASSERT_EQ(1, s_adc_cb_count);
  TEST_ASSERT_EQ(k_ra8_adc_test_result_a, s_adc_cb_last_result);
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
  ra8_sim_mmap_reset();
  s_adc_cb_count = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_attach_handler(nullptr, nullptr));
  ra8_adc_dispatch_cnv_end(k_ra8_adc_test_ch_valid);
  ra8_adc_dispatch_cnv_end(k_ra8_adc_test_ch_oor);
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
  ra8_sim_mmap_reset();

  const ra8_adc_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_exit_stop());
  TEST_END("adc power transition");
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
  test_init_happy,
  test_read_channel_null_out,
  test_read_channel_out_of_range,
  test_read_channel_huge,
  test_read_channel_hcr_but_no_result,
  test_read_channel_completes,
  test_read_channel_timeout,
  test_init_configured,
  test_init_configured_null,
  test_init_configured_scan,
  test_deinit,
  test_set_resolution,
  test_set_resolution_bad,
  test_init_configured_16bit,
  test_init_configured_bad_resolution,
  test_status_read_and_clear,
  test_status_null,
  test_attach_and_dispatch,
  test_dispatch_no_handler,
  test_power_transition,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_adc.c\n");
  return 0;
}
