/**
 * @file main.c
 * @brief USB host-mode MSC (Mass Storage) browser smoke test for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Hardware-test app for the host-side MSC class layer (`ra_usb_hmsc.c`).
 * Turns the EK-RA8D2 into a USB host on its USB-HS receptacle (J7 Type-C
 * on EK-RA8D2 v1) so the operator can plug a USB mass-storage device
 * (thumb drive) into it and watch the descriptor walk + first-sector
 * dump come out of the J-Link OB CDC virtual COM port at 115200 8N1.
 *
 * Sequence:
 *
 *   1. `ra_cgc_init()` -- XTAL -> PLL1, CPUCLK0 = 1 GHz, PCLKA = 125
 *      MHz. Same tree uart_hello uses.
 *   2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
 *   3. `ra_pfs_route_peripheral()` for the J-Link OB CDC bridge pins
 *      (PD_02 = TXD8, PD_03 = RXD8) and `ra_sci_init(8, ...)` so the
 *      board can print enumeration progress over 115200 8N1.
 *   4. `ra_pfs_route_peripheral()` for P4_08 with PSEL = 0x14
 *      (USBHS_VBUS sense). The HS PHY data lines are dedicated package
 *      balls and bypass the PFS path.
 *   5. `ra_gpio_output_init(k_ra_pin_led1)` and
 *      `ra_gpio_output_init(k_ra_pin_led2)`. LED1 lights when an MSC
 *      device attaches; LED2 toggles per SCSI op.
 *   6. `ra_usb_hmsc_init(k_ra_usb_speed_hs)` -- flips USBHS to host
 *      mode and arms the chapter-9 step machine.
 *   7. `ra_usb_hmsc_attach_callback(on_attach, ...)` -- on attach the
 *      callback records the device snapshot, lights LED1.
 *   8. After attach, main runs:
 *        - INQUIRY               (LED2 toggles)
 *        - READ_CAPACITY(10)     (LED2 toggles)
 *        - READ(10) on LBA 0     (LED2 toggles)
 *      and dumps the first 64 bytes of the device's MBR over SCI8 in
 *      hex. From there the loop idles in WFI; the dump only runs once.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1
 * (`picocom -b 115200 /dev/cu.usbmodem*`), reset the EVM, plug a USB
 * stick into J7. The line "ra8d2 host: device attached vid=... pid=...
 * max-lun=N" prints, INQUIRY vendor/product strings show up, capacity
 * is reported, and 64 bytes of the MBR (sector 0) are printed in hex.
 * On a typical thumb drive the last two bytes (offsets 510/511, in row
 * 32 of a full dump; for the truncated 64-byte view they are not yet
 * visible) form the boot signature 55 AA -- per the EFI / Microsoft
 * MBR layout spec.
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
#include "ra_usb_hmsc.h"

/* =============================================================================
 * Compile-time configuration
 * =============================================================================
 */

/**
 * @enum usb_host_msc_browse_config_t
 * @brief Compile-time settings for the host-MSC browse demo.
 */
typedef enum : uint32_t {
  k_usb_msc_baud        = 115200U, /**< J-Link OB CDC log baud.        */
  k_usb_msc_sci_channel = 8U,      /**< SCI8 -> J-Link OB CDC bridge.  */
  k_usb_msc_idle_ms     = 50U,     /**< Idle while waiting for attach. */
} usb_host_msc_browse_config_t;

/**
 * @enum usb_host_msc_dump_t
 * @brief Sizing constants for the MBR dump.
 */
typedef enum : uint16_t {
  k_usb_msc_block_size_bytes = 512U, /**< One SCSI default block.    */
  k_usb_msc_dump_bytes       = 64U,  /**< Bytes printed from sector. */
  k_usb_msc_dump_columns     = 16U,  /**< Hex bytes per dump row.    */
} usb_host_msc_dump_t;

/**
 * @enum usb_host_msc_psel_t
 * @brief PFS PSEL code for routing P4_08 to USBHS_VBUS.
 *
 * @details HUM Ch 20.2 "PFS register PSEL field encoding" gives 0x14
 * for USBHS.
 */
typedef enum : uint8_t {
  k_usb_msc_psel_usb_hs = 0x14U, /* HUM Ch 20.6 "Peripheral Select Settings" p 855 */
} usb_host_msc_psel_t;

