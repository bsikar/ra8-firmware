/**
 * @file main.c
 * @brief USB runtime role-flip self-loop application.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives a runtime swap of the USBHS and USBFS controller roles without resetting
 * the MCU. In Config A, USBFS runs in Device role and USBHS in Host role. After
 * executing teardown via ux_dcd_ra8_usb_uninitialize, ra8_usb_host_deinit, and
 * ra8_usb_pal_deinit, Config B is brought up with the roles swapped: USBFS in Host
 * role and USBHS in Device role.
 *
 * Emits "USB SELFTEST ROLE-FLIP PASS\r\n" upon verifying both phases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "usb_role_flip.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

#ifndef RA8_OFF_TARGET
#include "ra8_usb_compose.h"
#include "ux_api.h"
#include "ux_device_stack.h"

static uint8_t s_usbx_pool[32768];
static uint8_t s_device_framework[k_ra8_usb_desc_framework_bytes_max];
static uint8_t s_string_framework[k_ra8_usb_desc_strings_bytes_max];
static uint8_t s_langid_framework[k_ra8_usb_desc_langid_bytes];

static const ra8_usb_desc_device_t k_flip_usb_dev = {
  .vid          = 0x1209U,
  .pid          = 0x0001U,
  .bcd_device   = 0x0100U,
  .manufacturer = "RA8",
  .product      = "USB Role Flip",
  .serial       = "1",
};

static const ra8_usb_desc_dfu_t k_flip_usb_dfu = {
  .can_download      = true,
  .transfer_bytes    = 64U,
  .detach_timeout_ms = 1000U,
  .bcd_dfu           = 0x0110U,
};

static ra8_err_t flip_init_usbx_stack(void)
{
  const ra8_usb_class_t function = {
    .kind = k_ra8_usb_class_dfu,
    .dfu  = k_flip_usb_dfu,
  };
  const ra8_usb_device_cfg_t cfg = {
    .desc        = &k_flip_usb_dev,
    .classes     = &function,
    .class_count = 1U,
  };
  ra8_usb_device_frameworks_t fw = {
    .device      = s_device_framework,
    .device_cap  = (uint32_t)sizeof(s_device_framework),
    .strings     = s_string_framework,
    .strings_cap = (uint32_t)sizeof(s_string_framework),
    .langid      = s_langid_framework,
    .langid_cap  = (uint32_t)sizeof(s_langid_framework),
  };

  const ra8_err_t err = ra8_usb_device_compose(&cfg, &fw);
  if (err != k_ra8_ok) {
    return err;
  }

  if (_ux_system_initialize(s_usbx_pool, (ULONG)sizeof(s_usbx_pool), UX_NULL, 0UL) != UX_SUCCESS) {
    return k_ra8_err_invalid_state;
  }
  if (_ux_device_stack_initialize(s_device_framework,
                                  (ULONG)fw.device_len,
                                  s_device_framework,
                                  (ULONG)fw.device_len,
                                  s_string_framework,
                                  (ULONG)fw.strings_len,
                                  s_langid_framework,
                                  (ULONG)fw.langid_len,
                                  UX_NULL) != UX_SUCCESS) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}
#endif

/**
 * @enum usb_role_flip_cfg_t
 * @brief Application tunables.
 */
typedef enum : uint32_t {
  k_flip_baud      = 115200U, /**< Console UART baud rate. */
  k_flip_settle_ms = 50U,     /**< Role transition settle. */
} usb_role_flip_cfg_t;

/** @brief J-Link probe for the current role-flip phase. */
static volatile uint32_t s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_boot;

/** @brief J-Link probe counting completed full role-flip cycles. */
static volatile uint32_t s_dbg_pass_count = 0U;

static const uint8_t k_flip_start_msg[] =
  "usb_role_flip: starting runtime USB role swap selftest...\r\n";
static const uint8_t k_flip_pass_msg[] = "USB SELFTEST ROLE-FLIP PASS\r\n";
static const uint8_t k_flip_fail_msg[] = "usb_role_flip: role swap failed!\r\n";

/**
 * @brief Write message to console UART.
 *
 * @param[in] msg Message buffer.
 * @param[in] len Length in bytes.
 */
static void flip_log(const uint8_t* msg, size_t len)
{
  (void)ra8_board_uart_console_write(msg, len);
}

/**
 * @brief Halt execution on fatal error.
 */
static void flip_panic_halt(void)
{
  flip_log(k_flip_fail_msg, sizeof(k_flip_fail_msg) - 1U);
  (void)ra8_board_led_on(k_ra8_board_led1);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize CGC, console, LEDs, and clocks.
 */
static void flip_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_flip_baud,
    .leds_mask    = (uint32_t)k_ra8_board_bringup_led1 | (uint32_t)k_ra8_board_bringup_led2,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    flip_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    flip_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    flip_panic_halt();
  }
#ifndef RA8_OFF_TARGET
  if (flip_init_usbx_stack() != k_ra8_ok) {
    flip_panic_halt();
  }
#endif
}

/**
 * @brief Run one full sequence of Config A -> Teardown -> Config B -> Teardown.
 *
 * @return ra8_err_t outcome.
 */
[[nodiscard]] static ra8_err_t flip_execute_ladder(void)
{
  /* Phase 1: FS Device + HS Host */
  s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_cfg_a;
  ra8_err_t err    = usb_role_flip_setup_cfg_a();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_flip_settle_ms);

  s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_teardown1;
  err              = usb_role_flip_teardown_cfg_a();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_flip_settle_ms);

  /* Phase 2: Role flip -> FS Host + HS Device */
  s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_cfg_b;
  err              = usb_role_flip_setup_cfg_b();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_flip_settle_ms);

  s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_teardown2;
  err              = usb_role_flip_teardown_cfg_b();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_flip_settle_ms);

  s_dbg_flip_state = (uint32_t)k_usb_role_flip_phase_pass;
  s_dbg_pass_count++;
  return k_ra8_ok;
}

void main(void)
{
  flip_setup_or_halt();
  ra8_isr_globals_enable();

  flip_log(k_flip_start_msg, sizeof(k_flip_start_msg) - 1U);

  const ra8_err_t err = flip_execute_ladder();
  if (err != k_ra8_ok) {
    flip_panic_halt();
  }

  (void)ra8_board_led_on(k_ra8_board_led2);
  flip_log(k_flip_pass_msg, sizeof(k_flip_pass_msg) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
