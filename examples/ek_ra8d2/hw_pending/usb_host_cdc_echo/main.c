/**
 * @file examples/ek_ra8d2/usb_host_cdc_echo/main.c
 * @brief USB host-mode CDC ACM echo smoke test for EK-RA8D2 (USB-HS controller)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Inverse of `usb_cdc_echo`: that app turns the EK-RA8D2 into a USB
 * device (laptop sees a `/dev/cu.usbmodem*`); this app turns the
 * EK-RA8D2 into a USB **host** that enumerates an attached CDC-ACM
 * peripheral on the USB-HS receptacle (J7 Type-C on the EK-RA8D2 v1
 * board) and then runs a byte-mirror echo loop over its bulk pipes.
 *
 * Sequence:
 *
 *   1. `ra_cgc_init()` -- XTAL -> PLL1, CPUCLK0 = 1 GHz, PCLKA = 125
 *      MHz, SCICLK = PLL1R / 4 = 100 MHz. Same tree uart_hello uses.
 *   2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
 *   3. `ra_pfs_route_peripheral()` for the J-Link OB CDC bridge pins
 *      (PD_02 = TXD8, PD_03 = RXD8) and `ra_sci_init(8, &cfg)` so the
 *      board can print enumeration progress over 115200 8N1.
 *   4. `ra_pfs_route_peripheral()` for P4_08 with PSEL = 0x14
 *      (USBHS_VBUS sense). That is the only PFS pin the EK-RA8D2 v1
 *      USB-HS port needs; the HS PHY's data lines (USBHSDP /
 *      USBHSDM / USBHSRREF) are dedicated package balls and do not
 *      route through PFS. Cross-checked against
 *      `ra-fsp-examples/example_projects/ek_ra8d2/usb_hcdc/`.
 *   5. `ra_gpio_output_init(k_ra_pin_led1)` and
 *      `ra_gpio_output_init(k_ra_pin_led2)` for the two visual
 *      indicators: LED1 lights when a CDC-ACM device attaches, LED2
 *      toggles per byte echoed.
 *   6. `ra_usb_hcdc_init(k_ra_usb_speed_hs)` -- flips the USBHS
 *      controller into host mode through `ra_usb_host_init` (HSE
 *      bit set in SYSCFG, DCFM = 1, DRPD = 1, USBE = 1, DCP loaded
 *      with 64-byte max-packet) and arms the chapter-9 enumeration
 *      step machine.
 *   7. `ra_usb_hcdc_attach_callback(on_attach, ctx)` registers a
 *      thunk that fires once the descriptor walk finds a CDC-ACM
 *      function on the attached peripheral. The thunk records the
 *      device snapshot and lights LED1.
 *   8. Main loop:
 *        - `ra_usb_hcdc_recv(buf, sizeof buf, &got)` drains the
 *          attached device's bulk-IN pipe.
 *        - `ra_usb_hcdc_send(buf, got)` re-queues the same bytes on
 *          the bulk-OUT pipe so the peripheral sees them echoed
 *          back.
 *        - `ra_board_led_toggle(k_ra_board_led2)` per byte echoed.
 *        - 1 ms back-off if the pipe is empty.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1 and watch
 * the `ra8d2 host: ...` lines stream past. Plug a CDC-ACM peripheral
 * (another EK-RA8D2 running `usb_cdc_echo`, a USB-to-serial dongle,
 * or a Linux phone in CDC-ACM mode) into the EK-RA8D2's J7 Type-C
 * USB-HS port. Once enumeration completes, every byte the peripheral
 * sends comes straight back at it. LED1 (P6_00) lights on attach;
 * LED2 (P3_03) toggles per echoed byte.
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_hcdc.h"

/* =============================================================================
 * Compile-time configuration
 * =============================================================================
 */

/**
 * @enum usb_host_cdc_echo_config_t
 * @brief Compile-time settings for the host-CDC echo demo.
 */
