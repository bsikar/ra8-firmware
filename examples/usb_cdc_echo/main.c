/**
 * @file main.c
 * @brief USB CDC ACM echo smoke test for EK-RA8D2 (USB-FS controller)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual ("USB Full-Speed Connector" pinout table)
 * to the on-board USB-FS receptacle, and brings the device-mode CDC
 * ACM stack up via the NSC USB veneers + the ``ra_usb_cdc`` class
 * layer. Once the host enumerates the device the firmware drops into
 * a tight loop that drains bulk-OUT bytes and re-queues them on
 * bulk-IN, toggling LED1 (P6_00) on every byte echoed so the user
 * can see traffic on the wire.
 *
 * ## Pinout (USB-FS, from FSP example_projects/ek_ra8d2/usb_pcdc)
 *
 * | Net           | Pin    | PFS PSEL                | Notes                         |
 * |---------------|--------|-------------------------|-------------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) | VBUS sense (input).           |
 * | USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) | VBUS-enable drive (output).   |
 * | USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) | D+ data line (analog buffer). |
 * | USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) | D- data line (analog buffer). |
 *
 * ## Sequence
 *
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init(cpuclk0_hz)`` for the LED toggle and any
 *      back-off delays.
 *   3. ``ra_pfs_route_peripheral()`` for the four USB-FS pins above
 *      (PSEL = ``k_ra_psel_usb_fs``).
 *   4. ``ra_gpio_output_init(k_ra_pin_led1, low)`` for the visual
 *      heartbeat / per-byte traffic indicator.
 *   5. ``ra_nsc_usb_init(k_ra_usb_speed_fs)`` -- NSC veneer that
 *      lands inside the secure-side ``ra_usb_device_init`` and brings
 *      up the USBFS controller, MSTP, SYSCFG.SCKE / DRPD / USBE,
 *      and unmasks BEMPE | BRDYE | NRDYE | DVSE | CTRE | VBSE.
 *   6. ``ra_usb_cdc_init(k_ra_usb_speed_fs)`` -- configures PIPE1
 *      (bulk IN, EP1 IN), PIPE2 (bulk OUT, EP2 OUT), PIPE6
 *      (interrupt IN, EP3 IN), seeds 9600/8/N/1 line coding.
 *   7. ``ra_nsc_usb_attach(k_ra_usb_speed_fs, true)`` -- raise the
 *      D+ pull-up so the host begins enumeration.
 *   8. Loop: ``ra_usb_cdc_recv`` -> ``ra_usb_cdc_send`` echo, with
 *      an LED1 toggle per byte echoed.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) should
 * enumerate as ``/dev/cu.usbmodem*``. Open it RDWR with picocom or
 * screen and type characters; every byte should echo back and LED1
 * should toggle once per byte. The line-coding and DTR/RTS bits
 * are accepted but ignored -- the firmware is a pure byte mirror.
 *
 * ## VID / PID
 *
 * For local hardware bring-up only, the firmware is wired to the
 * pid.codes test-and-experimentation block:
 *
 *   - VID = 0x1209  (pid.codes free-for-experiments range)
 *   - PID = 0x000A  (locally chosen, do not redistribute)
 *
 * These are strictly for laptop-side enumeration; do not ship this
 * firmware on hardware that will leave the bench. The descriptor
 * strings are "Brighton Sikarskie" / "EK-RA8D2 CDC Echo".
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_nsc_comms.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_cdc.h"

/**
 * @brief USB-FS pinout (FSP-aligned, EK-RA8D2 v1 User's Manual,
 *        cross-checked against
 *        ra-fsp-examples/example_projects/ek_ra8d2/usb_pcdc/).
 *
 * @details Each value is a packed ``ra_port_pin_t`` (port << 8 | pin):
 *
 *   - P4_07 -- USB_FS_VBUS sense.
 *   - P5_00 -- USB_FS_VBUSEN drive.
 *   - P8_14 -- USB_FS_DP.
 *   - P8_15 -- USB_FS_DM.
 *
 * Built the same way ``uart_hello`` builds its packed pins: a
 * runtime ``(port << 8) | pin`` expression cast to ``ra_port_pin_t``.
 * This keeps the static analyser happy because the cast result is a
 * deterministic 16-bit identifier rather than an out-of-enum-range
 * value rejected by clang-tidy's enum-range check.
 */
static const ra_port_pin_t k_usb_cdc_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_usb_cdc_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_usb_cdc_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_usb_cdc_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/**
 * @enum usb_cdc_echo_buf_t
 * @brief Sizing constants for the echo loop.
 */
typedef enum : uint16_t {
  k_usb_cdc_echo_buf_bytes = 64U, /**< One bulk-FS packet per recv/send. */
} usb_cdc_echo_buf_t;

/**
 * @enum usb_cdc_echo_idle_ms_t
 * @brief Idle back-off when no bytes are queued.
 *
 * @details The CDC bulk pipes are NAK-driven; if no host token has
 * landed there is nothing to do. We sleep a short period so the
 * panic-halt path stays reachable and so the LED is not held high
 * by a tight spin.
 */
