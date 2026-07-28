/**
 * @file test_ra8_eth_mfwd.c
 * @brief Unit tests for ra8_eth_mfwd.c (MFWD sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_eth_mfwd.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum eth_mfwd_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_mfwd_probe_sts_a = 0xABCDU, /**< Planted in MFWD_STS to prove the read reaches the register. */
  k_mfwd_probe_sts_b =
    0xDEADU, /**< A second, different value, so the read cannot be a cached first result. */
} eth_mfwd_fixture_t;

/**
 * @enum eth_mfwd_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_mfwd_probe_sts_wide = 0xFEEDFACEU, /**< A full 32-bit value proving no field is truncated. */
} eth_mfwd_fixture2_t;

static uint32_t s_mfwd_cb_count;
static uint32_t s_mfwd_cb_last_mask;

static void stub_mfwd_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_mfwd_cb_count;
  s_mfwd_cb_last_mask = mask;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_mfwd_cb_count     = 0U;
  s_mfwd_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("mfwd init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_init());
  TEST_END("mfwd init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("mfwd deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_deinit());
  TEST_END("mfwd deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("mfwd status read + clear");
  prep();
  ra8_mfwd()->MFWD_STS = k_mfwd_probe_sts_wide;
  uint32_t mask        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_get_status(&mask));
  TEST_ASSERT_EQ(0xFEEDFACEU, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_clear_status(0x0F0F0F0FU));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_mfwd_get_status(nullptr));
  TEST_END("mfwd status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("mfwd attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_attach_handler(stub_mfwd_cb, (void*)(uintptr_t)0xA0U));
  ra8_mfwd()->MFWD_STS = k_mfwd_probe_sts_a;
  ra8_eth_mfwd_dispatch();
  TEST_ASSERT_EQ(1, s_mfwd_cb_count);
  TEST_ASSERT_EQ(0xABCDU, s_mfwd_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_attach_handler(nullptr, nullptr));
  ra8_mfwd()->MFWD_STS = k_mfwd_probe_sts_b;
  ra8_eth_mfwd_dispatch();
  TEST_ASSERT_EQ(1, s_mfwd_cb_count);
  TEST_END("mfwd attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("mfwd power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_exit_stop());
  TEST_END("mfwd power transition");
}

/**
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_eth_mfwd.c@ra8_eth_mfwd_route_queue):
 *   ``(port > k_ra8_mfwd_max_port) || (queue_index > k_ra8_mfwd_max_queue)``
 * inside ra8_eth_mfwd_route_queue. Two-condition decision needs N+1 = 3 vectors.
 * Vector 1: port=0, queue=0   -> false (both conditions false; happy path).
 * Vector 2: port=2, queue=0   -> true  (varies port only -> isolates port).
 * Vector 3: port=0, queue=32  -> true  (varies queue only -> isolates queue).
 * Vectors 1+2 prove port independently affects the outcome.
 * Vectors 1+3 prove queue_index independently affects the outcome.
 */
static void test_mcdc_route_queue_bounds(void)
{
  TEST_BEGIN("mfwd route_queue MC/DC bounds");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_init());
  /* Vector 1: in-range port + queue -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_route_queue(0U, 0U));
  /* Vector 2: port out of range, queue in range -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_mfwd_route_queue(2U, 0U));
  /* Vector 3: port in range, queue out of range -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_eth_mfwd_route_queue(0U, 32U));
  /* Also confirm a second valid port pair lands. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_mfwd_route_queue(1U, 31U));
  TEST_END("mfwd route_queue MC/DC bounds");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_mcdc_route_queue_bounds();
  (void)fprintf(stderr, "[OK  ] test_ra8_eth_mfwd.c\n");
  return 0;
}