typedef enum : uint32_t {
  k_usb_host_baud           = 115200U, /**< J-Link OB CDC log baud.        */
  k_usb_host_sci_channel    = 8U,      /**< SCI8 -> J-Link OB CDC bridge.  */
  k_usb_host_idle_ms        = 1U,      /**< Idle when bulk IN is empty.    */
  k_usb_host_print_interval = 64U,     /**< Print "echoed=N" every N bytes.*/
} usb_host_cdc_echo_config_t;

/**
 * @enum usb_host_cdc_echo_buf_t
 * @brief Echo-buffer sizing constants.
 *
 * @details The bulk-IN pipe at high speed transports up to 512 bytes
 * per packet. Sized to one max-HS-packet so a full bulk frame can be
 * drained in one `ra_usb_hcdc_recv` call.
 */
typedef enum : uint16_t {
  k_usb_host_echo_buf_bytes = 512U, /**< One HS bulk packet per drain. */
} usb_host_cdc_echo_buf_t;

/**
 * @enum usb_host_cdc_echo_psel_t
 * @brief PFS PSEL code for routing P4_08 to USBHS_VBUS.
 *
 * @details
 * HUM Ch 20.2 "PFS register PSEL field encoding" gives 0x14 for
 * USBHS. The shared `ra_psel_t` enum in `ra_gpio_constants.h` does
 * not yet expose a `k_ra_psel_usb_hs` symbol because the same
 * wire-level value (0x14) overlaps with QSPI on different pins; we
 * pin it here and document the cross-reference. `ra_mpc_psel_t` in
 * `ra_mpc.h` carries the same constant under
 * `k_ra_mpc_psel_usbhs`.
 */
typedef enum : uint8_t {
  k_usb_host_psel_usb_hs =
    (uint8_t)k_ra_psel_usb_hs, /* HUM Ch 20.6 "Peripheral Select Settings" p 855 */
} usb_host_cdc_echo_psel_t;

/* =============================================================================
 * Pin assignments
 * =============================================================================
 */

/**
 * @brief J-Link OB CDC TX pin. Same SCI8 channel uart_hello uses --
 *        the on-board J-Link OB exposes this as a virtual COM port.
 */
static const ra_port_pin_t k_usb_host_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/**
 * @brief J-Link OB CDC RX pin (paired with TX above).
 */
static const ra_port_pin_t k_usb_host_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief USBHS_VBUS sense (P4_08) -- the one PFS-muxed pin the
 *        USB-HS receptacle on EK-RA8D2 v1 needs.
 *
 * @details
 * Cross-checked against
 * `ra-fsp-examples/example_projects/ek_ra8d2/usb_hcdc/` which sets
 * `p408.usbhs.usbhs_vbus` and nothing else for USB-HS. The HS PHY
 * data lines (USBHSDP / USBHSDM / USBHSRREF) are dedicated package
 * pins on the BGA and bypass the PFS PSEL path entirely.
 */
static const ra_port_pin_t k_usb_host_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/* =============================================================================
 * Status messages over SCI8
 * =============================================================================
 */

/** @brief First message printed once SCI is up and host bring-up done. */
static const uint8_t k_usb_host_msg_ready[] =
  "ra8d2 host: ready, waiting for CDC-ACM device on USB-HS (J7)\r\n";

/** @brief Static prefix for the per-attach VID/PID print. */
static const uint8_t k_usb_host_msg_attach_pre[] = "ra8d2 host: device attached vid=0x";

/** @brief Mid-message separator for the per-attach print. */
static const uint8_t k_usb_host_msg_attach_mid[] = " pid=0x";

/** @brief Trailing CR/LF after the per-attach print. */
static const uint8_t k_usb_host_msg_crlf[] = "\r\n";

/** @brief Static prefix for the per-iteration echo counter print. */
static const uint8_t k_usb_host_msg_echo_pre[] = "ra8d2 host: bytes echoed=";

