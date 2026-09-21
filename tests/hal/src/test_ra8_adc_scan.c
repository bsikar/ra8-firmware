/**
 * @file test_ra8_adc_scan.c
 * @brief Scan-group / comparator / oversample tests for adc.c (ADC_B driver)
 *
 * @details
 * Split out of test_ra8_adc.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the sweep-3 scan-group + ELC
 * trigger + window comparator + oversampling tests plus the MC/DC vectors
 * and the header-accessor edge-pointer tests; the init / read-channel /
 * resolution / status / dispatch contract tests stay in test_ra8_adc.c.
 *
 * The ADC busy-poll reads ADSR.ADACT0 straight from the RAM-backed register
 * window. After ra8_fake_mmap_reset() ADACT0 reads 0 (idle), so the driver's
 * bounded busy-wait observes the conversion already complete on its first
 * poll and takes the success path deterministically.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum adc_scan_channel_t
 * @brief Which channels the scan is configured to sample.
 */
typedef enum : uint8_t {
  k_adc_second_channel =
    5U, /**< Middle-slot channel, not adjacent to the first, so a slot mix-up cannot land on it. */
} adc_scan_channel_t;

/**
 * @enum adc_scan_code_t
 * @brief One distinct conversion result per channel, so a scan that read the wrong slot fails a specific assertion.
 */
typedef enum : uint16_t {
  k_adc_code_ch0 = 0x0111U, /**< Conversion result planted for the first channel. */
  k_adc_code_ch1 = 0x0222U, /**< For the second.                                  */
  k_adc_code_ch2 =
    0x0333U, /**< For the third; three differ so a scan of the wrong slot fails an assertion. */
} adc_scan_code_t;

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

typedef enum : uint8_t {
  k_ra8_adc_test_group_one  = 1U,   /**< RA8 ADC test group one.  */
  k_ra8_adc_test_group_oor  = 9U,   /**< RA8 ADC test group oor.  */
  k_ra8_adc_test_group_huge = 200U, /**< RA8 ADC test group huge. */
  k_ra8_adc_test_cmp_table  = 3U,   /**< RA8 ADC test cmp table.  */
  k_ra8_adc_test_cmp_oor    = 8U,   /**< RA8 ADC test cmp oor.    */
} ra8_adc_test_group_t;

typedef enum : uint16_t {
  k_ra8_adc_test_window_low  = 0x0100U, /**< RA8 ADC test window low.  */
  k_ra8_adc_test_window_high = 0x0FFFU, /**< RA8 ADC test window high. */
} ra8_adc_test_window_t;

typedef enum : uint32_t {
  k_ra8_adc_test_admd0_disabled = 0x00000000UL, /**< RA8 ADC test admd0 disabled. */
} ra8_adc_test_admd_t;

