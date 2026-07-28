/**
 * @file test_ra8_ulpt.c
 * @brief Unit tests for ra8_ulpt.c (Ultra-Low-Power Timer driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_ulpt.h"
#include "ra8_ulpt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_ulpt_t
 * @brief Control-register pattern staged before the driver writes it.
 */
typedef enum : uint8_t {
  k_t_ulptcr_all_ones = 0xFFU, /**< Every bit set, so the driver's clearing of
                                    the reserved bits is visible.               */
} t_ulpt_t;

typedef enum : uint8_t {
  k_ra8_ulpt_test_ch_0   = 0U,   /**< RA8 ulpt test channel 0.   */
  k_ra8_ulpt_test_ch_1   = 1U,   /**< RA8 ulpt test channel 1.   */
  k_ra8_ulpt_test_ch_bad = 2U,   /**< RA8 ulpt test channel bad. */
  k_ra8_ulpt_test_ch_way = 200U, /**< RA8 ulpt test channel way. */
} ra8_ulpt_test_ch_t;

typedef enum : uint32_t {
  k_ra8_ulpt_test_period = 0x00ABCDEFUL, /**< RA8 ulpt test period. */
} ra8_ulpt_test_period_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_regs(void)
{
  TEST_BEGIN("ulpt init clears regs");
  ra8_fake_mmap_reset();

  volatile r_ulpt_regs_t* reg = ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->ULPTCR = k_t_ulptcr_all_ones;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_init());
  TEST_ASSERT_EQ(0, reg->ULPTCR);
  TEST_END("ulpt init clears regs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_channel_0(void)
{
  TEST_BEGIN("ulpt start channel 0");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_0, (uint32_t)k_ra8_ulpt_test_period));

  volatile r_ulpt_regs_t* reg = ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0);
  TEST_ASSERT_EQ(k_ra8_ulpt_test_period, reg->ULPTCNT);
  TEST_ASSERT_EQ(k_ra8_ulpt_mask_tstart, reg->ULPTCR);
  TEST_END("ulpt start channel 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_channel_1(void)
{
  TEST_BEGIN("ulpt start channel 1");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_1, (uint32_t)k_ra8_ulpt_test_period));
  TEST_END("ulpt start channel 1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_bad_channel(void)
{
  TEST_BEGIN("ulpt start bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_bad, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_way, 0U));
  TEST_END("ulpt start bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_happy(void)
{
  TEST_BEGIN("ulpt stop happy");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_0, (uint32_t)k_ra8_ulpt_test_period));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_stop((uint8_t)k_ra8_ulpt_test_ch_0));

  volatile r_ulpt_regs_t* reg = ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0);
  TEST_ASSERT_EQ(0, reg->ULPTCR);
  TEST_END("ulpt stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("ulpt stop bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_stop((uint8_t)k_ra8_ulpt_test_ch_bad));
  TEST_END("ulpt stop bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the ra8_ulpt_stop
 * TCSTF-confirm happy leg. The only compound decision reached is the
 * bounded-wait loop guard inside `ra8_hw_wait_flag_clear8`, whose MC/DC
 * vectors are owned by tests/test_ra8_hw_err_cov.c)
 */
static void test_stop_confirms_tcstf_clear(void)
{
  TEST_BEGIN("ulpt stop confirms TCSTF low");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ulpt_start((uint8_t)k_ra8_ulpt_test_ch_0, (uint32_t)k_ra8_ulpt_test_period));

  /* Seam unarmed: ULPTCR reads back 0 after the stop toggle, so TCSTF is
   * already low and the confirm poll succeeds on its first read. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_stop((uint8_t)k_ra8_ulpt_test_ch_0));

  volatile r_ulpt_regs_t* reg = ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(0, (reg->ULPTCR & (uint8_t)k_ra8_ulpt_mask_tcstf));
  TEST_END("ulpt stop confirms TCSTF low");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the ra8_ulpt_stop timeout
 * leg via the ra8_fake_mmio fault seam. The loop-bound decision inside
 * `ra8_hw_wait_flag_clear8` is MC/DC-covered by test_ra8_hw_err_cov.c)
 */
static void test_stop_tcstf_stuck_times_out(void)
{
  TEST_BEGIN("ulpt stop TCSTF stuck -> timeout");
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();

  volatile r_ulpt_regs_t* reg = ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0);
  TEST_ASSERT_NOT_NULL((void*)reg);

  /* Arm the ULPTCR window so every TCSTF-clear poll reports "still set":
   * models a counter whose count-status flag never drops (the >=2-wake
   * Software-Standby re-arm hang this guard prevents). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&reg->ULPTCR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_ulpt_stop((uint8_t)k_ra8_ulpt_test_ch_0));

  ra8_fake_mmio_reset();
  TEST_END("ulpt stop TCSTF stuck -> timeout");
}

/* ---- full build-out ---- */

static uint32_t s_ulpt_cb_count;
static uint8_t  s_ulpt_cb_last_ch;

static void stub_ulpt_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_ulpt_cb_count;
  s_ulpt_cb_last_ch = ch;
}

static void prep_w43(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_ulpt_cb_count   = 0U;
  s_ulpt_cb_last_ch = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("ulpt deinit");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_deinit((uint8_t)k_ra8_ulpt_test_ch_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_deinit((uint8_t)k_ra8_ulpt_test_ch_bad));
  TEST_END("ulpt deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_period_and_status(void)
{
  TEST_BEGIN("ulpt set_period + status");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_set_period((uint8_t)k_ra8_ulpt_test_ch_0, 0x1234U));
  TEST_ASSERT_EQ(0x1234U, ra8_ulpt((uint8_t)k_ra8_ulpt_test_ch_0)->ULPTCNT);

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_get_status((uint8_t)k_ra8_ulpt_test_ch_0, &mask));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ulpt_get_status((uint8_t)k_ra8_ulpt_test_ch_0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_set_period((uint8_t)k_ra8_ulpt_test_ch_bad, 0U));
  TEST_END("ulpt set_period + status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("ulpt attach + dispatch");
  prep_w43();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_attach_handler(stub_ulpt_cb, (void*)(uintptr_t)0x77U));
  ra8_ulpt_dispatch((uint8_t)k_ra8_ulpt_test_ch_1);
  TEST_ASSERT_EQ(1, s_ulpt_cb_count);
  TEST_ASSERT_EQ(k_ra8_ulpt_test_ch_1, s_ulpt_cb_last_ch);

  ra8_ulpt_dispatch((uint8_t)k_ra8_ulpt_test_ch_bad);
  TEST_ASSERT_EQ(1, s_ulpt_cb_count);
  TEST_END("ulpt attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("ulpt power transition");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_enter_stop((uint8_t)k_ra8_ulpt_test_ch_0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ulpt_exit_stop((uint8_t)k_ra8_ulpt_test_ch_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_enter_stop((uint8_t)k_ra8_ulpt_test_ch_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ulpt_exit_stop((uint8_t)k_ra8_ulpt_test_ch_bad));
  TEST_END("ulpt power transition");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_start_channel_0();
  test_start_channel_1();
  test_start_bad_channel();
  test_stop_happy();
  test_stop_bad_channel();
  test_stop_confirms_tcstf_clear();
  test_stop_tcstf_stuck_times_out();
  test_deinit();
  test_set_period_and_status();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra8_ulpt.c\n");
  return 0;
}