/* =============================================================================
 * Hex print sizing
 * =============================================================================
 */

/**
 * @enum usb_host_cdc_echo_hex_t
 * @brief Hex/decimal text-buffer sizing constants.
 */
typedef enum : uint8_t {
  k_usb_host_hex_chars_u16    = 4U,  /**< 16-bit value -> "ABCD".         */
  k_usb_host_hex_chars_u32    = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_usb_host_dec_chars_u32    = 10U, /**< Max digits for a 32-bit count.  */
  k_usb_host_hex_nibble_count = 4U,  /**< Bits per hex nibble.            */
} usb_host_cdc_echo_hex_t;

/**
 * @enum usb_host_cdc_echo_hex_mask_t
 * @brief Bit-mask constants used by the hex/decimal formatters.
 *
 * @details Wider type because masking against a 32-bit operand keeps
 * the integer-promotion rules consistent.
 */
typedef enum : uint32_t {
  k_usb_host_hex_nibble_mask = 0xFU, /**< 4-bit nibble mask.            */
  k_usb_host_dec_radix       = 10U,  /**< Base for decimal conversion.  */
  k_usb_host_hex_digit_split = 10U,  /**< Threshold between '0-9'/'A-F'.*/
} usb_host_cdc_echo_hex_mask_t;

/* =============================================================================
 * Attach state shared with the callback
 * =============================================================================
 */

/**
 * @struct usb_host_cdc_echo_state_t
 * @brief Mutable state set by the attach callback and read by main.
 */
typedef struct {
  bool                 attached; /**< True after attach callback fires. */
  ra_usb_hcdc_device_t device;   /**< Snapshot of the attached device.  */
} usb_host_cdc_echo_state_t;

/** @brief File-scope state shared between callback + main loop. */
static volatile usb_host_cdc_echo_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void usb_host_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Format a uint16_t into 4 uppercase hex characters.
 *
 * @param[in]  value The 16-bit value to format.
 * @param[out] out   Destination buffer; must hold at least 4 bytes.
 *
 * @pre `out` is non-NULL and has 4 bytes of storage.
 *
 * @post `out[0..3]` hold the big-endian hex digits of `value`.
 *
 * @since 0.1.0
 */
static void usb_host_format_hex_u16(uint16_t value, uint8_t* out)
{
  for (uint8_t i = 0U; i < k_usb_host_hex_chars_u16; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_host_hex_chars_u16 - 1U - i) * k_usb_host_hex_nibble_count);
    const uint32_t nibble = ((uint32_t)value >> shift) & k_usb_host_hex_nibble_mask;
    out[i] = (uint8_t)((nibble < k_usb_host_hex_digit_split)
                         ? ((uint8_t)'0' + (uint8_t)nibble)
                         : ((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_usb_host_hex_digit_split));
  }
}

/**
 * @brief Format a uint32_t as ASCII decimal into `out`, return digit count.
 *
 * @param[in]  value The 32-bit value to format.
 * @param[out] out   Destination buffer; must hold at least 10 bytes.
 *
 * @return Number of decimal digits written (1..10).
 *
 * @pre `out` is non-NULL and has 10 bytes of storage.
 *
 * @post Returns 1..10 indicating how many bytes of `out` were written.
 *
 * @since 0.1.0
 */
static uint8_t usb_host_format_decimal_u32(uint32_t value, uint8_t* out)
{
  uint8_t  scratch[k_usb_host_dec_chars_u32] = {};
  uint8_t  count                             = 0U;
  uint32_t v                                 = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return 1U;
  }
  while ((v != 0U) && (count < k_usb_host_dec_chars_u32)) {
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_usb_host_dec_radix));
    v              = v / k_usb_host_dec_radix;
    count++;
  }
  /* Reverse scratch into out. */
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return count;
}