static ra8_adc_scan_group_cfg_t make_scan_cfg(void)
{
  ra8_adc_scan_group_cfg_t cfg = {};
  cfg.num_channels             = 3U;
  cfg.channels[0]              = 4U;
  cfg.channels[1]              = k_adc_second_channel;
  cfg.channels[2]              = 6U;
  cfg.trigger                  = k_ra8_adc_trig_src_software;
  cfg.priority                 = k_ra8_adc_priority_high;
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  const ra8_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &cfg));

  /* ADSGER must have group-1 bit set in addition to the default group-0 bit. */
  const uint32_t expected = (1UL << k_ra8_adc_test_group_one) | 1UL;
  TEST_ASSERT((*ra8_adc_b_adsger() & expected) == expected);

  /* Each listed ADCHCR slot must encode SGSEL == 1 and CNVCS == ch. */
  for (uint8_t i = 0U; i < cfg.num_channels; ++i) {
    const uint32_t chcr = *ra8_adc_b_adchcr(cfg.channels[i]);
    const uint32_t sg   = (chcr & k_ra8_adchcr_mask_sgsel) >> (uint32_t)k_ra8_adchcr_bit_sgsel;
    const uint32_t cs   = (chcr & k_ra8_adchcr_mask_cnvcs) >> (uint32_t)k_ra8_adchcr_bit_cnvcs;
    TEST_ASSERT_EQ(k_ra8_adc_test_group_one, sg);
    TEST_ASSERT_EQ(cfg.channels[i], cs);
  }
  /* Software trigger -> ADTRGENR group-1 bit is clear. */
  TEST_ASSERT((*ra8_adc_b_adtrgenr() & (1UL << k_ra8_adc_test_group_one)) == 0U);
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  ra8_adc_scan_group_cfg_t cfg = make_scan_cfg();
  cfg.trigger                  = k_ra8_adc_trig_src_elc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &cfg));
  TEST_ASSERT((*ra8_adc_b_adtrgenr() & (1UL << k_ra8_adc_test_group_one)) != 0U);
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
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_configure_scan_group(0U, nullptr));
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
  ra8_fake_mmap_reset();
  const ra8_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group(k_ra8_adc_test_group_oor, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group(k_ra8_adc_test_group_huge, &cfg));

  ra8_adc_scan_group_cfg_t bad_cfg = make_scan_cfg();
  bad_cfg.num_channels             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_configure_scan_group(0U, &bad_cfg));

  bad_cfg             = make_scan_cfg();
  bad_cfg.channels[0] = (uint8_t)k_ra8_adc_b_max_channels; /* OOR. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_configure_scan_group(0U, &bad_cfg));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_start_group(k_ra8_adc_test_group_one));
  TEST_ASSERT((*ra8_adc_b_adstr(k_ra8_adc_test_group_one) & k_ra8_adstr_mask_adst) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_stop_group(k_ra8_adc_test_group_one));
  TEST_ASSERT((*ra8_adc_b_adstr(k_ra8_adc_test_group_one) & k_ra8_adstr_mask_adst) == 0U);
  TEST_ASSERT((*ra8_adc_b_adstopr() & (k_ra8_adstopr_mask_adstop0 | k_ra8_adstopr_mask_adstop1)) !=
              0U);

  /* Out-of-range groups are rejected on both entry points. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_start_group(k_ra8_adc_test_group_oor));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_stop_group(k_ra8_adc_test_group_huge));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  const ra8_adc_scan_group_cfg_t cfg = make_scan_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &cfg));

  /* Pre-load distinct ADDR slots so we can verify ordering. */
  *ra8_adc_b_addr(cfg.channels[0]) = k_adc_code_ch0;
  *ra8_adc_b_addr(cfg.channels[1]) = k_adc_code_ch1;
  *ra8_adc_b_addr(cfg.channels[2]) = k_adc_code_ch2;

  uint16_t buf[8] = {};
  uint8_t  count  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_read_group_results(k_ra8_adc_test_group_one, buf, &count));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  uint16_t buf   = 0U;
  uint8_t  count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_read_group_results(0U, nullptr, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_adc_read_group_results(0U, &buf, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_read_group_results(k_ra8_adc_test_group_oor, &buf, &count));
  /* Group 1 is unconfigured at this point. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_read_group_results(k_ra8_adc_test_group_one, &buf, &count));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_continuous_scan(k_ra8_adc_test_group_one, true));
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_continuous, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_set_continuous_scan(k_ra8_adc_test_group_one, false));
  TEST_ASSERT_EQ(k_ra8_adc_test_admd0_one_cycle, (*ra8_adc_b_admdr() & k_ra8_admdr_mask_admd0));

  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_set_continuous_scan(k_ra8_adc_test_group_oor, true));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_adc_set_compare_window(k_ra8_adc_test_cmp_table,
                                            k_ra8_adc_test_window_low,
                                            k_ra8_adc_test_window_high));

  const uint32_t tbl  = *ra8_adc_b_adcmptbr(k_ra8_adc_test_cmp_table);
  const uint32_t low  = (tbl & k_ra8_adcmptbr_mask_low) >> (uint32_t)k_ra8_adcmptbr_bit_low;
  const uint32_t high = (tbl & k_ra8_adcmptbr_mask_high) >> (uint32_t)k_ra8_adcmptbr_bit_high;
  TEST_ASSERT_EQ(k_ra8_adc_test_window_low, low);
  TEST_ASSERT_EQ(k_ra8_adc_test_window_high, high);
  TEST_ASSERT((*ra8_adc_b_adcmpenr() & (1UL << k_ra8_adc_test_cmp_table)) != 0U);

  /* CMPMD field for table 3 lives in ADCMPMDR0 at bit (3 * 8) = 24. */
  const uint32_t mdr0  = *ra8_adc_b_adcmpmdr(0U);
  const uint32_t shift = (uint32_t)k_ra8_adc_test_cmp_table * (uint32_t)k_ra8_adcmpmd_field_step;
  TEST_ASSERT_EQ(k_ra8_adcmpmd_inside, ((mdr0 >> shift) & 0x3UL));

  /* Per-channel ADDOPCRB CMPTBLEm bit is set for our table. */
  const uint32_t opcrb = *ra8_adc_b_addopcrb(k_ra8_adc_test_cmp_table);
  TEST_ASSERT((opcrb & k_ra8_addopcrb_mask_cmptblem) != 0U);
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_adc_set_compare_window(0U, k_ra8_adc_test_window_high, k_ra8_adc_test_window_low));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_set_compare_window(k_ra8_adc_test_cmp_oor,
                                            k_ra8_adc_test_window_low,
                                            k_ra8_adc_test_window_high));
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
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_adc_set_oversampling(k_ra8_adc_test_ch_valid, k_ra8_adc_oversample_16x));
  const uint32_t opcrb = *ra8_adc_b_addopcrb(k_ra8_adc_test_ch_valid);
  TEST_ASSERT_EQ(k_ra8_avemd_average,
                 ((opcrb & k_ra8_addopcrb_mask_avemd) >> (uint32_t)k_ra8_addopcrb_bit_avemd));
  TEST_ASSERT_EQ(k_ra8_adc_avg_16x,
                 ((opcrb & k_ra8_addopcrb_mask_adc) >> (uint32_t)k_ra8_addopcrb_bit_adc));

  /* Switching back to off must clear AVEMD. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_adc_set_oversampling(k_ra8_adc_test_ch_valid, k_ra8_adc_oversample_off));
  const uint32_t opcrb_after = *ra8_adc_b_addopcrb(k_ra8_adc_test_ch_valid);
  TEST_ASSERT_EQ(k_ra8_avemd_off, (opcrb_after & k_ra8_addopcrb_mask_avemd));
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
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_adc_set_oversampling(0U, (ra8_adc_oversample_t)0xFFU));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_set_oversampling(k_ra8_adc_test_ch_oor, k_ra8_adc_oversample_4x));
  TEST_END("adc oversampling: rejects bad mode and oor channel");
}

/**
 * @test test_mcdc_adc
 *
 * @par MC/DC:
 * Two 2-condition decisions inside ``adc.c``.  Each uses an N+1 = 3
 * vector subset (DO-178C 6.4.4.3 representative-subset MC/DC).
 *
 * Decision A: ``ra8_adc_read_channel`` line 311
 * (libs/ra8_hal/src/adc.c): ``if ((chcr == nullptr) || (addr == nullptr))``.
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
 *      (cfg->num_channels > k_ra8_adc_scan_group_max_channels))``.
 * - V1: num_channels=3 -> C1=F, C2=F -> dec F (cfg accepted)
 * - V2: num_channels=0 -> C1=T (short-circuits) -> dec T
 * - V3: num_channels=9 (> 8 max) -> C1=F, C2=T -> dec T
 * V1+V3 vary C2 only; V1+V2 vary C1 only.
 */
