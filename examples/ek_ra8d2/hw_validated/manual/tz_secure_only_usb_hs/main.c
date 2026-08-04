/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/main.c
 * @brief Secure-world-only ThreadX + USBX CDC ACM echo for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * High-Speed sibling of ``tz_secure_only_usb_fs`` (USB-FS on J11). This
 * variant brings up the USB 2.0 High-Speed controller (USBHS @
 * 0x40351000, HUM Ch 37) so the EK-RA8D2 USB-C receptacle (J7)
 * enumerates as a CDC-ACM device on the host. The class layer, the
 * USBX bridge in ``port/usbx/src/ux_dcd_ra8_usb.c`` and the register-level
 * driver in ``libs/ra8_hal/src/ra8_usb.c`` are already speed-
 * parameterised; this app simply selects ``k_ra8_usb_speed_hs`` and
 * uses the HS-specific board bring-up entry
 * ``ra8_board_usbhs_device_init`` (which arms the PHY 12 MHz reference
 * via ``ra8_cgc_usbhs_pll_enable`` and ungates MSTPB12 USBHS before
 * calling ``ra8_usb_device_init(k_ra8_usb_speed_hs)``).
 *
 * Once enumerated, the worker thread loops on
 * ``_ux_device_class_cdc_acm_read`` -> ``_ux_device_class_cdc_acm_write``
 * (echo). LED1 toggles per byte echoed.
 *
 * ## Pinout (USB-HS, EK-RA8D2 v1 User's Manual Rev 1.01 sec 6.2 p 34)
 *
 * The HS PHY data lines (USBH_P / USBH_N / USBHSRREF) are dedicated
 * package balls on the BGA and bypass the PFS PSEL path entirely.
 * Only one PFS-muxed pin needs routing for HS device-mode operation:
 *
 * | Net           | Pin    | PFS PSEL                |
 * |---------------|--------|-------------------------|
 * | USBHS_VBUS    | P4_08  | k_ra8_psel_usb_hs (0x14) |
 *
 * The board's J7 role-select GPIO (PD07, set LOW for device mode) is
 * pulled LOW by default on the EK-RA8D2 v1 -- the firmware does not
 * have to drive it. VBUS / VBUSEN / OVRCUR are sourced from the
 * on-board USB-PD controller, not from RA8D2 port pins, so no further
 * GPIO setup is required.
 *
 * ## Sequence
 *
 *   1. ``ra8_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra8_time_init`` for back-off delays.
 *   3. ``ra8_pfs_route_peripheral`` for P4_08 -> USBHS_VBUS.
 *   4. ``ra8_board_led_init(k_ra8_board_led1)`` for visual heartbeat.
 *   5. ThreadX ``tx_kernel_enter()`` -- spins the scheduler.
 *   6. ``tx_application_define`` -- spawns one worker thread that:
 *        - Allocates USBX memory pool and calls
 *          ``_ux_system_initialize`` + ``_ux_device_stack_initialize``.
 *        - Calls ``_ux_device_stack_class_register`` for the CDC-ACM
 *          class.
 *        - Calls ``ra8_board_usbhs_device_init()`` to arm the HS PHY
 *          clock + MSTP and bring up the USBHS controller.
 *        - Calls ``ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_hs)`` to
 *          plug our DCD bridge into USBX (which announces the
 *          highest-speed class as ``UX_HIGH_SPEED_DEVICE``).
 *        - Calls ``ra8_usb_device_attach(k_ra8_usb_speed_hs, true)`` so
 *          the host begins enumeration.
 *        - Drops into the echo loop.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-HS receptacle (J7) enumerates
 * as ``/dev/cu.usbmodem*``. Open it RDWR with picocom or screen and
 * type characters; every byte echoes back and LED1 toggles per byte.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"
#include "tz_secure_only_usb_hs_steps.h"

/**
 * @var s_demo_tag
 * @brief Log tag for this experiment's diagnostics on SCI8 / RTT.
 * @note File-scope, read-only after init.
 * @since 0.1.0
 */
static const char* s_demo_tag = "TZSECONLYHS";

/* -------------------------------------------------------------------------- */
/* Pinout (USBHS, EK-RA8D2 v1 User's Manual Rev 1.01 sec 6.2 p 34) */
/* -------------------------------------------------------------------------- */

