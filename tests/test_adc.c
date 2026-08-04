/**
 * @file test_adc.c
 * @brief MC/DC unit tests for libs/ra8_hal/src/adc.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_adc_ch_in_range     = 0U,   /**< Test ADC channel in range.      */
  k_test_adc_ch_max_idx      = 23U,  /**< Test ADC channel maximum index. */
  k_test_adc_ch_oob          = 24U,  /**< Test ADC channel oob.           */
  k_test_adc_ch_huge         = 200U, /**< Test ADC channel huge.          */
  k_test_adc_group_zero      = 0U,   /**< Test ADC group zero.            */
  k_test_adc_chan_count_zero = 0U,   /**< Test ADC chan count zero.       */
  k_test_adc_chan_count_one  = 1U,   /**< Test ADC chan count one.        */
  k_test_adc_chan_count_max  = 8U,   /**< Test ADC chan count maximum.    */
  k_test_adc_chan_count_over = 9U,   /**< Test ADC chan count over.       */
} test_adc_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_setup(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
}

/**
 * @test test_mcdc_adc_read_channel_chcr_addr_null
 *
 * @par MC/DC:
 * Decision: ``if ((chcr == nullptr) || (addr == nullptr))``
 * (2 conditions, libs/ra8_hal/src/adc.c around line 311)
 * Operands coupled (both nullptr together for OOB ch). Per DO-178C
 * 6.4.4.3 representative-subset is N+1 = 3 vectors. Pair (V1,V3)
 * flips the joint decision.
 */
static void test_mcdc_adc_read_channel_chcr_addr_null(void)
{
  TEST_BEGIN("adc read_channel MC/DC: (chcr==NULL || addr==NULL)");
  test_setup();
  uint16_t raw = 0U;
  TEST_ASSERT(ra8_adc_read_channel((uint8_t)k_test_adc_ch_in_range, &raw) !=
              k_ra8_err_out_of_range);
  /* k_test_adc_ch_max_idx == 23: ADCHCR[23] exists but ADDR[23] does not
   * (silicon only provides 23 result slots, see ra8_adc_b_addr() docs).
   * So channel 23 returns out_of_range -- this exercises the second
   * operand of the OR decision in adc.c independently. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_read_channel((uint8_t)k_test_adc_ch_max_idx, &raw));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel((uint8_t)k_test_adc_ch_oob, &raw));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_adc_read_channel((uint8_t)k_test_adc_ch_huge, &raw));
  TEST_END("adc read_channel MC/DC: (chcr==NULL || addr==NULL)");
}

/**
 * @test test_mcdc_adc_validate_group_cfg_num_channels
 *
 * @par MC/DC:
 * Decision: ``if ((cfg->num_channels == 0U) ||
 *                 (cfg->num_channels > k_ra8_adc_scan_group_max_channels))``
 * (2 conditions, libs/ra8_hal/src/adc.c around line 612)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors plus boundary.
 */
static void test_mcdc_adc_validate_group_cfg_num_channels(void)
{
  TEST_BEGIN("adc validate_group_cfg MC/DC: num==0 || num>max");
  test_setup();
  ra8_adc_scan_group_cfg_t cfg = {};
  cfg.channels[0]              = (uint8_t)k_test_adc_ch_in_range;
  cfg.trigger                  = k_ra8_adc_trig_src_software;
  cfg.priority                 = k_ra8_adc_priority_low;
  cfg.num_channels             = (uint8_t)k_test_adc_chan_count_one;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group((uint8_t)k_test_adc_group_zero, &cfg));
  for (uint8_t i = 0U; i < (uint8_t)k_test_adc_chan_count_max; ++i) {
    cfg.channels[i] = (uint8_t)k_test_adc_ch_in_range;
  }
  cfg.num_channels = (uint8_t)k_test_adc_chan_count_max;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_configure_scan_group((uint8_t)k_test_adc_group_zero, &cfg));
  cfg.num_channels = (uint8_t)k_test_adc_chan_count_zero;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group((uint8_t)k_test_adc_group_zero, &cfg));
  cfg.num_channels = (uint8_t)k_test_adc_chan_count_over;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_adc_configure_scan_group((uint8_t)k_test_adc_group_zero, &cfg));
  TEST_END("adc validate_group_cfg MC/DC: num==0 || num>max");
}

int32_t main(void)
{
  test_mcdc_adc_read_channel_chcr_addr_null();
  test_mcdc_adc_validate_group_cfg_num_channels();
  (void)fprintf(stderr, "[OK ] test_adc.c\n");
  return 0;
}