static void test_mcdc_adc(void)
{
  TEST_BEGIN("adc MC/DC: read_channel OR + validate_group_cfg OR");

  /* --- Decision A: ra8_adc_read_channel line 311 ------------------ */
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  uint16_t raw = 0U;
  /* V1: channel=5 (both pointers valid) -> dec F.  ADACT0 reads idle
   * after reset, so read_channel completes without spinning. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_read_channel(k_ra8_adc_test_ch_valid, &raw));
  /* V2: channel=23 (chcr!=NULL, addr==NULL) -> C2 alone forces dec T. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel(23U, &raw));
  /* V3: channel=24 (chcr==NULL short-circuit) -> C1 alone forces dec T. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel(k_ra8_adc_test_ch_oor, &raw));

  /* --- Decision B: internal_validate_group_cfg line 612 ---------- */
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  ra8_adc_scan_group_cfg_t good = make_scan_cfg();
  /* V1: num_channels=3 -> dec F, accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &good));
  /* V2: num_channels=0 -> C1=T short-circuit -> dec T -> out_of_range. */
  ra8_adc_scan_group_cfg_t zero = make_scan_cfg();
  zero.num_channels             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &zero));
  /* V3: num_channels=9 (> max=8) -> C1=F, C2=T -> dec T -> out_of_range.
   * num_channels is uint8_t, channels[] has 8 slots, but the validator
   * rejects on the count alone before walking the channel array. */
  ra8_adc_scan_group_cfg_t big = make_scan_cfg();
  big.num_channels             = (uint8_t)(k_ra8_adc_scan_group_max_channels + 1U);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group(k_ra8_adc_test_group_one, &big));

  TEST_END("adc MC/DC: read_channel OR + validate_group_cfg OR");
}

