/**
 * @file examples/ek_ra8d2/usb_host_msc_browse/main.c
 * @brief USB host-mode MSC browser smoke test for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Hardware-test app for the host-side MSC class layer (`ra_usb_hmsc.c`).
 * Turns the EK-RA8D2 into a USB host on the J7 USB-HS jack, enumerates an
 * inserted USB mass-storage device via ::ra_usb_hmsc_enumerate, then runs
 * SCSI INQUIRY -> READ_CAPACITY(10) -> READ(10) of LBA 0 and dumps the
 * first 64 bytes of the MBR plus the 55 AA boot-signature verdict over the
 * J-Link OB CDC virtual COM port at 115200 8N1.
 *
 * Board specifics (EK-RA8D2 v1 UM):
 *  - PD07 HIGH = U18 supplies VBUS to J7 (Sec 6.2 p 34, 2 A budget).
 *  - SW4-8 must sit in the Host position; set in software through the
 *    U15 expander (::ra_board_io_expander_set_usbhs_host_mode).
 *  - P4_08 is the only PFS-muxed USBHS pin (VBUS sense, PSEL 0x14);
 *    D+/D- are dedicated PHY package balls.
 *
 * The ladder retries every 5 s so a freshly inserted or reseated drive is
 * picked up automatically.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
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
  k_usb_msc_baud        = 115200U, /**< J-Link OB CDC log baud.            */
  k_usb_msc_sci_channel = 8U,      /**< SCI8 -> J-Link OB CDC bridge.      */
  k_usb_msc_idle_ms     = 50U,     /**< Idle tick in the parked main loop. */
  k_usb_msc_retry_ms    = 5000U,   /**< Pause between ladder retries.      */
  k_usb_msc_target_lun  = 0U,      /**< Browse LUN 0 (typical stick).      */
  k_usb_msc_mbr_lba     = 0U,      /**< Sector read for the dump.          */
  k_usb_msc_one_block   = 1U,      /**< Single-block READ(10).             */
} usb_host_msc_browse_config_t;

/**
 * @enum usb_host_msc_dump_t
 * @brief Sizing constants for the MBR dump.
 */
typedef enum : uint16_t {
  k_usb_msc_block_size_bytes = 512U, /**< One SCSI default block.    */
  k_usb_msc_dump_bytes       = 64U,  /**< Bytes printed from sector. */
  k_usb_msc_dump_columns     = 16U,  /**< Hex bytes per dump row.    */
  k_usb_msc_mbr_sig_off_lo   = 510U, /**< MBR signature low byte.    */
  k_usb_msc_mbr_sig_off_hi   = 511U, /**< MBR signature high byte.   */
} usb_host_msc_dump_t;

/**
 * @enum usb_host_msc_hex_t
 * @brief Hex/decimal text-buffer sizing constants.
 */
typedef enum : uint8_t {
  k_usb_msc_hex_chars_u8     = 2U,    /**< 8-bit value -> "AB".            */
  k_usb_msc_hex_chars_u16    = 4U,    /**< 16-bit value -> "ABCD".         */
  k_usb_msc_hex_chars_u32    = 8U,    /**< 32-bit value -> "ABCDEF01".     */
  k_usb_msc_dec_chars_u32    = 10U,   /**< Max digits for a 32-bit count.  */
  k_usb_msc_hex_nibble_count = 4U,    /**< Bits per hex nibble.            */
  k_usb_msc_hex_digit_split  = 10U,   /**< Threshold between '0-9'/'A-F'.  */
  k_usb_msc_mbr_sig_lo       = 0x55U, /**< Expected signature low byte.    */
  k_usb_msc_mbr_sig_hi       = 0xAAU, /**< Expected signature high byte.   */
} usb_host_msc_hex_t;

/**
 * @enum usb_host_msc_hex_mask_t
 * @brief Bit-mask constants used by the hex/decimal formatters.
 */
typedef enum : uint32_t {
  k_usb_msc_hex_nibble_mask = 0xFU, /**< 4-bit nibble mask.            */
  k_usb_msc_dec_radix       = 10U,  /**< Base for decimal conversion.  */
} usb_host_msc_hex_mask_t;

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

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra_port_pin_t k_usb_msc_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief Controller this app drives (the drive sits in the HS jack). */
static const ra_usb_speed_t k_usb_msc_speed = k_ra_usb_speed_hs;

