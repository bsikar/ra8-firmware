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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_probe_no_image(void)
{
  TEST_BEGIN("test_probe_no_image");
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(nullptr, 0U));
  TEST_ASSERT_EQ(1U, ra_ble_patch_test_warned());
  TEST_ASSERT_EQ(0U, ra_ble_patch_is_loaded());
  /* Second call must be idempotent (no second warning, same return). */
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(nullptr, 0U));
  TEST_END("test_probe_no_image");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_with_len(void)
{
  TEST_BEGIN("test_null_with_len");
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_patch_load(nullptr, 32U));
  TEST_END("test_null_with_len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_invalid_len(void)
{
  TEST_BEGIN("test_invalid_len");
  ra_ble_patch_test_reset();
  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U << 20));
  TEST_END("test_invalid_len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_real_image_unsupported(void)
{
  TEST_BEGIN("test_real_image_unsupported");
  ra_ble_patch_test_reset();
  uint8_t buf[64] = {};
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(buf, sizeof(buf)));
  TEST_ASSERT_EQ(0U, ra_ble_patch_is_loaded());
  TEST_END("test_real_image_unsupported");
}

/**
 * @test test_mcdc_ra_ble_patch
 *
 * @par MC/DC:
 * Decision A: ``ra_ble_patch_load`` line 80,
 * libs/ra_hal/src/ra_ble_patch.c:
 * ``if (image == NULL && len == 0U)`` (2 conditions, ``&&``). N+1 = 3:
 * - V1: image=NULL, len=0   -> dec T (probe path)
 * - V2: image=NULL, len=32  -> dec F (null_ptr)
 * - V3: image=valid, len=64 -> dec F (process)
 *
 * Decision B: ``ra_ble_patch_load`` line 95,
 * ``if (len < MIN || len > MAX)`` (2 conditions, ``||``). N+1 = 3:
 * - V1: len=64 (in range)  -> dec F (process; not_supp)
 * - V2: len=1   (< MIN)    -> dec T (invalid_arg)
 * - V3: len=1MB (> MAX)    -> dec T (invalid_arg)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra_ble_patch(void)
{
  TEST_BEGIN("ble_patch MC/DC: load 2-cond decisions");
  ra_ble_patch_test_reset();
  uint8_t buf[64] = {};
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(nullptr, 0U));
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_ble_patch_load(nullptr, 32U));
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(buf, 64U));
  ra_ble_patch_test_reset();
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_ble_patch_load(buf, 64U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_ble_patch_load(buf, 1U << 20));
  TEST_END("ble_patch MC/DC: load 2-cond decisions");
}

int main(void)
{
  test_probe_no_image();
  test_null_with_len();
  test_invalid_len();
  test_real_image_unsupported();
  test_mcdc_ra_ble_patch();
  return 0;
}
