/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_poeg.c
 * @brief Unit tests for ra8_poeg.c (Port Output Enable for GPT)
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_poeg.h"
#include "ra8_poeg_regs.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra8_poeg_test_group_valid = 0U, /**< RA8 poeg test group valid. */
  k_ra8_poeg_test_group_last  = 3U, /**< RA8 poeg test group last.  */
  k_ra8_poeg_test_group_bad   = 4U, /**< RA8 poeg test group bad.   */
} ra8_poeg_test_group_t;

static uint32_t s_poeg_cb_count;
static uint32_t s_poeg_cb_last_mask;
static void*    s_poeg_cb_last_ctx;

static void stub_poeg_cb(void* ctx, uint32_t mask)
{
  ++s_poeg_cb_count;
  s_poeg_cb_last_mask = mask;
  s_poeg_cb_last_ctx  = ctx;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_poeg_cb_count     = 0U;
  s_poeg_cb_last_mask = 0U;
  s_poeg_cb_last_ctx  = nullptr;
}

static ra8_poeg_cfg_t make_cfg(void)
{
  const ra8_poeg_cfg_t cfg = {
    .enable_pin      = true,
    .enable_ioc      = true,
    .enable_osc_stop = false,
    .invert_input    = true,
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
  TEST_BEGIN("poeg init configured");
  prep();

  const ra8_poeg_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_valid, &cfg));

  volatile r_poeg_regs_t* reg = ra8_poeg((uint8_t)k_ra8_poeg_test_group_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint32_t expected =
    (uint32_t)k_ra8_poeg_en_pide | (uint32_t)k_ra8_poeg_en_iocen | (uint32_t)k_ra8_poeg_en_inv;
  TEST_ASSERT_EQ(expected, reg->POEGG);
  TEST_END("poeg init configured");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("poeg init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_valid, nullptr));
  TEST_END("poeg init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_group(void)
{
  TEST_BEGIN("poeg init bad group");
  prep();

  const ra8_poeg_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_bad, &cfg));
  TEST_END("poeg init bad group");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("poeg deinit");
  prep();

  const ra8_poeg_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_deinit((uint8_t)k_ra8_poeg_test_group_valid));

  volatile r_poeg_regs_t* reg = ra8_poeg((uint8_t)k_ra8_poeg_test_group_valid);
  TEST_ASSERT_EQ(0, reg->POEGG);
  TEST_END("poeg deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_bad_group(void)
{
  TEST_BEGIN("poeg deinit bad group");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_poeg_deinit((uint8_t)k_ra8_poeg_test_group_bad));
  TEST_END("poeg deinit bad group");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_trigger_stop(void)
{
  TEST_BEGIN("poeg trigger stop");
  prep();

  const ra8_poeg_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_trigger_stop((uint8_t)k_ra8_poeg_test_group_valid));

  volatile r_poeg_regs_t* reg = ra8_poeg((uint8_t)k_ra8_poeg_test_group_valid);
  TEST_ASSERT((reg->POEGG & (uint32_t)k_ra8_poeg_status_ssf) != 0U);
  TEST_END("poeg trigger stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_trigger_stop_bad_group(void)
{
  TEST_BEGIN("poeg trigger stop bad group");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_poeg_trigger_stop((uint8_t)k_ra8_poeg_test_group_bad));
  TEST_END("poeg trigger stop bad group");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("poeg status read + clear");
  prep();

  volatile r_poeg_regs_t* reg = ra8_poeg((uint8_t)k_ra8_poeg_test_group_valid);
  reg->POEGG                  = (uint32_t)k_ra8_poeg_status_pidf | (uint32_t)k_ra8_poeg_status_iocf;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_get_status((uint8_t)k_ra8_poeg_test_group_valid, &mask));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_poeg_status_pidf | (uint32_t)k_ra8_poeg_status_iocf), mask);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_poeg_clear_status((uint8_t)k_ra8_poeg_test_group_valid, (uint32_t)k_ra8_poeg_status_pidf));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_get_status((uint8_t)k_ra8_poeg_test_group_valid, &mask));
  TEST_ASSERT_EQ(k_ra8_poeg_status_iocf, mask);
  TEST_END("poeg status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_null_out(void)
{
  TEST_BEGIN("poeg status null out");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_poeg_get_status((uint8_t)k_ra8_poeg_test_group_valid, nullptr));
  TEST_END("poeg status null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("poeg attach + dispatch");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_poeg_attach_handler((uint8_t)k_ra8_poeg_test_group_valid,
                                         stub_poeg_cb,
                                         (void*)(uintptr_t)0xA1A1U));

  volatile r_poeg_regs_t* reg = ra8_poeg((uint8_t)k_ra8_poeg_test_group_valid);
  reg->POEGG                  = (uint32_t)k_ra8_poeg_status_ssf;

  ra8_poeg_dispatch((uint8_t)k_ra8_poeg_test_group_valid);
  TEST_ASSERT_EQ(1, s_poeg_cb_count);
  TEST_ASSERT_EQ(k_ra8_poeg_status_ssf, s_poeg_cb_last_mask);
  TEST_END("poeg attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("poeg dispatch no handler");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_poeg_attach_handler((uint8_t)k_ra8_poeg_test_group_valid, nullptr, nullptr));

  ra8_poeg_dispatch((uint8_t)k_ra8_poeg_test_group_valid);
  ra8_poeg_dispatch((uint8_t)k_ra8_poeg_test_group_bad);
  TEST_ASSERT_EQ(0, s_poeg_cb_count);
  TEST_END("poeg dispatch no handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_bad_group(void)
{
  TEST_BEGIN("poeg attach bad group");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_poeg_attach_handler((uint8_t)k_ra8_poeg_test_group_bad, stub_poeg_cb, nullptr));
  TEST_END("poeg attach bad group");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("poeg power transition");
  prep();

  const ra8_poeg_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_init((uint8_t)k_ra8_poeg_test_group_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_enter_stop((uint8_t)k_ra8_poeg_test_group_valid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_poeg_exit_stop((uint8_t)k_ra8_poeg_test_group_valid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_poeg_enter_stop((uint8_t)k_ra8_poeg_test_group_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_poeg_exit_stop((uint8_t)k_ra8_poeg_test_group_bad));
  TEST_END("poeg power transition");
}

int32_t main(void)
{
  test_init_configured();
  test_init_null_cfg();
  test_init_bad_group();
  test_deinit();
  test_deinit_bad_group();
  test_trigger_stop();
  test_trigger_stop_bad_group();
  test_status_read_and_clear();
  test_status_null_out();
  test_attach_and_dispatch();
  test_dispatch_no_handler();
  test_attach_bad_group();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra8_poeg.c\n");
  return 0;
}
