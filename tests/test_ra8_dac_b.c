/**
 * @file test_ra8_dac_b.c
 * @brief Unit tests for ra8_dac_b.c (12-bit DAC_B driver)
 * @details Exercises DAC channel validation, conversion data writes, enable sequencing, and module-stop control in fake registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dac_b.h"
#include "ra8_dac_b_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum dac_b_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_dac_probe_ch0 = 0xAAAAU, /**< Data value planted in channel 0's DADR.                    */
  k_dac_probe_ch1 = 0xBBBBU, /**< In channel 1's, so a read of the wrong channel is visible. */
} dac_b_fixture_t;

/**
 * @enum dac_b_fixture2_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint32_t {
  k_dac_dacr0_all_ones =
    0xFFFFFFFFUL, /**< Every DACR0 bit set, so a wrong-field clear leaves evidence in the rest. */
} dac_b_fixture2_t;

typedef enum : uint8_t {
  k_ra8_dac_b_test_ch_0   = 0U,   /**< RA8 DAC b test channel 0.   */
  k_ra8_dac_b_test_ch_1   = 1U,   /**< RA8 DAC b test channel 1.   */
  k_ra8_dac_b_test_ch_bad = 2U,   /**< RA8 DAC b test channel bad. */
  k_ra8_dac_b_test_ch_way = 200U, /**< RA8 DAC b test channel way. */
} ra8_dac_b_test_ch_t;

