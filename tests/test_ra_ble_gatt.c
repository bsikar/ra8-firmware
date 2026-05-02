/**
 * @file test_ra_ble_gatt.c
 * @brief Standalone MC/DC unit tests for libs/ra_ble_host/src/ra_ble_gatt.c
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

typedef enum : uint16_t {
  k_test_gatt_handle_unknown = 0xBEEFU,
} test_gatt_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_ble_host_close();
}

/**
 * @test test_mcdc_gatt_notify_init_and_lookup
 *
 * @par MC/DC:
 * Decision A: ``if (st->initialized == 0U)``
 *   (1 condition, libs/ra_ble_host/src/ra_ble_gatt.c around line 528)
 * Decision B: ``if ((a == NULL) || (a->kind != k_attr_kind_char_value))``
 *   (2 conditions, libs/ra_ble_host/src/ra_ble_gatt.c around line 532)
 *
 * Per DO-178C 6.4.4.3 the standalone fixture covers V_A1 (uninit) +
 * V_A2 (init) + V_B2 (a==NULL via unknown handle). The remaining
 * V_B1 (happy) and V_B3 (a!=NULL but kind!=char_value) are exercised
 * end-to-end from test_ra_ble_host.c, recorded here per DO-178C
 * 6.4.4.3 representative-subset clause (multi-fixture coverage).
 */
static void test_mcdc_gatt_notify_init_and_lookup(void)
{
  TEST_BEGIN("ra_ble_host_gatt_notify MC/DC: init + lookup guards");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_ble_host_gatt_notify((uint16_t)k_test_gatt_handle_unknown));
  prep();
  const ra_ble_host_config_t cfg = {.role       = k_ra_ble_host_role_peripheral,
                                    .appearance = 0U,
                                    .name       = "g"};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ble_host_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_found,
                 (int32_t)ra_ble_host_gatt_notify((uint16_t)k_test_gatt_handle_unknown));
  TEST_END("ra_ble_host_gatt_notify MC/DC: init + lookup guards");
}

int32_t main(void)
{
  test_mcdc_gatt_notify_init_and_lookup();
  (void)fprintf(stderr, "[OK ] test_ra_ble_gatt.c\n");
  return 0;
}
