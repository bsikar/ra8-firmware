/**
 * @file test_ra8_eth_gptp.c
 * @brief Unit tests for ra8_eth_gptp.c (GPTP sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_eth_gptp.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum eth_gptp_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_gptp_probe_sts_a = 0xBEEFU, /**< Planted in GPTP_STS to prove the read reaches the register. */
  k_gptp_probe_sts_b =
    0xFACEU, /**< A second, different value, so the read cannot be a cached first result. */
} eth_gptp_fixture_t;

/**
 * @enum eth_gptp_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_gptp_probe_sts_wide = 0x1EEE1588U, /**< A full 32-bit value proving no field is truncated. */
} eth_gptp_fixture2_t;

static uint32_t s_gptp_cb_count;
static uint32_t s_gptp_cb_last_mask;

static void stub_gptp_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_gptp_cb_count;
  s_gptp_cb_last_mask = mask;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_gptp_cb_count     = 0U;
  s_gptp_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("gptp init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_init());
  TEST_END("gptp init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("gptp deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_deinit());
  TEST_END("gptp deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("gptp status read + clear");
  prep();
  ra8_gptp()->GPTP_STS = k_gptp_probe_sts_wide;
  uint32_t mask        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_get_status(&mask));
  TEST_ASSERT_EQ(0x1EEE1588U, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_clear_status(0x0000FFFFU));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_gptp_get_status(nullptr));
  TEST_END("gptp status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("gptp attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_attach_handler(stub_gptp_cb, (void*)(uintptr_t)0xF0U));
  ra8_gptp()->GPTP_STS = k_gptp_probe_sts_a;
  ra8_eth_gptp_dispatch();
  TEST_ASSERT_EQ(1, s_gptp_cb_count);
  TEST_ASSERT_EQ(0xBEEFU, s_gptp_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_attach_handler(nullptr, nullptr));
  ra8_gptp()->GPTP_STS = k_gptp_probe_sts_b;
  ra8_eth_gptp_dispatch();
  TEST_ASSERT_EQ(1, s_gptp_cb_count);
  TEST_END("gptp attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("gptp power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_gptp_exit_stop());
  TEST_END("gptp power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra8_eth_gptp.c\n");
  return 0;
}
