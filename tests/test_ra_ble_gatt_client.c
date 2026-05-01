/**
 * @file test_ra_ble_gatt_client.c
 * @brief Unit tests for libs/ra_ble_host GATT client wrapper.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_ble_gatt_client.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern void     ra_ble_gatt_client_test_inject_notify(uint16_t       conn_handle,
                                                      uint16_t       attr_handle,
                                                      const uint8_t* data,
                                                      uint16_t       len);
extern uint32_t ra_ble_gatt_client_test_pending_count(void);
extern void     ra_ble_gatt_client_test_reset(void);

static uint32_t s_notify_count;
static uint16_t s_last_attr;

static void notify_cb(void* ctx, uint16_t attr_handle, const uint8_t* data, uint16_t len)
{
  (void)ctx;
  (void)data;
  (void)len;
  s_notify_count++;
  s_last_attr = attr_handle;
}

static void disc_cb(void* ctx, const ra_ble_gatt_service_t* svc, uint16_t status)
{
  (void)ctx;
  (void)svc;
  (void)status;
}

static void read_cb(void* ctx, const uint8_t* data, uint16_t len, uint16_t status)
{
  (void)ctx;
  (void)data;
  (void)len;
  (void)status;
}

static void test_discover_null(void)
{
  TEST_BEGIN("test_discover_null");
  ra_ble_gatt_client_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_gatt_discover_services(0x40U, NULL, NULL));
  TEST_END("test_discover_null");
}

static void test_discover_busy(void)
{
  TEST_BEGIN("test_discover_busy");
  ra_ble_gatt_client_test_reset();
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_discover_services(0x40U, disc_cb, NULL));
  TEST_ASSERT_EQ(k_ra_err_busy, ra_ble_gatt_discover_services(0x40U, disc_cb, NULL));
  TEST_END("test_discover_busy");
}

static void test_read_null(void)
{
  TEST_BEGIN("test_read_null");
  ra_ble_gatt_client_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_gatt_read(0x40U, 0x10U, NULL, NULL));
  TEST_END("test_read_null");
}

static void test_write_validation(void)
{
  TEST_BEGIN("test_write_validation");
  ra_ble_gatt_client_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_gatt_write(0x40U, 0x10U, NULL, 1U, 0U, NULL, NULL));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_ble_gatt_write(0x40U, 0x10U, (const uint8_t*)"x", 65535U, 0U, NULL, NULL));
  uint8_t b = 0xAB;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_write(0x40U, 0x10U, &b, 1U, 0U, NULL, NULL));
  TEST_END("test_write_validation");
}

static void test_subscribe_and_notify(void)
{
  TEST_BEGIN("test_subscribe_and_notify");
  ra_ble_gatt_client_test_reset();
  s_notify_count = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_ble_gatt_subscribe(0x40U, 0x12U, 1U, 0U, notify_cb, NULL));
  uint8_t payload = 0x55U;
  ra_ble_gatt_client_test_inject_notify(0x40U, 0x11U, &payload, 1U);
  TEST_ASSERT_EQ(1U, s_notify_count);
  TEST_ASSERT_EQ(0x11U, s_last_attr);
  TEST_END("test_subscribe_and_notify");
}

static void test_subscribe_invalid(void)
{
  TEST_BEGIN("test_subscribe_invalid");
  ra_ble_gatt_client_test_reset();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_gatt_subscribe(0x40U, 0x12U, 0U, 0U, NULL, NULL));
  TEST_END("test_subscribe_invalid");
}

int main(void)
{
  test_discover_null();
  test_discover_busy();
  test_read_null();
  test_write_validation();
  test_subscribe_and_notify();
  test_subscribe_invalid();
  /* Silence unused-function warnings; read completion exercised in target build only. */
  (void)read_cb;
  return 0;
}