/**
 * @brief Push a literal asciiz block over SCI8.
 *
 * @details Convenience wrapper that takes a `sizeof(arr) - 1` C string
 * literal and forwards it polled to the SCI8 channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 *
 * @pre `data` is non-NULL and `len` is the byte length excluding NUL.
 *
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_usb_host_sci_channel, data, len);
}

/**
 * @brief Print "device attached vid=0xXXXX pid=0xXXXX" to SCI8.
 *
 * @param[in] vid USB vendor ID from the device descriptor.
 * @param[in] pid USB product ID from the device descriptor.
 *
 * @return ra_err_t First non-OK from any underlying SCI write.
 *
 * @retval k_ra_ok All four chunks successfully transmitted.
 * @retval other   Underlying SCI failure on the first failed chunk.
 *
 * @pre `ra_sci_init` has run.
 *
 * @post One full ASCII "vid=0x.. pid=0x..\r\n" line has been queued.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_print_attach(uint16_t vid, uint16_t pid)
{
  uint8_t hex_vid[k_usb_host_hex_chars_u16] = {};
  uint8_t hex_pid[k_usb_host_hex_chars_u16] = {};
  usb_host_format_hex_u16(vid, hex_vid);
  usb_host_format_hex_u16(pid, hex_pid);

  ra_err_t err = usb_host_sci_write(k_usb_host_msg_attach_pre,
                                    (uint32_t)(sizeof(k_usb_host_msg_attach_pre) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_host_sci_write(hex_vid, (uint32_t)k_usb_host_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_host_sci_write(k_usb_host_msg_attach_mid,
                           (uint32_t)(sizeof(k_usb_host_msg_attach_mid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_host_sci_write(hex_pid, (uint32_t)k_usb_host_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_host_sci_write(k_usb_host_msg_crlf, (uint32_t)(sizeof(k_usb_host_msg_crlf) - 1U));
}

/**
 * @brief Print "bytes echoed=N\r\n" to SCI8.
 *
 * @param[in] count Total number of bytes echoed since attach.
 *
 * @return ra_err_t First non-OK from any underlying SCI write.
 *
 * @retval k_ra_ok All chunks queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre `ra_sci_init` has run.
 *
 * @post One full ASCII "bytes echoed=N\r\n" line has been queued.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_print_echoed(uint32_t count)
{
  uint8_t       dec[k_usb_host_dec_chars_u32] = {};
  const uint8_t dec_len                       = usb_host_format_decimal_u32(count, dec);

  ra_err_t err =
    usb_host_sci_write(k_usb_host_msg_echo_pre, (uint32_t)(sizeof(k_usb_host_msg_echo_pre) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_host_sci_write(dec, (uint32_t)dec_len);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_host_sci_write(k_usb_host_msg_crlf, (uint32_t)(sizeof(k_usb_host_msg_crlf) - 1U));
}

/* =============================================================================
 * Pin / SCI / USB bring-up
 * =============================================================================
 */

/**
 * @brief Route the SCI8 J-Link OB CDC pins (PD_02 = TX, PD_03 = RX).
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                     Both pins routed.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success PD_02 and PD_03 are SCI-async (PSEL = 0x04).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_pins_sci_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_usb_host_pin_sci_tx, k_ra_psel_sci_async, "usb_host_cdc_echo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_host_pin_sci_rx,
                                 k_ra_psel_sci_async,
                                 "usb_host_cdc_echo.rxd8");
}

/**
 * @brief Route the USBHS_VBUS sense pin (P4_08, PSEL = 0x14).
 *
 * @details
 * The HS PHY's data lines (USBHSDP / USBHSDM / USBHSRREF) are
 * dedicated package balls and need no PFS routing; only P4_08 needs
 * to be muxed for USBHS_VBUS sense.
 *
 * @return Error code from `ra_pfs_route_peripheral`.
 *
 * @retval k_ra_ok                     P4_08 is now USBHS_VBUS.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success P4_08 PFS PSEL = 0x14, PMR = 1.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_pins_hs_init(void)
{
  return ra_pfs_route_peripheral(k_usb_host_pin_hs_vbus,
                                 (ra_psel_t)k_usb_host_psel_usb_hs,
                                 "usb_host_cdc_echo.hs_vbus");
}

/**
 * @brief Attach callback -- store the device snapshot and light LED1.
 *
 * @param[in] ctx    Caller context (unused; the callback writes to the
 *                   file-scope `s_state`).
 * @param[in] device Pointer to the attach snapshot. The HAL guarantees
 *                   it is valid for the duration of this call.
 *
 * @pre Driver was initialized via `ra_usb_hcdc_init`.
 *
 * @post `s_state.attached` is true, `s_state.device` carries the
 *       VID/PID/EP info, and LED1 is driven high.
 *
 * @note Invoked from dispatch context (typically the controller's
 *       interrupt). The body does not touch `ra_sci` because that
 *       would polling-wait inside an ISR; the SCI print is left to
 *       the main loop.
 *
 * @since 0.1.0
 */
