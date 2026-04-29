/**
 * @file test_ra_ctsu.c
 * @brief Unit tests for ra_ctsu.c (Capacitive Touch Sensing Unit driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ctsu_regs.h"
#include "ra_ctsu.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_ra_ctsu_test_period       = 0x12U,
  k_ra_ctsu_test_stab         = 0x34U,
  k_ra_ctsu_test_offset       = 0x0080U,
  k_ra_ctsu_test_threshold    = 0x0100U,
  k_ra_ctsu_test_count_seed   = 0x0123U,
  k_ra_ctsu_test_count_high   = 0x4567U,
  k_ra_ctsu_test_calib_seed_a = 100U,
  k_ra_ctsu_test_calib_seed_b = 200U,
  k_ra_ctsu_test_calib_seed_c = 300U,
  k_ra_ctsu_test_calib_avg    = 200U, /**< (100 + 200 + 300) / 3. */
} ra_ctsu_test_value_t;

typedef enum : uint8_t {
  k_ra_ctsu_test_elec_low  = 0U,
  k_ra_ctsu_test_elec_mid  = 5U,
  k_ra_ctsu_test_elec_high = 35U,
  k_ra_ctsu_test_elec_bad  = 36U,
} ra_ctsu_test_electrode_t;

static uint32_t s_cb_count;
static uint8_t  s_cb_last_electrode;
static ra_err_t s_cb_last_err;
static void*    s_cb_last_ctx;

static void stub_ctsu_cb(void* ctx, uint8_t electrode, ra_err_t err)
{
  s_cb_count += 1U;
  s_cb_last_electrode = electrode;
  s_cb_last_err       = err;
  s_cb_last_ctx       = ctx;
}

static void reset_cb(void)
{
  s_cb_count          = 0U;
  s_cb_last_electrode = 0U;
  s_cb_last_err       = k_ra_ok;
  s_cb_last_ctx       = nullptr;
}

static ra_ctsu_cfg_t make_default_cfg(void)
{
  ra_ctsu_cfg_t cfg  = {};
  cfg.mode           = k_ra_ctsu_mode_self_multi;
  cfg.clock_div      = k_ra_ctsu_clock_div_4;
  cfg.scan_period    = (uint8_t)k_ra_ctsu_test_period;
  cfg.stabilise_wait = (uint8_t)k_ra_ctsu_test_stab;
  cfg.electrode_mask = 0x0000000FFFFFFFFFULL;
  cfg.transmit_mask  = 0U;
  cfg.offset_initial = (uint16_t)k_ra_ctsu_test_offset;
  return cfg;
}

static void test_open_null_cfg(void)
{
  TEST_BEGIN("ctsu open null cfg");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_ctsu_open(nullptr));
  TEST_END("ctsu open null cfg");
}

static void test_open_zero_mask(void)
{
  TEST_BEGIN("ctsu open zero mask rejected");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg  = make_default_cfg();
  cfg.electrode_mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_ctsu_open(&cfg));
  TEST_END("ctsu open zero mask rejected");
}

static void test_open_happy(void)
{
  TEST_BEGIN("ctsu open happy");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_period, (int)reg->CTSUSDPRS);
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_stab, (int)reg->CTSUSST);
  TEST_ASSERT_EQ((int)k_ra_ctsu_clock_div_4, (int)reg->CTSUDCLKC);
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_offset, (int)reg->CTSUSO0);
  TEST_ASSERT_EQ(0xFF, (int)reg->CTSUCHAC0);
  /* electrode_mask is 36 bits wide, so CHAC4 (byte 4) only has the
   * low nibble populated (channels 32..35).
   */
  TEST_ASSERT_EQ(0x0F, (int)reg->CTSUCHAC4);
  /* PON should be set in CTSUCR0. */
  TEST_ASSERT_EQ((int)k_ra_ctsu_cr0_mask_pon,
                 (int)(reg->CTSUCR0 & (uint8_t)k_ra_ctsu_cr0_mask_pon));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu open happy");
}

static void test_close_powers_down(void)
{
  TEST_BEGIN("ctsu close powers down");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  TEST_ASSERT_EQ(0, (int)reg->CTSUCR0);

  uint32_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_not_initialized,
                 (int)ra_ctsu_get_data((uint8_t)k_ra_ctsu_test_elec_mid, &raw));
  TEST_END("ctsu close powers down");
}

static void test_scan_start_stop(void)
{
  TEST_BEGIN("ctsu scan start/stop");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_scan_start());
  TEST_ASSERT_EQ((int)k_ra_ctsu_cr0_mask_strt,
                 (int)(reg->CTSUCR0 & (uint8_t)k_ra_ctsu_cr0_mask_strt));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_scan_stop());
  TEST_ASSERT_EQ(0, (int)(reg->CTSUCR0 & (uint8_t)k_ra_ctsu_cr0_mask_strt));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu scan start/stop");
}

static void test_scan_start_not_init(void)
{
  TEST_BEGIN("ctsu scan_start not init");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close()); /* normalise state */
  TEST_ASSERT_EQ((int)k_ra_err_not_initialized, (int)ra_ctsu_scan_start());
  TEST_ASSERT_EQ((int)k_ra_err_not_initialized, (int)ra_ctsu_scan_stop());
  TEST_END("ctsu scan_start not init");
}