/**
 * @enum usb_host_msc_hex_t
 * @brief Hex/decimal text-buffer sizing constants.
 */
typedef enum : uint8_t {
  k_usb_msc_hex_chars_u8     = 2U,  /**< 8-bit value -> "AB".            */
  k_usb_msc_hex_chars_u16    = 4U,  /**< 16-bit value -> "ABCD".         */
  k_usb_msc_hex_chars_u32    = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_usb_msc_dec_chars_u32    = 10U, /**< Max digits for a 32-bit count.  */
  k_usb_msc_hex_nibble_count = 4U,  /**< Bits per hex nibble.            */
} usb_host_msc_hex_t;

/**
 * @enum usb_host_msc_hex_mask_t
 * @brief Bit-mask constants used by the hex/decimal formatters.
 */
typedef enum : uint32_t {
  k_usb_msc_hex_nibble_mask = 0xFU, /**< 4-bit nibble mask.            */
  k_usb_msc_dec_radix       = 10U,  /**< Base for decimal conversion.  */
  k_usb_msc_hex_digit_split = 10U,  /**< Threshold between '0-9'/'A-F'.*/
} usb_host_msc_hex_mask_t;

/**
 * @enum usb_host_msc_lun_t
 * @brief Logical-unit constants.
 */
typedef enum : uint8_t {
  k_usb_msc_target_lun = 0U, /**< Browse LUN 0 (typical thumb drive). */
} usb_host_msc_lun_t;

/* =============================================================================
 * Pin assignments
 * =============================================================================
 */

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_usb_msc_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_usb_msc_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_usb_msc_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/* =============================================================================
 * Status messages over SCI8
 * =============================================================================
 */

/** @brief First message after SCI is up. */
static const uint8_t k_usb_msc_msg_ready[] = "ra8d2 host: ready, plug a USB drive into J7\r\n";

/** @brief Static prefix for the per-attach VID/PID print. */
static const uint8_t k_usb_msc_msg_attach_pre[] = "ra8d2 host: device attached vid=0x";

/** @brief Mid-message separator for the per-attach print (PID). */
static const uint8_t k_usb_msc_msg_attach_mid[] = " pid=0x";

/** @brief Mid-message separator for the per-attach print (max-LUN). */
static const uint8_t k_usb_msc_msg_attach_lun[] = " max-lun=";

/** @brief Trailing CR/LF after a print. */
static const uint8_t k_usb_msc_msg_crlf[] = "\r\n";

/** @brief Banner before the INQUIRY decode. */
static const uint8_t k_usb_msc_msg_inquiry[] = "ra8d2 host: INQUIRY vendor=\"";

/** @brief Mid-banner between vendor and product fields. */
static const uint8_t k_usb_msc_msg_inquiry_pid[] = "\" product=\"";

/** @brief Mid-banner between product and revision fields. */
static const uint8_t k_usb_msc_msg_inquiry_rev[] = "\" rev=\"";

/** @brief Tail of the inquiry banner. */
static const uint8_t k_usb_msc_msg_inquiry_tail[] = "\"\r\n";

/** @brief Banner before READ_CAPACITY decode. */
static const uint8_t k_usb_msc_msg_capacity[] = "ra8d2 host: capacity blocks=";

/** @brief Mid-banner between block-count and block-size in capacity. */
static const uint8_t k_usb_msc_msg_capacity_mid[] = " block_size=";

/** @brief Banner before the MBR dump. */
static const uint8_t k_usb_msc_msg_mbr[] = "ra8d2 host: MBR sector 0 first 64 bytes:\r\n";

/** @brief Banner if a SCSI command failed. */
static const uint8_t k_usb_msc_msg_scsi_err[] = "ra8d2 host: SCSI op failed err=0x";

/** @brief A single space (used to format the hex dump). */
static const uint8_t k_usb_msc_msg_space[] = " ";

/* =============================================================================
 * Attach state shared with the callback
 * =============================================================================
 */

/**
 * @struct usb_host_msc_state_t
 * @brief Mutable state set by the attach callback and read by main.
 */
typedef struct {
  bool                 attached; /**< True after attach callback fires. */
  ra_usb_hmsc_device_t device;   /**< Snapshot of the attached device.  */
} usb_host_msc_state_t;

