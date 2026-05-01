/**
 * @file test_ra_ble_mesh.c
 * @brief Unit tests for libs/ra_ble_host mesh wrapper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble_mesh.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern void    ra_ble_mesh_test_emit_event(const ra_ble_mesh_event_t* evt);
extern uint8_t ra_ble_mesh_test_prov_active(void);

static uint32_t s_evt_count;

static void evt_cb(void* ctx, const ra_ble_mesh_event_t* evt)
{
  (void)ctx;
  (void)evt;
  s_evt_count++;
}

static void test_init_null(void)
{
  TEST_BEGIN("test_init_null");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_mesh_init(NULL));
  TEST_END("test_init_null");
}

static void test_init_invalid_count(void)
{
  TEST_BEGIN("test_init_invalid_count");
  ra_ble_mesh_config_t cfg = {};
  cfg.element_count        = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  cfg.element_count = (uint8_t)(k_ra_ble_mesh_max_elements + 1U);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_mesh_init(&cfg));
  TEST_END("test_init_invalid_count");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("test_lifecycle");
  ra_ble_mesh_config_t cfg     = {};
  cfg.element_count            = 1U;
  cfg.elements[0].model_count  = 1U;
  cfg.elements[0].models[0].id = 0x0000U; /* Configuration Server. */
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_init(&cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_attach_event_handler(evt_cb, NULL));
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_prov_enable());
  TEST_ASSERT_EQ(1U, ra_ble_mesh_test_prov_active());
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_prov_disable());
  TEST_ASSERT_EQ(0U, ra_ble_mesh_test_prov_active());
  s_evt_count             = 0U;
  ra_ble_mesh_event_t evt = {.kind = k_ra_ble_mesh_evt_provisioned};
  ra_ble_mesh_test_emit_event(&evt);
  TEST_ASSERT_EQ(1U, s_evt_count);
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_factory_reset());
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_mesh_close());
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_ble_mesh_close());
  TEST_END("test_lifecycle");
}

int main(void)
{
  test_init_null();
  test_init_invalid_count();
  test_lifecycle();
  return 0;
}
