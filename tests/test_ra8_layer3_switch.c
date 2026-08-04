/**
 * @file test_ra8_layer3_switch.c
 * @brief Unit tests for ra8_layer3_switch.c (Layer-3 switch placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_layer3_switch.h"
#include "unity_minimal.h"

/**
 * @enum layer3_switch_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_l3_mtu_bytes = 1500U, /**< The standard Ethernet MTU the switch is configured with. */
} layer3_switch_fixture_t;

typedef enum : uint32_t {
  k_test_l3_dst_ip = 0xC0A80100U, /**< 192.168.1.0 in net order. */
  k_test_l3_mask   = 0xFFFFFF00U, /**< Test l3 mask.             */
} test_layer3_const_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_layer3_switch_close();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_layer3_switch_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects zero port_count or mtu_bytes");
  prep();
  ra8_layer3_switch_cfg_t cfg = {.port_count = 0U, .mtu_bytes = k_l3_mtu_bytes, .promiscuous = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_layer3_switch_open(&cfg));
  cfg.port_count = 4U;
  cfg.mtu_bytes  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_layer3_switch_open(&cfg));
  TEST_END("open rejects zero port_count or mtu_bytes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra8_layer3_switch_cfg_t cfg = {.port_count = 4U, .mtu_bytes = 1500U, .promiscuous = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_layer3_switch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_layer3_switch_close());
  TEST_END("open / close lifecycle");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_add(void)
{
  TEST_BEGIN("route_add validates and refuses on placeholder");
  prep();
  const ra8_layer3_switch_route_t route = {.dst_ip      = (uint32_t)k_test_l3_dst_ip,
                                           .mask        = (uint32_t)k_test_l3_mask,
                                           .egress_port = 1U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_layer3_switch_route_add(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_layer3_switch_route_add(&route));

  const ra8_layer3_switch_cfg_t cfg = {.port_count = 4U, .mtu_bytes = 1500U, .promiscuous = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_layer3_switch_route_add(&route));
  TEST_END("route_add validates and refuses on placeholder");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_route_delete(void)
{
  TEST_BEGIN("route_delete state-checked");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    ra8_layer3_switch_route_delete((uint32_t)k_test_l3_dst_ip, (uint32_t)k_test_l3_mask));
  const ra8_layer3_switch_cfg_t cfg = {.port_count = 4U, .mtu_bytes = 1500U, .promiscuous = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_open(&cfg));
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_layer3_switch_route_delete((uint32_t)k_test_l3_dst_ip, (uint32_t)k_test_l3_mask));
  TEST_END("route_delete state-checked");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status(void)
{
  TEST_BEGIN("status_get null + reflects state");
  prep();
  uint8_t op = 0U;
  uint8_t pr = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_layer3_switch_status_get(nullptr, &pr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_layer3_switch_status_get(&op, nullptr));

  const ra8_layer3_switch_cfg_t cfg = {.port_count = 2U, .mtu_bytes = 9000U, .promiscuous = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_layer3_switch_status_get(&op, &pr));
  TEST_ASSERT_EQ(1, op);
  TEST_ASSERT_EQ(1, pr);
  TEST_END("status_get null + reflects state");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_route_add();
  test_route_delete();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra8_layer3_switch.c\n");
  return 0;
}