static void usb_host_on_attach(void* ctx, const ra_usb_hcdc_device_t* device)
{
  (void)ctx;
  if (device == nullptr) {
    return;
  }
  s_state.device   = *device;
  s_state.attached = true;
  /* `ra_gpio_write` is the right call here -- toggling per-attach
   * could land in either state if the loop has already toggled
   * LED1 in some earlier event. We force LED1 high to indicate
   * "device attached and ready". */
  (void)ra_board_led_on(k_ra_board_led1);
}

/**
 * @brief Bring CGC + SCI + LED1/LED2 + USBHS pins + host-CDC up.
 *        Panic-halts on any failure.
 *
 * @details
 * Ordered the same way uart_hello / usb_cdc_echo order their setup:
 * clock first, then SysTick, then SCI for log output, then GPIO
 * outputs, then USB pins, then the host-CDC class driver, finally
 * the attach callback.
 *
 * @since 0.1.0
 */
static void usb_host_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_host_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_host_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    usb_host_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_host_panic_halt();
  }

  /* SCI8 first so subsequent failures can report. */
  if (usb_host_pins_sci_init() != k_ra_ok) {
    usb_host_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_usb_host_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_usb_host_sci_channel, &sci_cfg) != k_ra_ok) {
    usb_host_panic_halt();
  }

  /* LEDs. */
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    usb_host_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    usb_host_panic_halt();
  }

  /* USB-HS VBUS pin (the only PFS-muxed USBHS pin on EK-RA8D2 v1). */
  if (usb_host_pins_hs_init() != k_ra_ok) {
    usb_host_panic_halt();
  }

  /* Host-CDC class layer up against the USBHS controller. This calls
   * `ra_usb_host_init(k_ra_usb_speed_hs)` underneath which gates the
   * USBHS MSTP, sets DCFM/DRPD/USBE, and arms the chapter-9 step
   * machine. */
  if (ra_usb_hcdc_init(k_ra_usb_speed_hs) != k_ra_ok) {
    usb_host_panic_halt();
  }
  if (ra_usb_hcdc_attach_callback(usb_host_on_attach, nullptr) != k_ra_ok) {
    usb_host_panic_halt();
  }
}

/* =============================================================================
 * Echo loop
 * =============================================================================
 */