typedef enum : uint8_t {
  k_usb_cdc_echo_idle_ms = 1U,
} usb_cdc_echo_idle_ms_t;

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void usb_cdc_echo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins (VBUS, VBUSEN, D+, D-) to the
 *        USBFS controller via the PFS PSEL field.
 *
 * @details
 * All four pins use ``k_ra_psel_usb_fs`` (PSEL = 0x13). The PFS
 * routing also enables the analog buffer for D+/D- inside the IOPORT
 * automatically because PSEL = 0x13 selects the USB-FS analog block
 * per HUM Ch 19.2.5 ("PFS PSEL field encoding").
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                     All four pins routed.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed by another owner.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_cdc_echo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_usb_cdc_pin_vbus, k_ra_psel_usb_fs, "usb_cdc_echo.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_cdc_pin_vbusen, k_ra_psel_usb_fs, "usb_cdc_echo.vbusen");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_cdc_pin_dp, k_ra_psel_usb_fs, "usb_cdc_echo.dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_cdc_pin_dm, k_ra_psel_usb_fs, "usb_cdc_echo.dm");
}

/**
 * @brief Bring CGC + SysTick + USB pin mux + LED1 + USB-CDC up.
 *        Panic-halts on any failure.
 *
 * @details
 * Mirrors uart_hello's setup pattern but swaps the SCI bring-up for
 * the USB-FS bring-up: pin-mux first, then ``ra_nsc_usb_init`` to
 * release the controller's MSTP gate and unmask its event sources,
 * then ``ra_usb_cdc_init`` to install the bulk + interrupt pipes and
 * seed line coding, then ``ra_nsc_usb_attach`` to raise the D+
 * pull-up so the host begins enumeration.
 *
 * @since 0.1.0
 */
static void usb_cdc_echo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }
  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }
  if (usb_cdc_echo_pins_init() != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }

  /* USB controller bring-up via the NSC veneer: lands in
   * ra_usb_device_init which releases the USBFS MSTP gate, drives
   * SYSCFG.SCKE high, clears DRPD, sets USBE, programs the DCP
   * max-packet to 64, and unmasks the device-mode interrupt set. */
  if (ra_nsc_usb_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }

  /* CDC ACM class layer: configures PIPE1 (bulk IN, EP1 IN, 64 B),
   * PIPE2 (bulk OUT, EP2 OUT, 64 B), PIPE6 (interrupt IN, EP3 IN,
   * 8 B), seeds 9600/8/N/1 line coding, DTR/RTS = 0. */
  if (ra_usb_cdc_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }

  /* Raise D+ pull-up so the host starts enumeration (SET_ADDRESS,
   * GET_DESCRIPTOR, SET_CONFIGURATION). VID = 0x1209 / PID = 0x000A
   * are documented in this file's header and the README. */
  if (ra_nsc_usb_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    usb_cdc_echo_panic_halt();
  }
}

/**
 * @brief Drain bulk-OUT and re-queue on bulk-IN, toggling LED1 per
 *        byte echoed.
 *
 * @details
 * One iteration of the echo loop. Returns ``k_ra_ok`` if either the
 * pipe was empty (k_ra_err_no_data) or the bytes were forwarded.
 * Any other failure escalates so the caller can panic-halt.
 *
 * @return Error code.
 *
 * @retval k_ra_ok        Bytes echoed (or pipe was empty).
 * @retval other          Underlying CDC failure.
 *
 * @pre ``ra_usb_cdc_init`` succeeded.
 *
 * @post On success the OUT bytes have been queued back on the IN pipe.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_cdc_echo_pump_once(void)
{
  uint8_t  buf[k_usb_cdc_echo_buf_bytes] = {};
  uint16_t len                           = k_usb_cdc_echo_buf_bytes;

  ra_err_t err = ra_usb_cdc_recv(buf, &len);
  if (err == k_ra_err_no_data) {
    return k_ra_ok; /* Pipe was empty -- nothing to do this tick. */
  }
  if (err != k_ra_ok) {
    return err;
  }
  if (len == 0U) {
    return k_ra_ok;
  }

  err = ra_usb_cdc_send(buf, len);
  if (err != k_ra_ok) {
    return err;
  }

  /* One LED1 toggle per byte echoed -- visual confirmation of every
   * byte landing on the host. At small chunk sizes this is
   * dominated by USB latency so the LED stays human-visible. */
  for (uint16_t i = 0U; i < len; i++) {
    if (ra_gpio_toggle(k_ra_pin_led1) != k_ra_ok) {
      return k_ra_err_gpio_invalid_pin;
    }
  }
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS + CDC ACM, then
 *        enters the byte-echo loop forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the recv -> send -> toggle
 *       loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  usb_cdc_echo_setup_or_halt();

  ra_isr_globals_enable();

  while (1) {
    if (usb_cdc_echo_pump_once() != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_usb_cdc_echo_idle_ms);
  }

  usb_cdc_echo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