static void test_get_data_null(void)
{
  TEST_BEGIN("ctsu get_data null");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_ctsu_get_data((uint8_t)k_ra_ctsu_test_elec_low, nullptr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu get_data null");
}

static void test_get_data_happy(void)
{
  TEST_BEGIN("ctsu get_data happy");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  reg->CTSUSC0                = (uint16_t)k_ra_ctsu_test_count_seed;
  reg->CTSUSC1                = (uint16_t)k_ra_ctsu_test_count_high;

  uint32_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_get_data((uint8_t)k_ra_ctsu_test_elec_mid, &raw));
  const uint32_t expected =
    (uint32_t)k_ra_ctsu_test_count_seed | ((uint32_t)k_ra_ctsu_test_count_high << 16U);
  TEST_ASSERT_EQ((int)expected, (int)raw);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu get_data happy");
}

static void test_get_data_bad_electrode(void)
{
  TEST_BEGIN("ctsu get_data bad electrode");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));
  uint32_t raw = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_ctsu_get_data((uint8_t)k_ra_ctsu_test_elec_bad, &raw));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu get_data bad electrode");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("ctsu attach + dispatch");
  ra_sim_mmap_reset();
  reset_cb();
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ctsu_attach_handler(stub_ctsu_cb, (void*)(uintptr_t)0xC0DEU));

  ra_ctsu_dispatch((uint8_t)k_ra_ctsu_test_elec_mid, k_ra_ok);
  TEST_ASSERT_EQ(1, (int)s_cb_count);
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_elec_mid, (int)s_cb_last_electrode);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)s_cb_last_err);
  TEST_ASSERT_EQ((int)0xC0DE, (int)(uintptr_t)s_cb_last_ctx);

  /* Out-of-range index should be silently dropped. */
  ra_ctsu_dispatch((uint8_t)k_ra_ctsu_test_elec_bad, k_ra_ok);
  TEST_ASSERT_EQ(1, (int)s_cb_count);

  /* Detach. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_attach_handler(nullptr, nullptr));
  ra_ctsu_dispatch((uint8_t)k_ra_ctsu_test_elec_low, k_ra_ok);
  TEST_ASSERT_EQ(1, (int)s_cb_count);
  TEST_END("ctsu attach + dispatch");
}

static void test_calibrate_averages_three(void)
{
  TEST_BEGIN("ctsu calibrate averages 3 dummy scans");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));

  /* Seed CTSUSC0 with a constant -- the simulator does not advance
   * silicon counters, so each "dummy scan" reads back the same value.
   */
  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  reg->CTSUSC0                = (uint16_t)k_ra_ctsu_test_calib_seed_b;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_calibrate((uint8_t)k_ra_ctsu_test_elec_mid));

  /* Final CTSUSO0 should equal the average (3x same value). */
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_calib_seed_b, (int)reg->CTSUSO0);
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_elec_mid, (int)reg->CTSUMCH0);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu calibrate averages 3 dummy scans");
}

static void test_calibrate_bad_electrode(void)
{
  TEST_BEGIN("ctsu calibrate bad electrode");
  ra_sim_mmap_reset();
  ra_ctsu_cfg_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_ctsu_calibrate((uint8_t)k_ra_ctsu_test_elec_bad));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_END("ctsu calibrate bad electrode");
}

static void test_calibrate_not_init(void)
{
  TEST_BEGIN("ctsu calibrate not init");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_close());
  TEST_ASSERT_EQ((int)k_ra_err_not_initialized,
                 (int)ra_ctsu_calibrate((uint8_t)k_ra_ctsu_test_elec_mid));
  TEST_END("ctsu calibrate not init");
}

static void test_threshold_storage(void)
{
  TEST_BEGIN("ctsu threshold storage");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ctsu_set_threshold((uint8_t)k_ra_ctsu_test_elec_high,
                                            (uint16_t)k_ra_ctsu_test_threshold));
  uint16_t out = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ctsu_get_threshold((uint8_t)k_ra_ctsu_test_elec_high, &out));
  TEST_ASSERT_EQ((int)k_ra_ctsu_test_threshold, (int)out);

  /* Bad electrode. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_ctsu_set_threshold((uint8_t)k_ra_ctsu_test_elec_bad, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_ctsu_get_threshold((uint8_t)k_ra_ctsu_test_elec_bad, &out));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_ctsu_get_threshold((uint8_t)k_ra_ctsu_test_elec_low, nullptr));
  TEST_END("ctsu threshold storage");
}

int32_t main(void)
{
  test_open_null_cfg();
  test_open_zero_mask();
  test_open_happy();
  test_close_powers_down();
  test_scan_start_stop();
  test_scan_start_not_init();
  test_get_data_null();
  test_get_data_happy();
  test_get_data_bad_electrode();
  test_attach_and_dispatch();
  test_calibrate_averages_three();
  test_calibrate_bad_electrode();
  test_calibrate_not_init();
  test_threshold_storage();
  (void)fprintf(stderr, "[OK  ] test_ra_ctsu.c\n");
  return 0;
}
