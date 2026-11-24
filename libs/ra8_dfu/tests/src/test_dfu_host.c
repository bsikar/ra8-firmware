/**
 * @file test_dfu_host.c
 * @brief Host C ABI acceptance tests for the Zig `ra8_dfu_host` driver.
 *
 * @details
 * Links directly against the `ra8_dfu` archive via `ra8_dfu_host.h` and drives
 * `ra8_dfu_host_run` and `ra8_dfu_host_program` over a scripted host USB HAL
 * seam. Exercises `Err.from` across named HAL codes, other valid `ra8_err_t`
 * values outside the driver's subset, and out-of-range raw codes to prove no
 * enum conversion panic crosses the C ABI in Debug or ReleaseSafe builds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_dfu_host.h"
#include "ra8_err.h"
#include "ra8_usb.h"

/**
 * @enum dfu_host_test_const_t
 * @brief Named constants for the C host-driver acceptance harness.
 */
typedef enum : uint32_t {
  k_test_block_bytes  = 64U,     /**< Matches DFU wTransferSize (64 bytes).     */
  k_test_img_fill     = 0xA5U,   /**< Deterministic payload byte pattern.       */
  k_test_err_unknown  = 0xFFFFU, /**< Out-of-range ra8_err_t code.              */
  k_test_err_custom   = 0xBEEFU, /**< Arbitrary unknown 16-bit error code.      */
  k_test_flash_stub   = 0x0107U, /**< k_ra8_err_not_supported for flash seam.   */
  k_test_line_j       = 1U,      /**< Attached full-speed J-state line reading. */
  k_test_max_usb_addr = 127U,    /**< Highest valid 7-bit USB device address.   */
} dfu_host_test_const_t;

/**
 * @struct dfu_host_mock_t
 * @brief Scripted HAL state backing the `ra8_usb_host_*` seam.
 */
typedef struct {
  ra8_err_t       init_err;     /**< Code returned by `ra8_usb_host_init`.         */
  ra8_err_t       xfer_err;     /**< Code returned by `ra8_usb_host_control_xfer`. */
  ra8_usb_speed_t last_speed;   /**< Last controller speed passed into the seam.   */
  uint32_t        init_calls;   /**< Number of `ra8_usb_host_init` invocations.    */
  uint32_t        deinit_calls; /**< Number of `ra8_usb_host_deinit` invocations.  */
  uint32_t        clock_ms;     /**< Monotonic millisecond mock clock.             */
  uint32_t        failures;     /**< Accumulated test assertion failure count.     */
} dfu_host_mock_t;

static dfu_host_mock_t s_mock = {};

static void internal_reset_mock(void)
{
  s_mock.init_err     = k_ra8_ok;
  s_mock.xfer_err     = k_ra8_ok;
  s_mock.last_speed   = k_ra8_usb_speed_fs;
  s_mock.init_calls   = 0U;
  s_mock.deinit_calls = 0U;
  s_mock.clock_ms     = 0U;
}

static void internal_check(bool condition, const char* what)
{
  if (!condition) {
    (void)fprintf(stderr, "FAIL: %s\n", what);
    s_mock.failures++;
  }
}

ra8_err_t ra8_usb_host_init(ra8_usb_speed_t speed)
{
  s_mock.last_speed = speed;
  s_mock.init_calls++;
  return s_mock.init_err;
}

ra8_err_t ra8_usb_host_deinit(ra8_usb_speed_t speed)
{
  s_mock.last_speed = speed;
  s_mock.deinit_calls++;
  return k_ra8_ok;
}

ra8_err_t ra8_usb_host_bus_reset(ra8_usb_speed_t speed, bool assert_reset)
{
  s_mock.last_speed = speed;
  return assert_reset ? k_ra8_ok : k_ra8_ok;
}

ra8_err_t ra8_usb_host_set_uact(ra8_usb_speed_t speed, bool enable)
{
  s_mock.last_speed = speed;
  return enable ? k_ra8_ok : k_ra8_ok;
}

