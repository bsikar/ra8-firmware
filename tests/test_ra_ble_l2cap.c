/**
 * @file test_ra_ble_l2cap.c
 * @brief Standalone MC/DC unit tests for libs/ra_ble_host/src/ra_ble_l2cap.c
 *
 * @details
 * Provides labeled MC/DC vector sets for the public-API decisions in
 * ``ra_ble_l2cap.c`` flagged by docs/MCDC_GAPS.csv.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_ble_host.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_l2cap_role_bogus = 99U,
} test_l2cap_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_ble_host_close();
}

/**
 * @test test_mcdc_l2cap_init_role_4cond
 *
 * @par MC/DC:
 * Decision: ``if ((cfg->role != k_ra_ble_host_role_peripheral) &&
 *                 (cfg->role != k_ra_ble_host_role_central) &&
 *                 (cfg->role != k_ra_ble_host_role_observer) &&
 *                 (cfg->role != k_ra_ble_host_role_broadcaster))``
 * (4 conditions, libs/ra_ble_host/src/ra_ble_l2cap.c around line 662 --
 * ra_ble_host_init)
 *
 * Per DO-178C 6.4.4.3 representative-subset for chained ``&&`` decision
 * is N+1 = 5 vectors. Pairs (V1,V5)..(V4,V5) flip C1..C4 in turn.
 */
static void test_mcdc_l2cap_init_role_4cond(void)
{
  TEST_BEGIN("ra_ble_host_init MC/DC: role 4-cond chain");
  ra_ble_host_config_t cfg = {.appearance = 0U, .name = "t"};
  prep();
  cfg.role = k_ra_ble_host_role_peripheral;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_central;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_observer;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = k_ra_ble_host_role_broadcaster;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  prep();
  cfg.role = (ra_ble_host_role_t)k_test_l2cap_role_bogus;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ble_host_init(&cfg));
  TEST_END("ra_ble_host_init MC/DC: role 4-cond chain");
}

int32_t main(void)
{
  test_mcdc_l2cap_init_role_4cond();
  (void)fprintf(stderr, "[OK ] test_ra_ble_l2cap.c\n");
  return 0;
}