/* =============================================================================
 * Status messages over SCI8
 * =============================================================================
 */

/** @brief First message after SCI is up. */
static const uint8_t k_usb_msc_msg_ready[] =
  "ra8d2 host: ready (USB-HS), plug a USB drive into the HS jack\r\n";
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
/** @brief Banner when a ladder step failed. */
static const uint8_t k_usb_msc_msg_fail[] = "ra8d2 host: step failed err=0x";
/** @brief A single space (used to format the hex dump). */
static const uint8_t k_usb_msc_msg_space[] = " ";
/** @brief Prefix for the MBR boot-signature verdict line. */
static const uint8_t k_usb_msc_msg_sig_pre[] = "ra8d2 host: mbr sig @510 = ";
/** @brief Boot-signature verdict: matches 55 AA. */
static const uint8_t k_usb_msc_msg_sig_ok[] = " (ok)\r\n";
/** @brief Boot-signature verdict: does not match 55 AA. */
static const uint8_t k_usb_msc_msg_sig_bad[] = " (BAD)\r\n";
/** @brief Printed between ladder retry cycles. */
static const uint8_t k_usb_msc_msg_retry[] =
  "ra8d2 host: retrying in 5 s (reseat the drive any time)\r\n";

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No further code runs.
 * @note Never returns.
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
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 * @pre Caller has already masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is in the printable hex range.
 * @post No state changes.
 * @note Pure function.
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
 * @pre @p out has 2 bytes of storage.
 * @pre None beyond the buffer contract.
 * @post @p out holds the big-endian hex digits of @p value.
 * @post No other state changes.
 * @note Pure function.
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
 * @pre @p out has 4 bytes of storage.
 * @pre None beyond the buffer contract.
 * @post @p out holds the big-endian hex digits of @p value.
 * @post No other state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static void usb_msc_format_hex_u16(uint16_t value, uint8_t* out)
{
  for (uint8_t i = 0U; i < k_usb_msc_hex_chars_u16; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_msc_hex_chars_u16 - 1U - i) * k_usb_msc_hex_nibble_count);
    out[i] = usb_msc_nibble_to_hex(((uint32_t)value >> shift) & k_usb_msc_hex_nibble_mask);
  }
}

