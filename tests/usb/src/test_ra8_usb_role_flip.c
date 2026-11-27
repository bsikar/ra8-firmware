/**
 * @file test_ra8_usb_role_flip.c
 * @brief Host unit tests for runtime USB role swap choreography.
 *
 * @details
 * Validates the dynamic teardown and role reversal state machine:
 *  - Config A: FS Device + HS Host
 *  - Teardown Config A via ux_dcd_ra8_usb_uninitialize, ra8_usb_host_deinit, ra8_usb_pal_deinit
 *  - Config B: FS Host + HS Device
 *  - Teardown Config B
 *
 * [Ring 4 / APP_TEST]
 * {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_pin_validator.h"
#include "ra8_usb.h"
#include "ra8_usb_host.h"
#include "ra8_usb_pal.h"
#include "unity_minimal.h"
#include "usb_role_flip.h"

/* Forward declare mock variables */
static ra8_err_t       s_mock_dcd_init_ret     = k_ra8_ok;
static ra8_err_t       s_mock_dcd_deinit_ret   = k_ra8_ok;
static uint32_t        s_mock_dcd_init_calls   = 0U;
static uint32_t        s_mock_dcd_deinit_calls = 0U;
static ra8_usb_speed_t s_mock_dcd_last_speed   = k_ra8_usb_speed_fs;

ra8_err_t ux_dcd_ra8_usb_initialize(ra8_usb_speed_t speed)
{
  s_mock_dcd_init_calls++;
  s_mock_dcd_last_speed = speed;
  return s_mock_dcd_init_ret;
}

ra8_err_t ux_dcd_ra8_usb_uninitialize(void)
{
  s_mock_dcd_deinit_calls++;
  return s_mock_dcd_deinit_ret;
}

/* Include step implementation directly for white-box host testing */
#include "usb_role_flip_steps.c"

/**
 * @brief Reset fake MMIO, pin ownership, MSTP, and mock hooks.
 */
static void prep(void)
{
  (void)ra8_usb_pal_deinit();
  (void)ra8_usb_host_deinit(k_ra8_usb_speed_fs);
  (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
  s_mock_dcd_init_ret     = k_ra8_ok;
  s_mock_dcd_deinit_ret   = k_ra8_ok;
  s_mock_dcd_init_calls   = 0U;
  s_mock_dcd_deinit_calls = 0U;
  s_mock_dcd_last_speed   = k_ra8_usb_speed_fs;
}

/**
 * @brief Verify Config A setup and teardown sequence.
 */
static void test_role_flip_cfg_a_lifecycle(void)
{
  TEST_BEGIN("role_flip: Config A setup and teardown");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, usb_role_flip_setup_cfg_a());
  TEST_ASSERT_EQ(1U, s_mock_dcd_init_calls);
  TEST_ASSERT_EQ((uint32_t)k_ra8_usb_speed_fs, (uint32_t)s_mock_dcd_last_speed);

  TEST_ASSERT_EQ(k_ra8_ok, usb_role_flip_teardown_cfg_a());
  TEST_ASSERT_EQ(1U, s_mock_dcd_deinit_calls);
  TEST_END("role_flip: Config A setup and teardown");
}

/**
 * @brief Verify Config B setup and teardown sequence.
 */
static void test_role_flip_cfg_b_lifecycle(void)
{
  TEST_BEGIN("role_flip: Config B setup and teardown");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, usb_role_flip_setup_cfg_b());
  TEST_ASSERT_EQ(1U, s_mock_dcd_init_calls);
  TEST_ASSERT_EQ((uint32_t)k_ra8_usb_speed_hs, (uint32_t)s_mock_dcd_last_speed);

  TEST_ASSERT_EQ(k_ra8_ok, usb_role_flip_teardown_cfg_b());
  TEST_ASSERT_EQ(1U, s_mock_dcd_deinit_calls);
  TEST_END("role_flip: Config B setup and teardown");
}

/**
 * @brief Verify complete two-phase cycle execution.
 */
static void test_role_flip_full_cycle(void)
{
  TEST_BEGIN("role_flip: Full role swap cycle");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, usb_role_flip_run_cycle());
  TEST_ASSERT_EQ(2U, s_mock_dcd_init_calls);
  TEST_ASSERT_EQ(2U, s_mock_dcd_deinit_calls);
  TEST_END("role_flip: Full role swap cycle");
}

/**
 * @brief Verify failure handling during DCD init and teardown.
 */
static void test_role_flip_dcd_errors(void)
{
  TEST_BEGIN("role_flip: DCD error propagation");
  prep();

  s_mock_dcd_init_ret = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, usb_role_flip_setup_cfg_a());

  prep();
  s_mock_dcd_deinit_ret = k_ra8_err_busy;
  TEST_ASSERT_EQ(k_ra8_err_busy, usb_role_flip_teardown_cfg_a());

  prep();
  s_mock_dcd_init_ret = k_ra8_err_timeout;
  TEST_ASSERT_EQ(k_ra8_err_timeout, usb_role_flip_run_cycle());
  TEST_END("role_flip: DCD error propagation");
}

int main(void)
{
  test_role_flip_cfg_a_lifecycle();
  test_role_flip_cfg_b_lifecycle();
  test_role_flip_full_cycle();
  test_role_flip_dcd_errors();
  return 0;
}