/**
 * @brief Exercise the out-of-range and branch-select legs of the header
 *        accessor guards (``ra8_adc_b_regs.h``).
 *
 * @details
 * The register-header inline accessors each guard their index argument and
 * return ``nullptr`` when it is outside the modelled register array. The
 * happy-path (in-range) legs are covered by the driver tests above; this case
 * drives the still-uncovered ``nullptr`` legs plus the ``ra8_adc_b_adcmpmdr``
 * which-selector branches so the header clears the >= 90% per-file floor under
 * gcc. Every input is a compile-time constant, so the case is deterministic
 * and needs no interrupt or injected stimulus.
 *
 * @par MC/DC:
 * Every accessor guard touched here is a single-condition decision (e.g.
 * ``if (n >= k_ra8_adc_b_ext_data_regs)``), so N+1 reduces to the two-outcome
 * pair: the FALSE (in-range) leg is exercised by the driver tests above, and
 * this case supplies the TRUE (out-of-range -> nullptr) leg. ``ra8_adc_b_adcmpmdr``
 * is a two-way selector (which==0 / which==1 / else) whose three legs are all
 * driven here (sel0 valid above, sel1 + else here). No ``&&`` / ``||`` compound
 * decisions are present in the code under test.
 */
static void test_accessor_edge_pointers(void)
{
  TEST_BEGIN("adc header accessors: out-of-range guard legs");
  ra8_fake_mmap_reset();

  /* ra8_adc_b_adcmpmdr: which==1 selects ADCMPMDR1 (valid, live window). */
  volatile uint32_t* mdr0 = ra8_adc_b_adcmpmdr(k_ra8_adc_test_cmpmdr_sel0);
  volatile uint32_t* mdr1 = ra8_adc_b_adcmpmdr(k_ra8_adc_test_cmpmdr_sel1);
  TEST_ASSERT_NOT_NULL(mdr0);
  TEST_ASSERT_NOT_NULL(mdr1);
  /* ADCMPMDR1 sits one 32-bit register (offset delta) above ADCMPMDR0. */
  TEST_ASSERT_EQ((uintptr_t)k_ra8_adc_b_off_adcmpmdr1 - (uintptr_t)k_ra8_adc_b_off_adcmpmdr0,
                 (uintptr_t)mdr1 - (uintptr_t)mdr0);
  *mdr1 = (uint32_t)k_ra8_adc_test_mdr1_sentinel;
  TEST_ASSERT_EQ(k_ra8_adc_test_mdr1_sentinel, *mdr1);
  /* which != 0,1 -> nullptr. */
  TEST_ASSERT_NULL(ra8_adc_b_adcmpmdr(k_ra8_adc_test_cmpmdr_oor));

  /* Remaining index guards: one-past-the-end argument -> nullptr leg. */
  TEST_ASSERT_NULL(ra8_adc_b_adcmptbr(k_ra8_adc_test_cmptbr_oor));
  TEST_ASSERT_NULL(ra8_adc_b_adsgdcr(k_ra8_adc_test_sgdcr_oor));
  TEST_ASSERT_NULL(ra8_adc_b_addopcrc(k_ra8_adc_test_opcrc_oor));
  TEST_ASSERT_NULL(ra8_adc_b_adexdr(k_ra8_adc_test_adexdr_oor));

  /* ra8_adc_b_adexdr_index_for_chan: CNVCS below the extended-channel base
   * returns the no-index sentinel (the >= base leg is covered by selfdiag). */
  TEST_ASSERT_EQ(k_ra8_adc_b_adexdr_no_index,
                 ra8_adc_b_adexdr_index_for_chan(k_ra8_adc_test_cnvcs_below));

  TEST_END("adc header accessors: out-of-range guard legs");
}

int main(void)
{
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
  return 0;
}