/**
 * @brief Drain bulk-IN, queue on bulk-OUT, toggle LED2 per byte.
 *
 * @param[in,out] echo_count Running count of bytes echoed since
 *                           attach. Updated on success.
 *
 * @return Error code.
 *
 * @retval k_ra_ok        Bytes echoed (or pipe was empty).
 * @retval other          Underlying host-CDC failure.
 *
 * @pre `ra_usb_hcdc_init` succeeded.
 * @pre Attach callback has fired (`s_state.attached == true`).
 * @pre `echo_count` is non-NULL.
 *
 * @post `*echo_count` is incremented by the number of bytes echoed.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_pump_once(uint32_t* echo_count)
{
  uint8_t  buf[k_usb_host_echo_buf_bytes] = {};
  uint16_t got                            = 0U;

  ra_err_t err = ra_usb_hcdc_recv(buf, k_usb_host_echo_buf_bytes, &got);
  if (err == k_ra_err_no_data) {
    return k_ra_ok;
  }
  if (err != k_ra_ok) {
    return err;
  }
  if (got == 0U) {
    return k_ra_ok;
  }

  err = ra_usb_hcdc_send(buf, got);
  if (err != k_ra_ok) {
    return err;
  }

  /* One LED2 toggle per byte echoed. At small chunk sizes this is
   * dominated by USB latency so the LED stays human-visible. */
  for (uint16_t i = 0U; i < got; i++) {
    if (ra_board_led_toggle(k_ra_board_led2) != k_ra_ok) {
      return k_ra_err_gpio_invalid_pin;
    }
  }
  *echo_count = *echo_count + (uint32_t)got;
  return k_ra_ok;
}

/**
 * @brief One iteration of the post-attach echo loop.
 *
 * @details Extracted from `main` to keep the main loop's nesting
 * level under the project's clang-tidy threshold. Each call drains
 * a chunk of bytes, re-queues them, and emits a periodic SCI status
 * line.
 *
 * @param[in,out] echoed     Running count of bytes echoed since attach.
 * @param[in,out] last_print Last `echoed` value the SCI status line
 *                           reflected. Updated when a print fires.
 *
 * @return ra_err_t propagated from the echo / SCI helpers.
 *
 * @retval k_ra_ok One iteration completed (with or without bytes).
 * @retval other   Underlying USB or SCI failure.
 *
 * @pre Attach callback already fired (`s_state.attached == true`).
 * @pre `echoed` and `last_print` are non-NULL.
 *
 * @post On success `*echoed` and `*last_print` reflect the new state.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_run_echo_step(uint32_t* echoed, uint32_t* last_print)
{
  const ra_err_t err = usb_host_pump_once(echoed);
  if (err != k_ra_ok) {
    return err;
  }
  /* Throttle the per-byte SCI counter so we do not flood the
   * J-Link CDC channel: print after each 64-byte chunk. */
  if ((*echoed - *last_print) < k_usb_host_print_interval) {
    return k_ra_ok;
  }
  const ra_err_t print_err = usb_host_print_echoed(*echoed);
  if (print_err != k_ra_ok) {
    return print_err;
  }
  *last_print = *echoed;
  return k_ra_ok;
}

/**
 * @brief Announce the attach event over SCI8 (printed once).
 *
 * @details Reads VID / PID from `s_state.device` and emits one
 * "ra8d2 host: device attached vid=... pid=..." line.
 *
 * @return ra_err_t propagated from `usb_host_print_attach`.
 *
 * @retval k_ra_ok Attach line queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre Attach callback already fired (`s_state.attached == true`).
 *
 * @post One ASCII line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_host_announce_attach(void)
{
  const uint16_t vid = s_state.device.vendor_id;
  const uint16_t pid = s_state.device.product_id;
  return usb_host_print_attach(vid, pid);
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up host-CDC then echo-loops.
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
  usb_host_setup_or_halt();

  ra_isr_globals_enable();

  /* Print "ready" once SCI is alive. */
  if (usb_host_sci_write(k_usb_host_msg_ready, (uint32_t)(sizeof(k_usb_host_msg_ready) - 1U)) !=
      k_ra_ok) {
    usb_host_panic_halt();
  }

  bool     announced  = false;
  uint32_t echoed     = 0U;
  uint32_t last_print = 0U;

  while (1) {
    if (s_state.attached && !announced) {
      if (usb_host_announce_attach() != k_ra_ok) {
        break;
      }
      announced = true;
    }
    if (s_state.attached) {
      if (usb_host_run_echo_step(&echoed, &last_print) != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms(k_usb_host_idle_ms);
  }

  usb_host_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