/**
 * @brief Format a uint32_t into ASCII decimal.
 *
 * @param[in]  value 32-bit value.
 * @param[out] out   Destination buffer (>=10 bytes).
 * @return Number of decimal digits written (1..10).
 * @retval 1 For values 0..9.
 * @pre @p out has 10 bytes of storage.
 * @pre None beyond the buffer contract.
 * @post @p out holds the most-significant-first digits.
 * @post No other state changes.
 * @note Pure function.
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
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
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
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_dec_u32(uint32_t value)
{
  uint8_t       dec[k_usb_msc_dec_chars_u32] = {};
  const uint8_t dec_len                      = usb_msc_format_decimal_u32(value, dec);
  return usb_msc_sci_write(dec, (uint32_t)dec_len);
}

/**
 * @brief Print "device attached vid=0x.. pid=0x.. max-lun=N".
 *
 * @param[in] device Enumerated device snapshot.
 * @return ra_err_t propagated from the underlying SCI writes.
 * @retval k_ra_ok All chunks queued.
 * @pre `ra_sci_init` ran; @p device is non-NULL.
 * @pre The snapshot was filled by ::ra_usb_hmsc_enumerate.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_attach(const ra_usb_hmsc_device_t* device)
{
  uint8_t  hex16[k_usb_msc_hex_chars_u16] = {};
  ra_err_t err =
    usb_msc_sci_write(k_usb_msc_msg_attach_pre, (uint32_t)(sizeof(k_usb_msc_msg_attach_pre) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  usb_msc_format_hex_u16(device->vendor_id, hex16);
  err = usb_msc_sci_write(hex16, (uint32_t)k_usb_msc_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    usb_msc_sci_write(k_usb_msc_msg_attach_mid, (uint32_t)(sizeof(k_usb_msc_msg_attach_mid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  usb_msc_format_hex_u16(device->product_id, hex16);
  err = usb_msc_sci_write(hex16, (uint32_t)k_usb_msc_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    usb_msc_sci_write(k_usb_msc_msg_attach_lun, (uint32_t)(sizeof(k_usb_msc_msg_attach_lun) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_print_dec_u32((uint32_t)device->max_lun);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

/**
 * @brief Print the decoded INQUIRY response strings.
 *
 * @param[in] resp Decoded INQUIRY response from the host-MSC layer.
 * @return ra_err_t propagated from SCI helpers.
 * @retval k_ra_ok All chunks queued.
 * @pre SCI8 init already ran; @p resp is non-NULL.
 * @pre @p resp came from ::ra_usb_hmsc_inquiry.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note The three string fields are space-padded ASCII, not NUL-terminated.
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
 * @brief Print "capacity blocks=N block_size=M".
 *
 * @param[in] block_count Total block count.
 * @param[in] block_size  Block size in bytes.
 * @return ra_err_t propagated from SCI helpers.
 * @retval k_ra_ok All chunks queued.
 * @pre SCI8 init already ran.
 * @pre Values came from ::ra_usb_hmsc_read_capacity.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
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
 * @brief Hex-dump the first 64 bytes of @p sector, then the 55 AA verdict.
 *
 * @param[in] sector Pointer to a 512-byte sector buffer.
 * @return ra_err_t propagated from SCI helpers.
 * @retval k_ra_ok Dump complete.
 * @pre @p sector is non-NULL and holds 512 valid bytes.
 * @pre SCI8 init already ran.
 * @post The dump plus the signature verdict line are queued.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_dump_mbr(const uint8_t* sector)
{
  ra_err_t err = usb_msc_sci_write(k_usb_msc_msg_mbr, (uint32_t)(sizeof(k_usb_msc_msg_mbr) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t hex[k_usb_msc_hex_chars_u8] = {};
  for (uint16_t i = 0U; i < k_usb_msc_dump_bytes; i++) {
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
  (void)usb_msc_sci_write(k_usb_msc_msg_sig_pre, (uint32_t)(sizeof(k_usb_msc_msg_sig_pre) - 1U));
  usb_msc_format_hex_u8(sector[k_usb_msc_mbr_sig_off_lo], hex);
  (void)usb_msc_sci_write(hex, (uint32_t)k_usb_msc_hex_chars_u8);
  (void)usb_msc_sci_write(k_usb_msc_msg_space, (uint32_t)(sizeof(k_usb_msc_msg_space) - 1U));
  usb_msc_format_hex_u8(sector[k_usb_msc_mbr_sig_off_hi], hex);
  (void)usb_msc_sci_write(hex, (uint32_t)k_usb_msc_hex_chars_u8);
  bool sig_ok = false;
  if (sector[k_usb_msc_mbr_sig_off_lo] == (uint8_t)k_usb_msc_mbr_sig_lo) {
    if (sector[k_usb_msc_mbr_sig_off_hi] == (uint8_t)k_usb_msc_mbr_sig_hi) {
      sig_ok = true;
    }
  }
  if (sig_ok) {
    return usb_msc_sci_write(k_usb_msc_msg_sig_ok, (uint32_t)(sizeof(k_usb_msc_msg_sig_ok) - 1U));
  }
  return usb_msc_sci_write(k_usb_msc_msg_sig_bad, (uint32_t)(sizeof(k_usb_msc_msg_sig_bad) - 1U));
}

/**
 * @brief Print "step failed err=0xNNNNNNNN".
 *
 * @param[in] err Error code returned from a ladder step.
 * @return ra_err_t propagated from SCI helpers.
 * @retval k_ra_ok The diagnostic line is queued.
 * @pre SCI8 init already ran.
 * @pre @p err is not k_ra_ok.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_print_fail(ra_err_t err)
{
  uint8_t  hex[k_usb_msc_hex_chars_u32] = {};
  uint32_t v                            = (uint32_t)err;
  for (uint8_t i = 0U; i < k_usb_msc_hex_chars_u32; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_msc_hex_chars_u32 - 1U - i) * k_usb_msc_hex_nibble_count);
    hex[i] = usb_msc_nibble_to_hex((v >> shift) & k_usb_msc_hex_nibble_mask);
  }
  ra_err_t e = usb_msc_sci_write(k_usb_msc_msg_fail, (uint32_t)(sizeof(k_usb_msc_msg_fail) - 1U));
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
 * Bring-up
 * =============================================================================
 */

