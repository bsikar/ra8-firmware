/**
 * @file test_ra_agt.c
 * @brief Unit tests for ra_agt.c (Asynchronous General-Purpose Timer)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_agt_regs.h"
#include "ra_agt.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_agt_test_channel_valid  = 0U,
  k_ra_agt_test_channel_middle = 5U,
  k_ra_agt_test_channel_last   = 9U,
  k_ra_agt_test_channel_bad    = 10U,
  k_ra_agt_test_channel_way    = 200U,
} ra_agt_test_channel_t;

typedef enum : uint16_t {
  k_ra_agt_test_reload = 0x1234U,
} ra_agt_test_reload_t;

typedef enum : uint8_t {
  k_ra_agt_test_tstart_bit = 0x01U,
} ra_agt_test_bits_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_happy(void)
{
  TEST_BEGIN("agt start_free_run happy");
  ra_sim_mmap_reset();

  const ra_err_t err =
    ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_valid, (uint16_t)k_ra_agt_test_reload);
  TEST_ASSERT_EQ(k_ra_ok, err);

  volatile r_agt_regs_t* reg = ra_agt((uint8_t)k_ra_agt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra_agt_test_reload, reg->AGT);
  TEST_ASSERT_EQ(k_ra_agt_test_tstart_bit, reg->AGTCR);
  TEST_ASSERT_EQ(0, reg->AGTMR1);
  TEST_ASSERT_EQ(0, reg->AGTMR2);
  TEST_END("agt start_free_run happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_middle_channel(void)
{
  TEST_BEGIN("agt start_free_run middle channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_middle, (uint16_t)k_ra_agt_test_reload));
  volatile r_agt_regs_t* reg = ra_agt((uint8_t)k_ra_agt_test_channel_middle);
  TEST_ASSERT_EQ(k_ra_agt_test_reload, reg->AGT);
  TEST_END("agt start_free_run middle channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_last_channel(void)
{
  TEST_BEGIN("agt start_free_run last channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_ok, ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_last, 0U));
  TEST_END("agt start_free_run last channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_bad_channel(void)
{
  TEST_BEGIN("agt start_free_run bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_bad, 0U));
  TEST_END("agt start_free_run bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_huge_channel(void)
{
  TEST_BEGIN("agt start_free_run huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_way, 0U));
  TEST_END("agt start_free_run huge channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_happy(void)
{
  TEST_BEGIN("agt stop happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_valid, (uint16_t)k_ra_agt_test_reload));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_stop((uint8_t)k_ra_agt_test_channel_valid));

  volatile r_agt_regs_t* reg = ra_agt((uint8_t)k_ra_agt_test_channel_valid);
  TEST_ASSERT_EQ(0, reg->AGTCR);
  TEST_END("agt stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("agt stop bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_agt_stop((uint8_t)k_ra_agt_test_channel_bad));
  TEST_END("agt stop bad channel");
}

/* ---- full build-out ---- */

static uint32_t s_agt_cb_count;
static uint8_t  s_agt_cb_last_ch;

static void stub_agt_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_agt_cb_count;
  s_agt_cb_last_ch = ch;
}

static void prep_w43(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_agt_cb_count   = 0U;
  s_agt_cb_last_ch = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("agt deinit");
  prep_w43();
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_valid, (uint16_t)k_ra_agt_test_reload));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_deinit((uint8_t)k_ra_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_agt_deinit((uint8_t)k_ra_agt_test_channel_bad));
  TEST_END("agt deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_reload_and_status(void)
{
  TEST_BEGIN("agt set_reload + status");
  prep_w43();
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_set_reload((uint8_t)k_ra_agt_test_channel_valid, 0xBEEFU));
  TEST_ASSERT_EQ(0xBEEFU, ra_agt((uint8_t)k_ra_agt_test_channel_valid)->AGT);

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_get_status((uint8_t)k_ra_agt_test_channel_valid, &mask));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_agt_get_status((uint8_t)k_ra_agt_test_channel_valid, nullptr));
  TEST_END("agt set_reload + status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("agt attach + dispatch");
  prep_w43();

  TEST_ASSERT_EQ(k_ra_ok, ra_agt_attach_handler(stub_agt_cb, (void*)(uintptr_t)0x88U));
  ra_agt_dispatch((uint8_t)k_ra_agt_test_channel_middle);
  TEST_ASSERT_EQ(1, s_agt_cb_count);
  TEST_ASSERT_EQ(k_ra_agt_test_channel_middle, s_agt_cb_last_ch);

  ra_agt_dispatch((uint8_t)k_ra_agt_test_channel_bad);
  TEST_ASSERT_EQ(1, s_agt_cb_count);
  TEST_END("agt attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("agt power transition");
  prep_w43();
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_agt_start_free_run((uint8_t)k_ra_agt_test_channel_valid, (uint16_t)k_ra_agt_test_reload));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_enter_stop((uint8_t)k_ra_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_exit_stop((uint8_t)k_ra_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_agt_enter_stop((uint8_t)k_ra_agt_test_channel_bad));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_agt_exit_stop((uint8_t)k_ra_agt_test_channel_bad));
  TEST_END("agt power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_mstp_channel_power(void)
{
  TEST_BEGIN("agt no-mstp channel power");
  prep_w43();
  /* Channels >= 2 have no dedicated MSTP bit; deinit / enter_stop /
   * exit_stop should return OK without calling ra_mstp_disable. */
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_deinit((uint8_t)k_ra_agt_test_channel_middle));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_enter_stop((uint8_t)k_ra_agt_test_channel_middle));
  TEST_ASSERT_EQ(k_ra_ok, ra_agt_exit_stop((uint8_t)k_ra_agt_test_channel_middle));
  TEST_END("agt no-mstp channel power");
}

int32_t main(void)
{
  test_start_free_run_happy();
  test_start_free_run_middle_channel();
  test_start_free_run_last_channel();
  test_start_free_run_bad_channel();
  test_start_free_run_huge_channel();
  test_stop_happy();
  test_stop_bad_channel();
  test_deinit();
  test_set_reload_and_status();
  test_attach_and_dispatch();
  test_power_transition();
  test_no_mstp_channel_power();
  (void)fprintf(stderr, "[OK ] test_ra_agt.c\n");
  return 0;
}
