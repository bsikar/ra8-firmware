/**
 * @file test_ra8_acmphs.c
 * @brief Unit tests for ra8_acmphs.c (High-Speed Analog Comparator driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_acmphs.h"
#include "ra8_acmphs_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @enum acmphs_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint8_t {
  k_acmphs_probe_cmpctl =
    0xFFU, /**< Every CMPCTL bit set, so a configure that clears the wrong field leaves evidence in the rest. */
} acmphs_fixture_t;

typedef enum : uint8_t {
  k_ra8_acmphs_test_ch_first = 0U,   /**< RA8 acmphs test channel first. */
  k_ra8_acmphs_test_ch_mid   = 3U,   /**< RA8 acmphs test channel mid.   */
  k_ra8_acmphs_test_ch_last  = 5U,   /**< RA8 acmphs test channel last.  */
  k_ra8_acmphs_test_ch_bad   = 6U,   /**< RA8 acmphs test channel bad.   */
  k_ra8_acmphs_test_ch_way   = 200U, /**< RA8 acmphs test channel way.   */
} ra8_acmphs_test_ch_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("acmphs init happy");
  ra8_fake_mmap_reset();

  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CMPCTL = k_acmphs_probe_cmpctl;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  TEST_ASSERT_EQ(0, reg->CMPCTL);
  TEST_END("acmphs init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_first_channel(void)
{
  TEST_BEGIN("acmphs enable first channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_enable((uint8_t)k_ra8_acmphs_test_ch_first));
  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  TEST_ASSERT_EQ(k_ra8_acmphs_mask_hcen, (reg->CMPCTL & (uint8_t)k_ra8_acmphs_mask_hcen));
  TEST_END("acmphs enable first channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_mid_channel(void)
{
  TEST_BEGIN("acmphs enable mid channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_enable((uint8_t)k_ra8_acmphs_test_ch_mid));
  TEST_END("acmphs enable mid channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_last_channel(void)
{
  TEST_BEGIN("acmphs enable last channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_enable((uint8_t)k_ra8_acmphs_test_ch_last));
  TEST_END("acmphs enable last channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_bad_channel(void)
{
  TEST_BEGIN("acmphs enable bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_channel_enable((uint8_t)k_ra8_acmphs_test_ch_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_channel_enable((uint8_t)k_ra8_acmphs_test_ch_way));
  TEST_END("acmphs enable bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_output_high(void)
{
  TEST_BEGIN("acmphs read output high");
  ra8_fake_mmap_reset();

  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CMPMON = (uint8_t)k_ra8_acmphs_mask_hcmon;

  ra8_level_t level = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_read_output((uint8_t)k_ra8_acmphs_test_ch_first, &level));
  TEST_ASSERT_EQ(k_ra8_level_high, level);
  TEST_END("acmphs read output high");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_output_low(void)
{
  TEST_BEGIN("acmphs read output low");
  ra8_fake_mmap_reset();

  ra8_level_t level = k_ra8_level_high;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_read_output((uint8_t)k_ra8_acmphs_test_ch_first, &level));
  TEST_ASSERT_EQ(k_ra8_level_low, level);
  TEST_END("acmphs read output low");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_output_null_out(void)
{
  TEST_BEGIN("acmphs read output null");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_acmphs_read_output((uint8_t)k_ra8_acmphs_test_ch_first, nullptr));
  TEST_END("acmphs read output null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_output_bad_channel(void)
{
  TEST_BEGIN("acmphs read output bad channel");
  ra8_fake_mmap_reset();

  ra8_level_t level = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_read_output((uint8_t)k_ra8_acmphs_test_ch_bad, &level));
  TEST_END("acmphs read output bad channel");
}

/* ---------------------------------------------------------------------------
 * full build-out
 * 
 */

static uint32_t s_acmphs_cb_count;
static uint8_t  s_acmphs_cb_last_ch;

static void stub_acmphs_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_acmphs_cb_count;
  s_acmphs_cb_last_ch = ch;
}

static void prep_w42(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_acmphs_cb_count   = 0U;
  s_acmphs_cb_last_ch = 0U;
}

static ra8_acmphs_cfg_t make_cfg(void)
{
  const ra8_acmphs_cfg_t cfg = {
    .ivpsel     = 0x01U,
    .ivrefsel   = 0x02U,
    .edge       = k_ra8_acmphs_edge_rise,
    .filter_en  = true,
    .invert_out = false,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_init_configured(void)
{
  TEST_BEGIN("acmphs channel_init configured");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, &cfg));

  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT((reg->CMPCTL & (uint8_t)k_ra8_acmphs_mask_hcen) != 0U);
  TEST_ASSERT_EQ(0x01U, reg->CMPSEL0);
  TEST_ASSERT_EQ(0x02U, reg->CMPSEL1);
  /* filter_en now maps to CMPCTL.CDFS[1:0] instead of a separate CMPFIR. */
  TEST_ASSERT((reg->CMPCTL & (uint8_t)k_ra8_acmphs_mask_cdfs) != 0U);
  TEST_END("acmphs channel_init configured");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_init_null_cfg(void)
{
  TEST_BEGIN("acmphs channel_init null cfg");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, nullptr));
  TEST_END("acmphs channel_init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_init_bad_channel(void)
{
  TEST_BEGIN("acmphs channel_init bad channel");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_bad, &cfg));
  TEST_END("acmphs channel_init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_init_no_filter_with_invert(void)
{
  TEST_BEGIN("acmphs channel_init no filter + invert");
  prep_w42();

  ra8_acmphs_cfg_t cfg = make_cfg();
  cfg.filter_en        = false;
  cfg.invert_out       = true;
  cfg.edge             = k_ra8_acmphs_edge_fall;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, &cfg));

  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  /* filter off -> CDFS[1:0] = 0. Invert -> CINV bit 0 set. */
  TEST_ASSERT((reg->CMPCTL & (uint8_t)k_ra8_acmphs_mask_cdfs) == 0U);
  TEST_ASSERT((reg->CMPCTL & (uint8_t)k_ra8_acmphs_mask_cinv) != 0U);
  TEST_END("acmphs channel_init no filter + invert");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_init_last_channel_no_mstp(void)
{
  TEST_BEGIN("acmphs channel_init last channel (no MSTP)");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_last, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_deinit((uint8_t)k_ra8_acmphs_test_ch_last));
  TEST_END("acmphs channel_init last channel (no MSTP)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_bad_channel(void)
{
  TEST_BEGIN("acmphs clear_status bad channel");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_acmphs_clear_status((uint8_t)k_ra8_acmphs_test_ch_bad));
  TEST_END("acmphs clear_status bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_deinit(void)
{
  TEST_BEGIN("acmphs channel_deinit");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_deinit((uint8_t)k_ra8_acmphs_test_ch_first));
  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_first);
  TEST_ASSERT_EQ(0, reg->CMPCTL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_channel_deinit((uint8_t)k_ra8_acmphs_test_ch_bad));
  TEST_END("acmphs channel_deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_inputs(void)
{
  TEST_BEGIN("acmphs set_inputs");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_set_inputs((uint8_t)k_ra8_acmphs_test_ch_mid, 0xAAU, 0x55U));

  volatile r_acmphs_regs_t* reg = ra8_acmphs((uint8_t)k_ra8_acmphs_test_ch_mid);
  TEST_ASSERT_EQ(0xAAU, reg->CMPSEL0);
  TEST_ASSERT_EQ(0x55U, reg->CMPSEL1);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_set_inputs((uint8_t)k_ra8_acmphs_test_ch_bad, 0U, 0U));
  TEST_END("acmphs set_inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("acmphs status read + clear");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, &cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_get_status((uint8_t)k_ra8_acmphs_test_ch_first, &mask));
  TEST_ASSERT((mask & (uint8_t)k_ra8_acmphs_mask_hcen) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_clear_status((uint8_t)k_ra8_acmphs_test_ch_first));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_get_status((uint8_t)k_ra8_acmphs_test_ch_first, &mask));
  TEST_ASSERT_EQ(0, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_acmphs_get_status((uint8_t)k_ra8_acmphs_test_ch_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_acmphs_get_status((uint8_t)k_ra8_acmphs_test_ch_bad, &mask));
  TEST_END("acmphs status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("acmphs attach + dispatch");
  prep_w42();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_attach_handler(stub_acmphs_cb, (void*)(uintptr_t)0xAABBU));

  ra8_acmphs_dispatch((uint8_t)k_ra8_acmphs_test_ch_mid);
  TEST_ASSERT_EQ(1, s_acmphs_cb_count);
  TEST_ASSERT_EQ(k_ra8_acmphs_test_ch_mid, s_acmphs_cb_last_ch);

  ra8_acmphs_dispatch((uint8_t)k_ra8_acmphs_test_ch_bad);
  TEST_ASSERT_EQ(1, s_acmphs_cb_count);
  TEST_END("acmphs attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("acmphs power transition");
  prep_w42();

  const ra8_acmphs_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_channel_init((uint8_t)k_ra8_acmphs_test_ch_first, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_enter_stop((uint8_t)k_ra8_acmphs_test_ch_first));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_exit_stop((uint8_t)k_ra8_acmphs_test_ch_first));

  /* Channels >= 4 have no MSTP id, enter/exit is a no-op and always succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_enter_stop((uint8_t)k_ra8_acmphs_test_ch_last));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_acmphs_exit_stop((uint8_t)k_ra8_acmphs_test_ch_last));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_acmphs_enter_stop((uint8_t)k_ra8_acmphs_test_ch_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_acmphs_exit_stop((uint8_t)k_ra8_acmphs_test_ch_bad));
  TEST_END("acmphs power transition");
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
  test_enable_first_channel,
  test_enable_mid_channel,
  test_enable_last_channel,
  test_enable_bad_channel,
  test_read_output_high,
  test_read_output_low,
  test_read_output_null_out,
  test_read_output_bad_channel,
  test_channel_init_configured,
  test_channel_init_null_cfg,
  test_channel_init_bad_channel,
  test_channel_init_no_filter_with_invert,
  test_channel_init_last_channel_no_mstp,
  test_channel_deinit,
  test_clear_status_bad_channel,
  test_set_inputs,
  test_status_read_and_clear,
  test_attach_and_dispatch,
  test_power_transition,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_acmphs.c\n");
  return 0;
}
