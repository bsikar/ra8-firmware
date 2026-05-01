/**
 * @file test_ra_ble_patch.c
 * @brief Unit tests for libs/ra_hal/src/ra_ble_patch.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_ble_patch.h"
#include "ra_err.h"
#include "unity_minimal.h"

extern void    ra_ble_patch_test_reset(void);
extern uint8_t ra_ble_patch_test_warned(void);

static void test_probe_no_image(void)
{
  TEST_BEGIN("test_probe_no_image");
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(NULL, 0U));
  TEST_ASSERT_EQ(1U, ra_ble_patch_test_warned());
  TEST_ASSERT_EQ(0U, ra_ble_patch_is_loaded());
  /* Second call must be idempotent (no second warning, same return). */
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(NULL, 0U));
  TEST_END("test_probe_no_image");
}

static void test_null_with_len(void)
{
  TEST_BEGIN("test_null_with_len");
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_patch_load(NULL, 32U));
  TEST_END("test_null_with_len");
}

static void test_invalid_len(void)
{
  TEST_BEGIN("test_invalid_len");
  ra_ble_patch_test_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U << 20));
  TEST_END("test_invalid_len");
}

static void test_real_image_unsupported(void)
{
  TEST_BEGIN("test_real_image_unsupported");
  ra_ble_patch_test_reset();
  uint8_t buf[64] = {};
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(buf, sizeof(buf)));
  TEST_ASSERT_EQ(0U, ra_ble_patch_is_loaded());
  TEST_END("test_real_image_unsupported");
}

int main(void)
{
  test_probe_no_image();
  test_null_with_len();
  test_invalid_len();
  test_real_image_unsupported();
  return 0;
}