/**
 * @brief Bring CGC + USB60CLK + SysTick + SCI8 + LEDs + USB host up.
 *        Panic-halts on any failure.
 *
 * @details Board steps for the J7 host role: PD07 HIGH (U18 supplies the
 * jack), SW4-8 to Host via the U15 expander, P4_08 routed to USBHS_VBUS,
 * then `ra_usb_hmsc_init` (which runs the full HS PHY bring-up).
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; the USBHS controller is in host mode.
 * @post LED1/LED2 are initialized.
 * @note Called exactly once from main.
 * @since 0.1.0
 */
static void usb_msc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
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
  if (ra_pfs_route_peripheral(k_usb_msc_pin_sci_tx, k_ra_psel_sci_async, "usb_msc.txd8") !=
      k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_usb_msc_pin_sci_rx, k_ra_psel_sci_async, "usb_msc.rxd8") !=
      k_ra_ok) {
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
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_gpio_output_init(k_usb_msc_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_usb_msc_pin_hs_vbus, k_ra_psel_usb_hs, "usb_msc.hs_vbus") !=
      k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_usb_hmsc_init(k_usb_msc_speed) != k_ra_ok) {
    usb_msc_panic_halt();
  }
}

/**
 * @brief Enumerate + INQUIRY + capacity + MBR dump, all via ra_usb_hmsc.
 *
 * @details One full pass of the browse ladder over the class-layer API.
 * Each SCSI op toggles LED2; LED1 lights on full success.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The MBR dump and signature verdict are printed.
 * @pre ::usb_msc_setup_or_halt completed.
 * @pre The USB drive is inserted in the HS jack.
 * @post On success LED1 is on and all result lines are printed.
 * @post On failure the error line is printed.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_run_ladder(void)
{
  static uint8_t s_sector[k_usb_msc_block_size_bytes] = {};

  ra_usb_hmsc_device_t device = {};
  ra_err_t             err    = ra_usb_hmsc_enumerate(&device);
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_print_attach(&device);
  if (err != k_ra_ok) {
    return err;
  }
  ra_usb_hmsc_inquiry_response_t inquiry = {};
  err = ra_usb_hmsc_inquiry((uint8_t)k_usb_msc_target_lun, &inquiry);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);
  err = usb_msc_print_inquiry(&inquiry);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;
  err = ra_usb_hmsc_read_capacity((uint8_t)k_usb_msc_target_lun, &block_count, &block_size);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);
  err = usb_msc_print_capacity(block_count, block_size);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_hmsc_read10((uint8_t)k_usb_msc_target_lun,
                           (uint32_t)k_usb_msc_mbr_lba,
                           (uint16_t)k_usb_msc_one_block,
                           s_sector);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);
  err = usb_msc_dump_mbr(s_sector);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led1);
  return k_ra_ok;
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up the HS host and climbs the MSC
 *        ladder until it succeeds.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the MBR dump + signature verdict are printed and
 *       LED1 is lit; the CPU then parks in WFI.
 * @post On any HAL init failure the function halts in WFI.
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

  /* Climb the ladder until it succeeds: a freshly inserted (or reseated)
   * drive is picked up by the next retry cycle automatically. */
  ra_err_t err = usb_msc_run_ladder();
  while (err != k_ra_ok) {
    (void)usb_msc_print_fail(err);
    (void)usb_msc_sci_write(k_usb_msc_msg_retry, (uint32_t)(sizeof(k_usb_msc_msg_retry) - 1U));
    ra_delay_ms(k_usb_msc_retry_ms);
    err = usb_msc_run_ladder();
  }

  while (1) {
    ra_delay_ms(k_usb_msc_idle_ms);
    __asm__ volatile("wfi");
  }

  return 0;
}
#pragma GCC diagnostic pop