typedef enum : uint16_t {
  k_ra8_dac_b_test_mid  = 0x0800U, /**< RA8 DAC b test mid.     */
  k_ra8_dac_b_test_over = 0xFFFFU, /**< RA8 DAC b test over.    */
  k_ra8_dac_b_test_max  = 0x0FFFU, /**< RA8 DAC b test maximum. */
} ra8_dac_b_test_value_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_regs(void)
{
  TEST_BEGIN("dac_b init clears both instances");
  ra8_fake_mmap_reset();

  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);
  reg0->DACR0                   = k_dac_dacr0_all_ones;
  reg0->DADR                    = k_dac_probe_ch0;
  reg1->DACR0                   = k_dac_dacr0_all_ones;
  reg1->DADR                    = k_dac_probe_ch1;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init());
  TEST_ASSERT_EQ(0, reg0->DACR0);
  TEST_ASSERT_EQ(0, reg0->DADR);
  TEST_ASSERT_EQ(0, reg1->DACR0);
  TEST_ASSERT_EQ(0, reg1->DADR);
  TEST_END("dac_b init clears both instances");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_channel_0(void)
{
  TEST_BEGIN("dac_b write channel 0");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init());
  /* Pre-arm channel 0 (FSP order: Open clears DACR0; Start sets DACEN). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(0U, true));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dac_b_write((uint8_t)k_ra8_dac_b_test_ch_0, (uint16_t)k_ra8_dac_b_test_mid));

  volatile r_dac_b_regs_t* reg = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  TEST_ASSERT_EQ(k_ra8_dac_b_test_mid, reg->DADR);
  /* FSP write writes ONLY DADR; it never touches DACR0. */
  TEST_ASSERT((reg->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_ASSERT((reg->DACR0 & (uint32_t)k_ra8_dacr0_mask_dae) == 0U);
  TEST_END("dac_b write channel 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_channel_1(void)
{
  TEST_BEGIN("dac_b write channel 1");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(1U, true));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dac_b_write((uint8_t)k_ra8_dac_b_test_ch_1, (uint16_t)k_ra8_dac_b_test_mid));

  volatile r_dac_b_regs_t* reg = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);
  TEST_ASSERT_EQ(k_ra8_dac_b_test_mid, reg->DADR);
  TEST_ASSERT((reg->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_END("dac_b write channel 1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_clamps_over_range(void)
{
  TEST_BEGIN("dac_b write clamps over-range");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dac_b_write((uint8_t)k_ra8_dac_b_test_ch_0, (uint16_t)k_ra8_dac_b_test_over));

  volatile r_dac_b_regs_t* reg = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  TEST_ASSERT_EQ(k_ra8_dac_b_test_max, reg->DADR);
  TEST_END("dac_b write clamps over-range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_bad_channel(void)
{
  TEST_BEGIN("dac_b write bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dac_b_write((uint8_t)k_ra8_dac_b_test_ch_bad, (uint16_t)k_ra8_dac_b_test_mid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dac_b_write((uint8_t)k_ra8_dac_b_test_ch_way, (uint16_t)k_ra8_dac_b_test_mid));
  TEST_END("dac_b write bad channel");
}

static uint32_t s_dac_cb_count;
static uint8_t  s_dac_cb_last_ch;

static void stub_dac_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_dac_cb_count;
  s_dac_cb_last_ch = ch;
}

static void prep_w42(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_dac_cb_count   = 0U;
  s_dac_cb_last_ch = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_configured(void)
{
  TEST_BEGIN("dac_b init configured: both channels, vref low (OFSSEL=1)");
  prep_w42();

  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_low,
    .data_format             = k_ra8_dac_b_format_left,
    .internal_output_enabled = true,
    .enable_channel0         = true,
    .enable_channel1         = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));

  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);

  /* OFSSEL is bit 8 of DACR2 -- writing vref_low (1) shifts to 0x100. */
  TEST_ASSERT_EQ(k_ra8_dacr2_mask_ofssel, (reg0->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel));
  TEST_ASSERT_EQ(k_ra8_dacr2_mask_ofssel, (reg1->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel));
  /* DPSEL is bit 16 of DACR1 -- writing format_left (1) shifts to 0x10000. */
  TEST_ASSERT_EQ(k_ra8_dacr1_mask_dpsel, (reg0->DACR1 & (uint32_t)k_ra8_dacr1_mask_dpsel));
  TEST_ASSERT_EQ(k_ra8_dacr1_mask_dpsel, (reg1->DACR1 & (uint32_t)k_ra8_dacr1_mask_dpsel));
  /* internal_output_enabled=true means DAOUTDIS bit must be CLEAR. */
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_daoutdis) == 0U);
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_daoutdis) == 0U);
  /* Both channels enabled. */
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_END("dac_b init configured: both channels, vref low (OFSSEL=1)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null(void)
{
  TEST_BEGIN("dac_b init null");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dac_b_init_configured(nullptr));
  TEST_END("dac_b init null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("dac_b deinit");
  prep_w42();

  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_normal,
    .data_format             = k_ra8_dac_b_format_right,
    .internal_output_enabled = true,
    .enable_channel0         = true,
    .enable_channel1         = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_deinit());
  /* FSP R_DAC_B_Close: DACR0 = DAOUTDIS_Msk (Hi-Z output, DACEN cleared). */
  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);
  TEST_ASSERT_EQ(k_ra8_dacr0_mask_daoutdis, reg0->DACR0);
  TEST_ASSERT_EQ(k_ra8_dacr0_mask_daoutdis, reg1->DACR0);
  TEST_END("dac_b deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_vref(void)
{
  TEST_BEGIN("dac_b set_vref");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_vref(k_ra8_dac_b_vref_low));
  /* OFSSEL bit must be set after set_vref(low). */
  TEST_ASSERT(
    (ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0)->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel) != 0U);
  TEST_ASSERT(
    (ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1)->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_vref(k_ra8_dac_b_vref_normal));
  TEST_ASSERT(
    (ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0)->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel) == 0U);
  TEST_ASSERT(
    (ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1)->DACR2 & (uint32_t)k_ra8_dacr2_mask_ofssel) == 0U);
  TEST_END("dac_b set_vref");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_output_enable_toggle(void)
{
  TEST_BEGIN("dac_b output_enable toggle");
  prep_w42();

  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(0U, true));
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(0U, false));
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) == 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(1U, true));
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_set_output_enable(1U, false));
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) == 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dac_b_set_output_enable(9U, true));
  TEST_END("dac_b output_enable toggle");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_configured_both_disabled(void)
{
  TEST_BEGIN("dac_b init both channels disabled, external output");
  prep_w42();

  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_normal,
    .data_format             = k_ra8_dac_b_format_right,
    .internal_output_enabled = false,
    .enable_channel0         = false,
    .enable_channel1         = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init_configured(&cfg));
  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_1);
  /* internal_output_enabled=false -> DAOUTDIS bit set, DACEN clear. */
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_daoutdis) != 0U);
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_daoutdis) != 0U);
  TEST_ASSERT((reg0->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) == 0U);
  TEST_ASSERT((reg1->DACR0 & (uint32_t)k_ra8_dacr0_mask_dacen) == 0U);
  TEST_END("dac_b init both channels disabled, external output");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("dac_b status read + clear");
  prep_w42();

  volatile r_dac_b_regs_t* reg0 = ra8_dac_b((uint8_t)k_ra8_dac_b_test_ch_0);
  reg0->DACR0                   = (uint32_t)k_ra8_dacr0_mask_dacen;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_get_status(&mask));
  TEST_ASSERT_EQ(0x01, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_clear_status());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_get_status(&mask));
  TEST_ASSERT_EQ(0, mask);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dac_b_get_status(nullptr));
  TEST_END("dac_b status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("dac_b attach + dispatch");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_attach_handler(stub_dac_cb, (void*)(uintptr_t)0xFEEDU));
  ra8_dac_b_dispatch_update((uint8_t)k_ra8_dac_b_test_ch_1);
  TEST_ASSERT_EQ(1, s_dac_cb_count);
  TEST_ASSERT_EQ(k_ra8_dac_b_test_ch_1, s_dac_cb_last_ch);

  ra8_dac_b_dispatch_update((uint8_t)k_ra8_dac_b_test_ch_bad);
  TEST_ASSERT_EQ(1, s_dac_cb_count);
  TEST_END("dac_b attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("dac_b power transition");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dac_b_exit_stop());
  TEST_END("dac_b power transition");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_write_channel_0();
  test_write_channel_1();
  test_write_clamps_over_range();
  test_write_bad_channel();
  test_init_configured();
  test_init_configured_both_disabled();
  test_init_null();
  test_deinit();
  test_set_vref();
  test_output_enable_toggle();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  return 0;
}