ra8_err_t ra8_usb_host_set_target(ra8_usb_speed_t speed, uint8_t dev_addr)
{
  s_mock.last_speed = speed;
  return (dev_addr <= (uint8_t)k_test_max_usb_addr) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

uint16_t ra8_usb_host_line_state(ra8_usb_speed_t speed)
{
  s_mock.last_speed = speed;
  return (uint16_t)k_test_line_j;
}

ra8_err_t ra8_usb_host_control_xfer(ra8_usb_speed_t        speed,
                                    const ra8_usb_setup_t* setup,
                                    uint8_t*               data,
                                    uint16_t               data_len,
                                    uint16_t*              out_received)
{
  s_mock.last_speed = speed;
  if (setup == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((data != nullptr) && (data_len > 0U)) {
    data[0] = 0U;
  }
  if (out_received != nullptr) {
    *out_received = 0U;
  }
  return s_mock.xfer_err;
}

uint32_t ra8_time_ms(void)
{
  return s_mock.clock_ms;
}

void ra8_delay_ms(uint32_t ms)
{
  s_mock.clock_ms += ms;
}

/* Host-link seam symbols pulled in by the single-archive ra8_dfu build. */
uint16_t ra8_flash_open(const void* cfg)
{
  return (cfg != nullptr) ? (uint16_t)k_test_flash_stub : (uint16_t)k_ra8_err_null_ptr;
}

uint16_t ra8_flash_set_window(uintptr_t low, uintptr_t high)
{
  return (low <= high) ? (uint16_t)k_test_flash_stub : (uint16_t)k_ra8_err_invalid_arg;
}

uint16_t ra8_flash_write_block(uint32_t mram_addr, const uint8_t* src, uint32_t len, uint8_t world)
{
  (void)mram_addr;
  (void)world;
  return ((src != nullptr) && (len > 0U)) ? (uint16_t)k_test_flash_stub
                                          : (uint16_t)k_ra8_err_invalid_arg;
}

void ra8_scb_set_vtor(uintptr_t base)
{
  (void)base;
}

static void internal_test_unknown_init_error(void)
{
  uint8_t               img[k_test_block_bytes] = {};
  ra8_dfu_host_result_t res                     = {};

  img[0] = (uint8_t)k_test_img_fill;

  /* Valid ra8_err_t code outside Err's named enum subset. */
  internal_reset_mock();
  s_mock.init_err = k_ra8_err_hw_init_failed;
  ra8_err_t err   = ra8_dfu_host_run(k_ra8_usb_speed_hs, img, (uint32_t)sizeof(img), &res);
  internal_check(err == k_ra8_err_hw_init_failed, "run returns k_ra8_err_hw_init_failed");
  internal_check(res.last_err == k_ra8_err_hw_init_failed, "result mirrors hw_init_failed");
  internal_check(s_mock.init_calls == 1U, "init called once");
  internal_check(s_mock.deinit_calls == 0U, "deinit skipped on failed init");

  /* Arbitrary out-of-range raw error code: must pass through without panicking. */
  internal_reset_mock();
  s_mock.init_err = (ra8_err_t)k_test_err_unknown;
  err             = ra8_dfu_host_run(k_ra8_usb_speed_fs, img, (uint32_t)sizeof(img), &res);
  internal_check((uint16_t)err == (uint16_t)k_test_err_unknown,
                 "run preserves unknown 0xFFFF code without panicking");
  internal_check((uint16_t)res.last_err == (uint16_t)k_test_err_unknown,
                 "result.last_err mirrors 0xFFFF");
}

static void internal_test_unknown_xfer_error(void)
{
  uint8_t               img[k_test_block_bytes] = {};
  ra8_dfu_host_result_t res                     = {};

  img[0] = (uint8_t)k_test_img_fill;

  /* Valid ra8_err_t outside Err's named subset during enumeration transfer. */
  internal_reset_mock();
  s_mock.xfer_err = k_ra8_err_busy;
  ra8_err_t err   = ra8_dfu_host_program(k_ra8_usb_speed_hs, img, (uint32_t)sizeof(img), &res);
  internal_check(err == k_ra8_err_busy, "program returns k_ra8_err_busy");
  internal_check(res.last_err == k_ra8_err_busy, "result mirrors busy");
  internal_check(s_mock.deinit_calls == 1U, "deinit tears down controller after transfer error");

  /* Out-of-range raw code returned from control_xfer. */
  internal_reset_mock();
  s_mock.xfer_err = (ra8_err_t)k_test_err_custom;
  err             = ra8_dfu_host_program(k_ra8_usb_speed_hs, img, (uint32_t)sizeof(img), &res);
  internal_check((uint16_t)err == (uint16_t)k_test_err_custom,
                 "program preserves unknown 0xBEEF code without panicking");
  internal_check((uint16_t)res.last_err == (uint16_t)k_test_err_custom,
                 "result.last_err mirrors 0xBEEF");
  internal_check(s_mock.deinit_calls == 1U, "deinit called on unknown xfer error");
}

int main(void)
{
  internal_test_unknown_init_error();
  internal_test_unknown_xfer_error();

  if (s_mock.failures != 0U) {
    (void)fprintf(stderr, "%u check(s) failed\n", s_mock.failures);
    return 1;
  }
  (void)printf("test_dfu_host: OK\n");
  return 0;
}