/** @brief File-scope state shared between callback + main loop. */
static volatile usb_host_msc_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop.
 *
 * @pre Called only after a fatal error or after the one-shot dump.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void usb_msc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @param[in] nibble 4-bit value.
 *
 * @return ASCII '0'..'9' or 'A'..'F'.
 *
 * @pre Caller has already masked the value to 4 bits.
 *
 * @post Returned byte is in the printable hex range.
 *
 * @since 0.1.0
 */
static uint8_t usb_msc_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_usb_msc_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_usb_msc_hex_digit_split);
}

/**
 * @brief Format a uint8_t into 2 uppercase hex characters.
 *
 * @param[in]  value 8-bit value.
 * @param[out] out   Destination buffer (>=2 bytes).
 *
 * @pre `out` has 2 bytes of storage.
 *
 * @post `out[0..1]` hold the big-endian hex digits of `value`.
 *
 * @since 0.1.0
 */
static void usb_msc_format_hex_u8(uint8_t value, uint8_t* out)
{
  out[0] = usb_msc_nibble_to_hex(((uint32_t)value >> k_usb_msc_hex_nibble_count) &
                                 k_usb_msc_hex_nibble_mask);
  out[1] = usb_msc_nibble_to_hex((uint32_t)value & k_usb_msc_hex_nibble_mask);
}

/**
 * @brief Format a uint16_t into 4 uppercase hex characters.
 *
 * @param[in]  value 16-bit value.
 * @param[out] out   Destination buffer (>=4 bytes).
 *
 * @pre `out` has 4 bytes of storage.
 *
 * @post `out[0..3]` hold the big-endian hex digits of `value`.
 *
 * @since 0.1.0
 */
static void usb_msc_format_hex_u16(uint16_t value, uint8_t* out)
{
  for (uint8_t i = 0U; i < k_usb_msc_hex_chars_u16; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_msc_hex_chars_u16 - 1U - i) * k_usb_msc_hex_nibble_count);
    const uint32_t nibble = ((uint32_t)value >> shift) & k_usb_msc_hex_nibble_mask;
    out[i]                = usb_msc_nibble_to_hex(nibble);
  }
}

/**
 * @brief Format a uint32_t into ASCII decimal.
 *
 * @param[in]  value 32-bit value.
 * @param[out] out   Destination buffer (>=10 bytes).
 *
 * @return Number of decimal digits written (1..10).
 *
 * @pre `out` has 10 bytes of storage.
 *
 * @post Returns 1..10 indicating bytes written into `out`.
 *
 * @since 0.1.0
 */
static uint8_t usb_msc_format_decimal_u32(uint32_t value, uint8_t* out)
{
  uint8_t  scratch[k_usb_msc_dec_chars_u32] = {};
  uint8_t  count                            = 0U;
  uint32_t v                                = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return 1U;
  }
  while ((v != 0U) && (count < k_usb_msc_dec_chars_u32)) {
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_usb_msc_dec_radix));
    v              = v / k_usb_msc_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return count;
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 *
 * @pre `data` is non-NULL, `len` is the byte length excluding NUL.
 *
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_usb_msc_sci_channel, data, len);
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @param[in] value Value to print.
 *
 * @return ra_err_t propagated from the SCI helper.
 *
 * @retval k_ra_ok All bytes queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre SCI8 init already ran.
 *
 * @post One ASCII decimal token in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_dec_u32(uint32_t value)
{
  uint8_t       dec[k_usb_msc_dec_chars_u32] = {};
  const uint8_t dec_len                      = usb_msc_format_decimal_u32(value, dec);
  return usb_msc_sci_write(dec, (uint32_t)dec_len);
}