/**
 * @brief USBHS_VBUS sense pin (P4_08), packed ``ra8_port_pin_t``.
 *
 * @details
 * Built as a runtime cast so clang-tidy's enum-range check is happy
 * with the otherwise out-of-enum value. Cross-checked against the FSP
 * example ``ra-fsp-examples/example_projects/ek_ra8d2/usb_hcdc/``
 * which sets ``p408.usbhs.usbhs_vbus`` and nothing else for USB-HS.
 *
 * @since 0.1.0
 */
static const ra8_port_pin_t k_demo_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/**
 * @brief J7 USB-HS role-select strap (PD07), packed ``ra8_port_pin_t``.
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34: PD07 (port 13 / pin 7) is
 * the J7 USB-HS role select line. Driving it LOW selects Device mode;
 * driving it HIGH selects Host mode. The board does not pull this pin
 * to any default level by hardware -- the firmware MUST own it.
 *
 * Built as a runtime cast so clang-tidy's enum-range check is happy
 * with the otherwise out-of-enum value.
 *
 * @since 0.1.0
 */
static const ra8_port_pin_t k_demo_pin_pd07_role = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Startup helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the USBHS_VBUS sense pin (P4_08) to the USBHS controller.
 *
 * @return Error from ra8_pfs_route_peripheral, or k_ra8_ok.
 * @retval k_ra8_ok P4_08 is in USBHS peripheral mode (PSEL = 0x14).
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success P4_08 PFS PSEL = 0x14, PMR = 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_pins_init(void)
{
  /* The HS PHY data lines (USBHSDP / USBHSDM / USBHSRREF) are
   * dedicated package balls on the BGA and bypass the PFS PSEL path.
   * VBUS / VBUSEN / OVRCUR are sourced by the on-board USB-PD
   * controller. So only P4_08 needs PFS routing for the controller. */
  ra8_err_t err =
    ra8_pfs_route_peripheral(k_demo_pin_hs_vbus, k_ra8_psel_usb_hs, "usb_cdc_hs.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  /* PD07 (J7 USB-HS role select, UM 6.2 p 34): drive LOW for Device
   * mode. Mirrors the FS demo's P5_00 / VBUSEN GPIO drive in shape:
   * the role line is owned by the application early so the analog
   * block sees a stable strap before the controller is brought up.
   * Previously this happened inside ra8_board_usbhs_device_init from
   * the worker thread, but that caller also invoked ra8_usb_device_init
   * which is then re-invoked by ux_dcd_ra8_usb_initialize -- the
   * duplicate PHY bring-up is what kept INTENB0 from sticking and is
   * the most plausible reason the host enumerates FS but never sends
   * SETUP on HS. Owning PD07 here lets the worker drop the redundant
   * board init call entirely. */
  return ra8_gpio_output_init(k_demo_pin_pd07_role, k_ra8_level_low);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-HS pin + LED1 + ThreadX.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }

  /* Bring up the USBHS PHY clock (USB60CKCR / USB60CLK = PLL2P / 4 =
   * 60 MHz) BEFORE any caller releases MSTPB12 (USBHS) -- mirrors the
   * FS demo's ra8_cgc_usbfs_clock_enable() pattern. The bridge's
   * ra8_usb_device_init invocation later releases MSTPB12 and immediately
   * starts the PHY-PLL CLKSEL bisect; without this clock arm step the
   * PHY block has no reference and PLLLOCK never asserts.
   *
   * Previously this lived inside ra8_board_usbhs_device_init() called
   * from the worker, but that wrapper also called ra8_usb_device_init
   * which is then re-invoked by ux_dcd_ra8_usb_initialize -- the
   * duplicate PHY bring-up clobbered INTENB0 and is the most plausible
   * explanation for the "ISR fires but host never sends SETUP" symptom
   * on HS. With the clock armed here the worker can drop the wrapper
   * call entirely, making the HS demo's worker-side init sequence a
   * structural mirror of the working FS demo. */
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    demo_panic_halt();
  }

  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  ra8_log_init();
  ra8_log_info(s_demo_tag, "tz_secure_only_usb_hs boot, CGC OK, USBHS pin routed");

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