/**
 * @brief Print "ra8d2 host: device attached vid=0x.. pid=0x.. max-lun=N".
 *
 * @param[in] vid     USB vendor ID.
 * @param[in] pid     USB product ID.
 * @param[in] max_lun Get-Max-LUN response.
 *
 * @return ra_err_t propagated from the underlying SCI writes.
 *
 * @retval k_ra_ok All chunks queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre `ra_sci_init` ran.
 *
 * @post One ASCII line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_attach(uint16_t vid, uint16_t pid, uint8_t max_lun)
{
  uint8_t hex_vid[k_usb_msc_hex_chars_u16] = {};
  uint8_t hex_pid[k_usb_msc_hex_chars_u16] = {};
  usb_msc_format_hex_u16(vid, hex_vid);
  usb_msc_format_hex_u16(pid, hex_pid);

  ra_err_t err =
    usb_msc_sci_write(k_usb_msc_msg_attach_pre, (uint32_t)(sizeof(k_usb_msc_msg_attach_pre) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(hex_vid, (uint32_t)k_usb_msc_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    usb_msc_sci_write(k_usb_msc_msg_attach_mid, (uint32_t)(sizeof(k_usb_msc_msg_attach_mid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(hex_pid, (uint32_t)k_usb_msc_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    usb_msc_sci_write(k_usb_msc_msg_attach_lun, (uint32_t)(sizeof(k_usb_msc_msg_attach_lun) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_print_dec_u32((uint32_t)max_lun);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

/* =============================================================================
 * Pin / SCI / USB bring-up
 * =============================================================================
 */

/**
 * @brief Route SCI8 J-Link OB CDC pins (PD_02 = TX, PD_03 = RX).
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
[[nodiscard]] static ra_err_t usb_msc_pins_sci_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_usb_msc_pin_sci_tx, k_ra_psel_sci_async, "usb_host_msc_browse.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_msc_pin_sci_rx,
                                 k_ra_psel_sci_async,
                                 "usb_host_msc_browse.rxd8");
}

/**
 * @brief Route the USBHS_VBUS sense pin (P4_08, PSEL = 0x14).
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
[[nodiscard]] static ra_err_t usb_msc_pins_hs_init(void)
{
  return ra_pfs_route_peripheral(k_usb_msc_pin_hs_vbus,
                                 (ra_psel_t)k_usb_msc_psel_usb_hs,
                                 "usb_host_msc_browse.hs_vbus");
}

/**
 * @brief Attach callback -- store the device snapshot, light LED1.
 *
 * @param[in] ctx    Caller context (unused; the callback writes
 *                   the file-scope `s_state`).
 * @param[in] device Pointer to the attach snapshot.
 *
 * @pre Driver was initialised via `ra_usb_hmsc_init`.
 *
 * @post `s_state.attached` is true, `s_state.device` carries the
 *       VID/PID/EP/max-lun info, and LED1 is driven high.
 *
 * @note Invoked from dispatch context; do not call SCI from here --
 *       the main loop performs the SCI prints.
 *
 * @since 0.1.0
 */
static void usb_msc_on_attach(void* ctx, const ra_usb_hmsc_device_t* device)
{
  (void)ctx;
  if (device == nullptr) {
    return;
  }
  s_state.device   = *device;
  s_state.attached = true;
  (void)ra_board_led_on(k_ra_board_led1);
}

/**
 * @brief Bring CGC + SCI + LED1/LED2 + USBHS pins + host-MSC up.
 *        Panic-halts on any failure.
 *
 * @details
 * Same staging order as uart_hello / usb_host_cdc_echo.
 *
 * @since 0.1.0
 */
static void usb_msc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_msc_panic_halt();
  }

  if (usb_msc_pins_sci_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_usb_msc_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_usb_msc_sci_channel, &sci_cfg) != k_ra_ok) {
    usb_msc_panic_halt();
  }

  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    usb_msc_panic_halt();
  }

  if (usb_msc_pins_hs_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }

  if (ra_usb_hmsc_init(k_ra_usb_speed_hs) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_usb_hmsc_attach_callback(usb_msc_on_attach, nullptr) != k_ra_ok) {
    usb_msc_panic_halt();
  }
}

/* =============================================================================
 * INQUIRY / READ_CAPACITY / READ(10) sequence
 * =============================================================================
 */

/**
 * @brief Print the decoded INQUIRY response strings.
 *
 * @param[in] resp Decoded INQUIRY response from the host-MSC layer.
 *
 * @return ra_err_t propagated from SCI helpers.
 *
 * @retval k_ra_ok All chunks queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre SCI8 init already ran.
 *
 * @post One ASCII line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_inquiry(const ra_usb_hmsc_inquiry_response_t* resp)
{
  ra_err_t err =
    usb_msc_sci_write(k_usb_msc_msg_inquiry, (uint32_t)(sizeof(k_usb_msc_msg_inquiry) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(resp->vendor_id, (uint32_t)sizeof(resp->vendor_id));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(k_usb_msc_msg_inquiry_pid,
                          (uint32_t)(sizeof(k_usb_msc_msg_inquiry_pid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(resp->product_id, (uint32_t)sizeof(resp->product_id));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(k_usb_msc_msg_inquiry_rev,
                          (uint32_t)(sizeof(k_usb_msc_msg_inquiry_rev) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(resp->product_revision, (uint32_t)sizeof(resp->product_revision));
  if (err != k_ra_ok) {
    return err;
  }
  return usb_msc_sci_write(k_usb_msc_msg_inquiry_tail,
                           (uint32_t)(sizeof(k_usb_msc_msg_inquiry_tail) - 1U));
}

/**
 * @brief Print "ra8d2 host: capacity blocks=N block_size=M\r\n".
 *
 * @param[in] block_count Total block count.
 * @param[in] block_size  Block size in bytes.
 *
 * @return ra_err_t propagated from SCI helpers.
 *
 * @retval k_ra_ok All chunks queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre SCI8 init already ran.
 *
 * @post One ASCII line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_capacity(uint32_t block_count, uint32_t block_size)
{
  ra_err_t err =
    usb_msc_sci_write(k_usb_msc_msg_capacity, (uint32_t)(sizeof(k_usb_msc_msg_capacity) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_print_dec_u32(block_count);
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_sci_write(k_usb_msc_msg_capacity_mid,
                          (uint32_t)(sizeof(k_usb_msc_msg_capacity_mid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_print_dec_u32(block_size);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

/**
 * @brief Hex-dump the first 64 bytes of `sector` over SCI8.
 *
 * @details Prints `k_usb_msc_dump_columns` bytes per row separated by
 * spaces, with CRLF between rows.
 *
 * @param[in] sector Pointer to a 512-byte sector buffer.
 *
 * @return ra_err_t propagated from SCI helpers.
 *
 * @retval k_ra_ok Dump complete.
 * @retval other   Underlying SCI failure.
 *
 * @pre `sector` is non-NULL and points to >=64 bytes of valid data.
 *
 * @post 64 hex bytes plus delimiters and CRLFs are in the SCI8 FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_dump_first_bytes(const uint8_t* sector)
{
  ra_err_t err = usb_msc_sci_write(k_usb_msc_msg_mbr, (uint32_t)(sizeof(k_usb_msc_msg_mbr) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  for (uint16_t i = 0U; i < k_usb_msc_dump_bytes; i++) {
    uint8_t hex[k_usb_msc_hex_chars_u8] = {};
    usb_msc_format_hex_u8(sector[i], hex);
    err = usb_msc_sci_write(hex, (uint32_t)k_usb_msc_hex_chars_u8);
    if (err != k_ra_ok) {
      return err;
    }
    const bool end_of_row = (((uint16_t)(i + 1U) % k_usb_msc_dump_columns) == 0U);
    if (end_of_row) {
      err = usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
    } else {
      err = usb_msc_sci_write(k_usb_msc_msg_space, (uint32_t)(sizeof(k_usb_msc_msg_space) - 1U));
    }
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Toggle LED2 once -- one visual blink per SCSI op.
 *
 * @return ra_err_t passthrough from `ra_gpio_toggle`.
 *
 * @retval k_ra_ok                    Toggle accepted.
 * @retval k_ra_err_gpio_invalid_pin  LED2 pin index rejected.
 *
 * @pre LED2 GPIO output is initialised.
 *
 * @post LED2 has flipped state once.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_blink_op(void)
{
  return ra_board_led_toggle(k_ra_board_led2);
}

/**
 * @brief Run INQUIRY -> READ_CAPACITY -> READ(10) on LBA 0 and dump.
 *
 * @param[out] sector_buf Buffer of `k_usb_msc_block_size_bytes` bytes
 *                        that receives the sector-0 read.
 *
 * @return ra_err_t propagated from the SCSI / SCI helpers.
 *
 * @retval k_ra_ok                Full sequence completed; sector dumped.
 * @retval k_ra_err_null_ptr      `sector_buf` was NULL.
 * @retval k_ra_err_invalid_state No device attached when the call ran.
 * @retval k_ra_err_hw_error      A SCSI op failed (BBB transfer error).
 * @retval other                  Underlying SCI failure.
 *
 * @pre Attach callback has fired (`s_state.attached == true`).
 * @pre `sector_buf` has 512 bytes of storage.
 *
 * @post `sector_buf` holds sector 0 and the 64-byte dump line is queued
 *       on SCI8.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_run_browse(uint8_t* sector_buf)
{
  if (sector_buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (!s_state.attached) {
    return k_ra_err_invalid_state;
  }

  ra_usb_hmsc_inquiry_response_t inquiry = {};
  ra_err_t                       err     = ra_usb_hmsc_inquiry(k_usb_msc_target_lun, &inquiry);
  if (err != k_ra_ok) {
    return err;
  }
  (void)usb_msc_blink_op();
  err = usb_msc_print_inquiry(&inquiry);
  if (err != k_ra_ok) {
    return err;
  }

  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;
  err                  = ra_usb_hmsc_read_capacity(k_usb_msc_target_lun, &block_count, &block_size);
  if (err != k_ra_ok) {
    return err;
  }
  (void)usb_msc_blink_op();
  err = usb_msc_print_capacity(block_count, block_size);
  if (err != k_ra_ok) {
    return err;
  }

  err = ra_usb_hmsc_read10(k_usb_msc_target_lun, 0U, 1U, sector_buf);
  if (err != k_ra_ok) {
    return err;
  }
  (void)usb_msc_blink_op();
  return usb_msc_dump_first_bytes(sector_buf);
}

/**
 * @brief Print "SCSI op failed err=0xNNNNNNNN\r\n" on SCSI failure.
 *
 * @param[in] err Error code returned from the SCSI helper.
 *
 * @return ra_err_t propagated from SCI helpers.
 *
 * @retval k_ra_ok The diagnostic line is queued.
 * @retval other   Underlying SCI failure (the main loop ignores this).
 *
 * @pre SCI8 init already ran.
 *
 * @post One diagnostic line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_scsi_err(ra_err_t err)
{
  uint8_t  hex[k_usb_msc_hex_chars_u32] = {};
  uint32_t v                            = (uint32_t)err;
  for (uint8_t i = 0U; i < k_usb_msc_hex_chars_u32; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_msc_hex_chars_u32 - 1U - i) * k_usb_msc_hex_nibble_count);
    hex[i] = usb_msc_nibble_to_hex((v >> shift) & k_usb_msc_hex_nibble_mask);
  }

  ra_err_t e =
    usb_msc_sci_write(k_usb_msc_msg_scsi_err, (uint32_t)(sizeof(k_usb_msc_msg_scsi_err) - 1U));
  if (e != k_ra_ok) {
    return e;
  }
  e = usb_msc_sci_write(hex, (uint32_t)k_usb_msc_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up host-MSC then runs a one-shot
 *        INQUIRY -> READ_CAPACITY -> READ(10) -> dump sequence.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On a clean run the CPU dumps the MBR once and parks in WFI.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  usb_msc_setup_or_halt();

  ra_isr_globals_enable();

  if (usb_msc_sci_write(k_usb_msc_msg_ready, (uint32_t)(sizeof(k_usb_msc_msg_ready) - 1U)) !=
      k_ra_ok) {
    usb_msc_panic_halt();
  }

  bool    announced                              = false;
  bool    browsed                                = false;
  uint8_t sector_buf[k_usb_msc_block_size_bytes] = {};

  while (1) {
    if (s_state.attached && !announced) {
      const uint16_t vid     = s_state.device.vendor_id;
      const uint16_t pid     = s_state.device.product_id;
      const uint8_t  max_lun = s_state.device.max_lun;
      if (usb_msc_print_attach(vid, pid, max_lun) != k_ra_ok) {
        break;
      }
      announced = true;
    }
    if (s_state.attached && !browsed) {
      const ra_err_t err = usb_msc_run_browse(sector_buf);
      if (err != k_ra_ok) {
        (void)usb_msc_print_scsi_err(err);
      }
      browsed = true;
    }
    ra_delay_ms(k_usb_msc_idle_ms);
  }

  usb_msc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
