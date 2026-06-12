/**
 * @file examples/ek_ra8d2/usb_host_msc_browse/main.c
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

#include "ra8d2_usb_regs.h"
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
  k_usb_msc_baud              = 115200U, /**< J-Link OB CDC log baud.        */
  k_usb_msc_sci_channel       = 8U,      /**< SCI8 -> J-Link OB CDC bridge.  */
  k_usb_msc_idle_ms           = 50U,     /**< Idle while waiting for attach. */
  k_usb_msc_vbus_ms           = 200U,    /**< VBUS / device power-on settle. */
  k_usb_msc_reset_hold_ms     = 50U,     /**< USB bus-reset hold (>=10 ms).  */
  k_usb_msc_recovery_ms       = 20U,     /**< Post-reset recovery (TRSTRCY).  */
  k_usb_msc_enum_to_ms        = 3000U,   /**< Enumeration pump timeout.      */
  k_usb_msc_ctrt_mask         = (1U << (uint32_t)k_ra_int0_bit_ctrt), /**< INTSTS0 CTRT bit. */
  k_usb_msc_bmreq_dev_in      = 0x80U,   /**< bmRequestType: dev->host, std. */
  k_usb_msc_breq_get_desc     = 0x06U,   /**< bRequest: GET_DESCRIPTOR.      */
  k_usb_msc_wval_dev_desc     = 0x0100U, /**< wValue: DEVICE descriptor, idx0.*/
  k_usb_msc_dev_desc_len      = 18U,     /**< Device descriptor length.       */
  k_usb_msc_desc_vid_off      = 8U,      /**< idVendor LSB offset.            */
  k_usb_msc_desc_pid_off      = 10U,     /**< idProduct LSB offset.           */
  k_usb_msc_bmreq_dev_out     = 0x00U,   /**< bmRequestType: host->dev, std.  */
  k_usb_msc_breq_set_addr     = 0x05U,   /**< bRequest: SET_ADDRESS.          */
  k_usb_msc_test_address      = 1U,      /**< Address assigned to the device. */
  k_usb_msc_addr_settle_ms    = 5U,      /**< Post-SET_ADDRESS recovery wait. */
  k_usb_msc_breq_set_config   = 0x09U,   /**< bRequest: SET_CONFIGURATION.    */
  k_usb_msc_breq_get_max_lun  = 0xFEU,   /**< MSC class: GET_MAX_LUN.        */
  k_usb_msc_bmreq_class_if_in = 0xA1U,   /**< bmRequestType: class, iface, in.*/
  k_usb_msc_wval_cfg_desc     = 0x0200U, /**< wValue: CONFIGURATION desc, idx0.*/
  k_usb_msc_cfg_hdr_len       = 9U,      /**< Configuration descriptor header. */
  k_usb_msc_cfg_buf_len       = 128U,    /**< Full-configuration read buffer.  */
  k_usb_msc_cfg_off_total     = 2U,      /**< wTotalLength LSB offset.         */
  k_usb_msc_cfg_off_value     = 5U,      /**< bConfigurationValue offset.      */
  k_usb_msc_desc_off_len      = 0U,      /**< Any descriptor: bLength.         */
  k_usb_msc_desc_off_type     = 1U,      /**< Any descriptor: bDescriptorType. */
  k_usb_msc_dtype_iface       = 4U,      /**< bDescriptorType: INTERFACE.      */
  k_usb_msc_dtype_ep          = 5U,      /**< bDescriptorType: ENDPOINT.       */
  k_usb_msc_if_off_number     = 2U,      /**< Interface: bInterfaceNumber.     */
  k_usb_msc_if_off_class      = 5U,      /**< Interface: bInterfaceClass.      */
  k_usb_msc_if_off_sub        = 6U,      /**< Interface: bInterfaceSubClass.   */
  k_usb_msc_if_off_proto      = 7U,      /**< Interface: bInterfaceProtocol.   */
  k_usb_msc_class_msc         = 0x08U,   /**< Mass Storage class code.         */
  k_usb_msc_subclass_scsi     = 0x06U,   /**< SCSI transparent command set.    */
  k_usb_msc_protocol_bot      = 0x50U,   /**< Bulk-Only Transport protocol.    */
  k_usb_msc_ep_off_addr       = 2U,      /**< Endpoint: bEndpointAddress.      */
  k_usb_msc_ep_off_attr       = 3U,      /**< Endpoint: bmAttributes.          */
  k_usb_msc_ep_off_mps        = 4U,      /**< Endpoint: wMaxPacketSize LSB.    */
  k_usb_msc_ep_dir_in_bit     = 0x80U,   /**< bEndpointAddress direction bit.  */
  k_usb_msc_ep_num_mask       = 0x0FU,   /**< bEndpointAddress number field.   */
  k_usb_msc_ep_attr_mask      = 0x03U,   /**< bmAttributes transfer-type mask. */
  k_usb_msc_ep_attr_bulk      = 0x02U,   /**< bmAttributes: bulk transfer.     */
  k_usb_msc_pipe_in           = 1U,      /**< Controller pipe for bulk IN.     */
  k_usb_msc_pipe_out          = 2U,      /**< Controller pipe for bulk OUT.    */
  k_usb_msc_cbw_len           = 31U,     /**< MSC BOT CBW length.              */
  k_usb_msc_csw_len           = 13U,     /**< MSC BOT CSW length.              */
  k_usb_msc_cbw_tag_off       = 4U,      /**< dCBWTag offset inside the CBW.   */
  k_usb_msc_cdb6_len          = 6U,      /**< 6-byte CDB length.               */
  k_usb_msc_cdb10_len         = 10U,     /**< 10-byte CDB length.              */
  k_usb_msc_cdb_alloc_off     = 4U,      /**< INQUIRY allocation-length byte.  */
  k_usb_msc_read10_cnt_off    = 8U,      /**< READ(10) transfer-count LSB.     */
  k_usb_msc_one_block         = 1U,      /**< Single-block READ(10) count.     */
  k_usb_msc_inq_vendor_off    = 8U,      /**< INQUIRY data: vendor id offset.  */
  k_usb_msc_inq_product_off   = 16U,     /**< INQUIRY data: product id offset. */
  k_usb_msc_inq_rev_off       = 32U,     /**< INQUIRY data: revision offset.   */
  k_usb_msc_bits_per_byte     = 8U,      /**< Bit width of one byte.           */
  k_usb_msc_be32_top_byte     = 3U,      /**< Index of the MSB in a BE u32.    */
  k_usb_msc_mbr_sig_off_lo    = 510U,    /**< MBR signature low-byte offset.   */
  k_usb_msc_mbr_sig_off_hi    = 511U,    /**< MBR signature high-byte offset.  */
  k_usb_msc_mbr_sig_lo        = 0x55U,   /**< Expected MBR signature low byte. */
  k_usb_msc_mbr_sig_hi        = 0xAAU,   /**< Expected MBR signature high byte.*/
  k_usb_msc_devadd1_off       = 0x00D2U, /**< DEVADD1 raw offset (diag read).  */
  k_usb_msc_attach_to_ms      = 2000U,   /**< Wait for D+ attach after VBUS.   */
  k_usb_msc_retry_ms          = 5000U,   /**< Pause between ladder retries.    */
  k_usb_msc_debounce_ms       = 500U,    /**< Post-attach settle before reset. */
  k_usb_msc_enum_tries        = 8U,      /**< (reset?, addr) hunt attempts.    */
  k_usb_msc_enum_no_rst_tries = 4U,      /**< Attempts before using bus reset. */
  k_usb_msc_addr_alt_mask     = 0x03U,   /**< Alternate addr 0..3 per attempt. */
} usb_host_msc_browse_config_t;

/**
 * @enum usb_host_msc_step_t
 * @brief Ladder step identifiers printed on a bring-up failure.
 */
typedef enum : uint8_t {
  k_usb_msc_step_dev_desc = 1U, /**< GET_DESCRIPTOR(device) at addr 0.  */
  k_usb_msc_step_set_addr = 2U, /**< SET_ADDRESS + re-read at addr 1.   */
  k_usb_msc_step_config   = 3U, /**< Read + parse config descriptor.    */
  k_usb_msc_step_set_cfg  = 4U, /**< SET_CONFIGURATION.                 */
  k_usb_msc_step_pipes    = 5U, /**< Bulk pipe setup.                   */
  k_usb_msc_step_inquiry  = 6U, /**< SCSI INQUIRY over BOT.             */
  k_usb_msc_step_capacity = 7U, /**< SCSI READ_CAPACITY(10) over BOT.   */
  k_usb_msc_step_read_mbr = 8U, /**< SCSI READ(10) of LBA 0 over BOT.   */
} usb_host_msc_step_t;

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
 * @brief PFS PSEL code for routing the USBFS host pins.
 *
 * @details HUM Ch 20.6 "Peripheral Select Settings" gives 0x13 for the
 * Full-Speed controller (USBFS). The drive is on the FS receptacle
 * because FS (12 Mbps) is simpler to bring up than HS (480 Mbps) and
 * sidesteps the HS SET_ADDRESS-stall blocker.
 */
typedef enum : uint8_t {
  k_usb_msc_psel_usb_fs =
    (uint8_t)k_ra_psel_usb_fs, /* HUM Ch 20.6 "Peripheral Select Settings" p 855 */
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
  k_usb_msc_lnst_mask       = 0x3U, /**< SYSSTS0.LNST[1:0] line-state. */
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

/** @brief USB_FS_VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra_port_pin_t k_usb_msc_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USB_FS_VBUSEN host VBUS-supply enable (P5_00, PSEL = 0x13).
 *  Routed as the USB peripheral function so the controller asserts it in
 *  host mode -- this is what actually powers the attached thumb drive. */
static const ra_port_pin_t k_usb_msc_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USB_FS_DP data line (P8_14, PSEL = 0x13). */
static const ra_port_pin_t k_usb_msc_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USB_FS_DM data line (P8_15, PSEL = 0x13). */
static const ra_port_pin_t k_usb_msc_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/* =============================================================================
 * Status messages over SCI8
 * =============================================================================
 */

/** @brief First message after SCI is up. */
static const uint8_t k_usb_msc_msg_ready[] =
  "ra8d2 host: ready (USB-FS), plug a USB drive into the FS port\r\n";

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

/** @brief Enumeration diagnostic prefix. */
static const uint8_t k_usb_msc_msg_enum[] = "ra8d2 host: enum lnst=";
/** @brief Enumeration diagnostic: host control-transfer stage field. */
static const uint8_t k_usb_msc_msg_enum_stage[] = " stage=";
/** @brief Enumeration diagnostic: DVSTCTR0 field. */
static const uint8_t k_usb_msc_msg_enum_dvst[] = " dvstctr0=0x";
/** @brief Enumeration diagnostic: INTSTS0 field. */
static const uint8_t k_usb_msc_msg_enum_ints[] = " intsts0=0x";
/** @brief Enumeration diagnostic: DCPCTR field. */
static const uint8_t k_usb_msc_msg_enum_dcp[] = " dcpctr=0x";
/** @brief Enumeration diagnostic: NRDYSTS field. */
static const uint8_t k_usb_msc_msg_enum_nrdy[] = " nrdy=0x";
/** @brief Enumeration diagnostic: DCPMAXP field (DEVSEL | MXPS). */
static const uint8_t k_usb_msc_msg_enum_maxp[] = " dcpmaxp=0x";
/** @brief Enumeration diagnostic: INTSTS1 field (SACK/SIGN/ATTCH). */
static const uint8_t k_usb_msc_msg_enum_i1[] = " ints1=0x";
/** @brief Enumeration diagnostic: DEVADD1 slot. */
static const uint8_t k_usb_msc_msg_enum_da1[] = " devadd1=0x";
/** @brief Enumeration diagnostic: PIPE1CTR field. */
static const uint8_t k_usb_msc_msg_enum_p1[] = " p1ctr=0x";
/** @brief Enumeration diagnostic: PIPE2CTR field. */
static const uint8_t k_usb_msc_msg_enum_p2[] = " p2ctr=0x";
/** @brief Prefix for the parsed MSC endpoint report. */
static const uint8_t k_usb_msc_msg_eps_pre[] = "ra8d2 host: msc iface eps in=0x";
/** @brief Separator between the IN and OUT endpoint numbers. */
static const uint8_t k_usb_msc_msg_eps_mid[] = " out=0x";
/** @brief Printed once SET_CONFIGURATION + pipe setup are done. */
static const uint8_t k_usb_msc_msg_cfg_ok[] = "ra8d2 host: configured, pipes ready\r\n";
/** @brief Prefix for the MBR boot-signature verdict line. */
static const uint8_t k_usb_msc_msg_sig_pre[] = "ra8d2 host: mbr sig @510 = ";
/** @brief Boot-signature verdict: matches 55 AA. */
static const uint8_t k_usb_msc_msg_sig_ok[] = " (ok)\r\n";
/** @brief Boot-signature verdict: does not match 55 AA. */
static const uint8_t k_usb_msc_msg_sig_bad[] = " (BAD)\r\n";
/** @brief Prefix for the ladder failure line (step id follows). */
static const uint8_t k_usb_msc_msg_step_fail[] = "ra8d2 host: FAIL step=";
/** @brief Printed when the device accepts and answers at address 1. */
static const uint8_t k_usb_msc_msg_addr1[] = "ra8d2 host: address 1 assigned\r\n";
/** @brief Printed between ladder retry cycles. */
static const uint8_t k_usb_msc_msg_retry[] =
  "ra8d2 host: retrying in 5 s (reseat the drive any time)\r\n";
/** @brief Prefix for the enumeration-hunt winner report. */
static const uint8_t k_usb_msc_msg_won[] = "ra8d2 host: enum attempt won=";

/* =============================================================================
 * Attach state shared with the callback
 * =============================================================================
 */

/**
 * @struct usb_msc_target_t
 * @brief MSC device facts gathered while walking the config descriptor.
 */
typedef struct {
  uint8_t  in_ep;     /**< Device bulk-IN endpoint number (no dir bit). */
  uint8_t  out_ep;    /**< Device bulk-OUT endpoint number.             */
  uint8_t  iface;     /**< MSC bInterfaceNumber (for GET_MAX_LUN).      */
  uint8_t  cfg_value; /**< bConfigurationValue to activate.             */
  uint16_t in_mps;    /**< Bulk-IN wMaxPacketSize.                      */
  uint16_t out_mps;   /**< Bulk-OUT wMaxPacketSize.                     */
} usb_msc_target_t;

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
 * @brief Route the four USBFS host pins (VBUS, VBUSEN, D+, D-) to PSEL 0x13.
 *
 * @details Unlike the HS receptacle (whose D+/D- are dedicated package
 * balls), the FS controller's data lines P8_14/P8_15 are PFS-muxed and
 * must be routed. VBUSEN (P5_00) is routed to the USB peripheral function
 * so the controller drives it HIGH in host mode -- that is what supplies
 * VBUS to the attached drive.
 *
 * @return Error code from the first failing `ra_pfs_route_peripheral`.
 *
 * @retval k_ra_ok                     All four FS pins routed.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success P4_07/P5_00/P8_14/P8_15 PFS PSEL = 0x13, PMR = 1.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_pins_fs_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_usb_msc_pin_fs_vbus,
                                         (ra_psel_t)k_usb_msc_psel_usb_fs,
                                         "usb_host_msc_browse.fs_vbus");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_msc_pin_fs_vbusen,
                                (ra_psel_t)k_usb_msc_psel_usb_fs,
                                "usb_host_msc_browse.fs_vbusen");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_msc_pin_fs_dp,
                                (ra_psel_t)k_usb_msc_psel_usb_fs,
                                "usb_host_msc_browse.fs_dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_msc_pin_fs_dm,
                                 (ra_psel_t)k_usb_msc_psel_usb_fs,
                                 "usb_host_msc_browse.fs_dm");
}

/**
 * @brief Print one labelled "<label>0x<hex16>" register field over SCI8.
 *
 * @param[in] label NUL-stripped literal (sent without its terminator).
 * @param[in] label_len Bytes of @p label to send.
 * @param[in] value 16-bit register value to format as 4 hex digits.
 *
 * @pre SCI8 init already ran.
 * @post One "<label>ABCD" token is queued on SCI8.
 * @since 0.1.0
 */
static void usb_msc_print_reg(const uint8_t* label, uint32_t label_len, uint16_t value)
{
  uint8_t hex[k_usb_msc_hex_chars_u16] = {};
  usb_msc_format_hex_u16(value, hex);
  (void)usb_msc_sci_write(label, label_len);
  (void)usb_msc_sci_write(hex, (uint32_t)k_usb_msc_hex_chars_u16);
}

/**
 * @brief Dump the host enumeration diagnostic line over SCI8.
 *
 * @details Reports SYSSTS0.LNST (1 = FS device pull-up visible), the last
 * control-transfer stage, and the live DVSTCTR0/INTSTS0/DCPCTR/NRDYSTS plus
 * both bulk pipe controls so a stalled transfer can be localised.
 *
 * @pre SCI8 init already ran; the FS controller is clocked.
 * @post One diagnostic line is queued on SCI8.
 * @since 0.1.0
 */
static void usb_msc_print_enum_diag(void)
{
  volatile const r_usb_regs_t* fs = (volatile const r_usb_regs_t*)(uintptr_t)k_ra_usb_fs0_base_addr;
  const uint16_t               lnst = (uint16_t)(fs->SYSSTS0 & (uint16_t)k_usb_msc_lnst_mask);
  (void)usb_msc_sci_write(k_usb_msc_msg_enum, (uint32_t)(sizeof(k_usb_msc_msg_enum) - 1U));
  (void)usb_msc_print_dec_u32((uint32_t)lnst);
  (void)usb_msc_sci_write(k_usb_msc_msg_enum_stage,
                          (uint32_t)(sizeof(k_usb_msc_msg_enum_stage) - 1U));
  (void)usb_msc_print_dec_u32((uint32_t)ra_usb_host_ctrl_stage());
  usb_msc_print_reg(k_usb_msc_msg_enum_dvst,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_dvst) - 1U),
                    fs->DVSTCTR0);
  usb_msc_print_reg(k_usb_msc_msg_enum_ints,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_ints) - 1U),
                    fs->INTSTS0);
  usb_msc_print_reg(k_usb_msc_msg_enum_dcp,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_dcp) - 1U),
                    fs->DCPCTR);
  usb_msc_print_reg(k_usb_msc_msg_enum_nrdy,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_nrdy) - 1U),
                    fs->NRDYSTS);
  usb_msc_print_reg(k_usb_msc_msg_enum_maxp,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_maxp) - 1U),
                    fs->DCPMAXP);
  usb_msc_print_reg(k_usb_msc_msg_enum_i1,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_i1) - 1U),
                    fs->INTSTS1);
  /* DEVADD1 sits past the modelled register window: raw offset 0xD2. */
  usb_msc_print_reg(k_usb_msc_msg_enum_da1,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_da1) - 1U),
                    *(volatile const uint16_t*)((uintptr_t)fs + (uintptr_t)k_usb_msc_devadd1_off));
  usb_msc_print_reg(k_usb_msc_msg_enum_p1,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_p1) - 1U),
                    fs->PIPECTR[k_usb_msc_pipe_in - 1U]);
  usb_msc_print_reg(k_usb_msc_msg_enum_p2,
                    (uint32_t)(sizeof(k_usb_msc_msg_enum_p2) - 1U),
                    fs->PIPECTR[k_usb_msc_pipe_out - 1U]);
  (void)usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

[[nodiscard]] static ra_err_t usb_msc_print_scsi_err(ra_err_t err);

/**
 * @brief Issue a standard GET_DESCRIPTOR(device) and return its 18 bytes.
 *
 * @details Thin wrapper over ::ra_usb_host_control_xfer used both for the
 * address-0 identity read and the post-SET_ADDRESS verification read.
 *
 * @param[out] desc Receives up to 18 descriptor bytes.
 * @param[out] out_rx Receives the byte count the device returned.
 * @return Passthrough from ::ra_usb_host_control_xfer.
 * @retval k_ra_ok The descriptor read completed.
 * @pre The bus is reset and UACT is on; the DCP targets the device.
 * @pre @p desc holds at least 18 bytes.
 * @post @p desc carries the device descriptor on success.
 * @post @p out_rx reflects the DATA-IN byte count.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_read_dev_desc(uint8_t* desc, uint16_t* out_rx)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_usb_msc_bmreq_dev_in,
    .b_request       = (uint8_t)k_usb_msc_breq_get_desc,
    .w_value         = (uint16_t)k_usb_msc_wval_dev_desc,
    .w_index         = 0U,
    .w_length        = (uint16_t)k_usb_msc_dev_desc_len,
  };
  return ra_usb_host_control_xfer(k_ra_usb_speed_fs,
                                  &setup,
                                  desc,
                                  (uint16_t)k_usb_msc_dev_desc_len,
                                  out_rx);
}

/**
 * @brief Reset the FS bus and read the device descriptor at address 0.
 *
 * @details Drives the bus reset by hand (assert USBRST, hold, release,
 * enable UACT, honour the TRSTRCY recovery), then reads the device
 * descriptor via the polled control engine and prints the VID/PID line
 * (the DATA-IN byte count rides in the max-lun field for confirmation).
 *
 * @param[out] out_addr Receives the address the device answered at.
 * @return Result of the descriptor read.
 * @retval k_ra_ok The drive answered with its descriptor.
 * @pre `ra_usb_host_init` ran; VBUS is supplied; the drive is inserted.
 * @pre The SCI8 console is up for the status prints.
 * @post On success the device descriptor VID/PID are printed.
 * @post The bus is left active (UACT on) targeting `*out_addr`.
 * @note Blocking; uses fixed settle delays around the reset.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_enumerate(uint8_t* out_addr)
{
  ra_delay_ms(k_usb_msc_vbus_ms);
  /* The VBUS power cycle means the device attaches AFTER boot: wait for
   * the D+ pull-up (SYSSTS0.LNST leaves SE0) before driving the bus
   * reset, else the reset fires on an empty bus and the late-attaching
   * device never enters its Default state (it then ignores all tokens:
   * observed as vid=0x0000 with lnst=1). Then apply the >=100 ms
   * attach debounce the USB spec requires before reset. */
  volatile const r_usb_regs_t* fs = (volatile const r_usb_regs_t*)(uintptr_t)k_ra_usb_fs0_base_addr;
  const uint32_t               t0 = ra_time_ms();
  while ((fs->SYSSTS0 & (uint16_t)k_usb_msc_lnst_mask) == 0U) {
    if ((ra_time_ms() - t0) > (uint32_t)k_usb_msc_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms(k_usb_msc_debounce_ms);

  /* Hunt for the device: it may still hold an address assigned by an
   * earlier run (a hub-side "power cycle" does not always drop VBUS), and
   * the bus reset may or may not knock it back to the default address.
   * Try each (reset?, address) combination and report the winner -- the
   * combination that answers tells us the true device state. */
  uint8_t  desc[k_usb_msc_dev_desc_len] = {};
  uint16_t rx                           = 0U;
  ra_err_t err                          = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_usb_msc_enum_tries; attempt++) {
    const bool do_reset = (attempt >= (uint8_t)k_usb_msc_enum_no_rst_tries);
    if (do_reset) {
      (void)ra_usb_host_bus_reset(k_ra_usb_speed_fs, true);
      ra_delay_ms(k_usb_msc_reset_hold_ms);
      (void)ra_usb_host_bus_reset(k_ra_usb_speed_fs, false);
    }
    (void)ra_usb_host_set_uact(k_ra_usb_speed_fs, true);
    ra_delay_ms(k_usb_msc_recovery_ms);
    const uint8_t addr = (uint8_t)(attempt & (uint8_t)k_usb_msc_addr_alt_mask);
    (void)ra_usb_host_set_target(k_ra_usb_speed_fs, addr);
    rx  = 0U;
    err = usb_msc_read_dev_desc(desc, &rx);
    if (err == k_ra_ok) {
      if (rx == (uint16_t)k_usb_msc_dev_desc_len) {
        (void)usb_msc_sci_write(k_usb_msc_msg_won, (uint32_t)(sizeof(k_usb_msc_msg_won) - 1U));
        (void)usb_msc_print_dec_u32((uint32_t)attempt);
        (void)usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
        *out_addr = addr;
        break;
      }
      err = k_ra_err_hw_error;
    }
  }
  const uint16_t vid = (uint16_t)((uint16_t)desc[k_usb_msc_desc_vid_off] |
                                  (uint16_t)(desc[k_usb_msc_desc_vid_off + 1U] << 8U));
  const uint16_t pid = (uint16_t)((uint16_t)desc[k_usb_msc_desc_pid_off] |
                                  (uint16_t)(desc[k_usb_msc_desc_pid_off + 1U] << 8U));
  (void)usb_msc_print_attach(vid, pid, (uint8_t)rx);
  return err;
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
/**
 * @brief Route the FS USB pins and bring the host controller up.
 *        Panic-halts on any failure.
 *
 * @pre CGC (incl. USBFS clock) and SCI8 console are already up.
 * @post P4_07/P5_00/P8_14/P8_15 are USB-FS; the controller is in host mode.
 * @since 0.1.0
 */
static void usb_msc_usb_init_or_halt(void)
{
  if (usb_msc_pins_fs_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_usb_host_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_msc_panic_halt();
  }
}

static void usb_msc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  /* Bring up PLL2 -> USBCKCR so the USBFS SIE sees a spec-compliant 48 MHz
   * reference. Must run BEFORE MSTPB11 is released (ra_usb_hmsc_init);
   * without it DVSTCTR0 writes (UACT) never stick and no SOF is generated,
   * so an attached device is detected (LNST=1) but never enumerates. */
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
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

  usb_msc_usb_init_or_halt();
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
 * @pre LED2 GPIO output is initialized.
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
 * @brief Decode a little-endian 32-bit value from a byte buffer.
 *
 * @details Used to recover the dCBWTag that ::ra_usb_hmsc_build_cbw wrote
 * into the CBW so the CSW echo can be verified.
 *
 * @param[in] p Pointer to 4 little-endian bytes.
 * @return The decoded value.
 * @retval 0 All four bytes were zero.
 * @pre @p p is non-NULL and holds at least 4 readable bytes.
 * @pre The bytes are little-endian on the wire.
 * @post No state is modified.
 * @post The return value reflects the 4 bytes exactly.
 * @note Pure function.
 * @since 0.1.0
 */
static uint32_t usb_msc_le32(const uint8_t* p)
{
  uint32_t v = 0U;
  for (uint8_t i = 0U; i <= (uint8_t)k_usb_msc_be32_top_byte; i++) {
    const uint32_t shifted = (uint32_t)p[i] << (uint32_t)(i * (uint8_t)k_usb_msc_bits_per_byte);
    v                      = v | shifted;
  }
  return v;
}

/**
 * @brief Decode a big-endian 32-bit value from a byte buffer.
 *
 * @details SCSI READ_CAPACITY(10) returns its two fields big-endian.
 *
 * @param[in] p Pointer to 4 big-endian bytes.
 * @return The decoded value.
 * @retval 0 All four bytes were zero.
 * @pre @p p is non-NULL and holds at least 4 readable bytes.
 * @pre The bytes are big-endian on the wire.
 * @post No state is modified.
 * @post The return value reflects the 4 bytes exactly.
 * @note Pure function.
 * @since 0.1.0
 */
static uint32_t usb_msc_be32(const uint8_t* p)
{
  uint32_t v = 0U;
  for (uint8_t i = 0U; i <= (uint8_t)k_usb_msc_be32_top_byte; i++) {
    const uint8_t  rev     = (uint8_t)((uint8_t)k_usb_msc_be32_top_byte - i);
    const uint32_t shifted = (uint32_t)p[i] << (uint32_t)(rev * (uint8_t)k_usb_msc_bits_per_byte);
    v                      = v | shifted;
  }
  return v;
}

/**
 * @brief Check whether the descriptor at @p d is the MSC BOT interface.
 *
 * @details Matches bInterfaceClass 0x08 (Mass Storage), bInterfaceSubClass
 * 0x06 (SCSI transparent), bInterfaceProtocol 0x50 (Bulk-Only Transport).
 *
 * @param[in] d Pointer to an interface descriptor.
 * @return Whether the interface is MSC BOT.
 * @retval true  The interface is the MSC BOT one.
 * @retval false Any of the three identity fields differ.
 * @pre @p d points at a descriptor with bDescriptorType INTERFACE.
 * @pre The descriptor is at least 8 bytes long.
 * @post No state is modified.
 * @post The verdict reflects the three class fields only.
 * @note Pure helper for the config-descriptor walk.
 * @since 0.1.0
 */
static bool usb_msc_iface_is_msc(const uint8_t* d)
{
  if (d[k_usb_msc_if_off_class] == (uint8_t)k_usb_msc_class_msc) {
    if (d[k_usb_msc_if_off_sub] == (uint8_t)k_usb_msc_subclass_scsi) {
      if (d[k_usb_msc_if_off_proto] == (uint8_t)k_usb_msc_protocol_bot) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Record a bulk endpoint descriptor into the target snapshot.
 *
 * @details Filters for bmAttributes == bulk and slots the endpoint into the
 * IN or OUT position (first match wins) with its wMaxPacketSize.
 *
 * @param[in]     d   Pointer to an endpoint descriptor.
 * @param[in,out] out Target snapshot being filled.
 * @pre @p d points at a descriptor with bDescriptorType ENDPOINT.
 * @pre @p out is non-NULL and zero-initialised for unfilled slots.
 * @post A matching bulk endpoint is recorded once.
 * @post Non-bulk endpoints leave @p out untouched.
 * @note Pure helper for the config-descriptor walk.
 * @since 0.1.0
 */
static void usb_msc_note_endpoint(const uint8_t* d, usb_msc_target_t* out)
{
  const uint8_t attr = (uint8_t)(d[k_usb_msc_ep_off_attr] & (uint8_t)k_usb_msc_ep_attr_mask);
  if (attr != (uint8_t)k_usb_msc_ep_attr_bulk) {
    return;
  }
  const uint8_t  ea  = d[k_usb_msc_ep_off_addr];
  const uint16_t mps = (uint16_t)((uint16_t)d[k_usb_msc_ep_off_mps] |
                                  (uint16_t)((uint16_t)d[k_usb_msc_ep_off_mps + 1U]
                                             << (uint16_t)k_usb_msc_bits_per_byte));
  if ((ea & (uint8_t)k_usb_msc_ep_dir_in_bit) != 0U) {
    if (out->in_ep == 0U) {
      out->in_ep  = (uint8_t)(ea & (uint8_t)k_usb_msc_ep_num_mask);
      out->in_mps = mps;
    }
  } else {
    if (out->out_ep == 0U) {
      out->out_ep  = (uint8_t)(ea & (uint8_t)k_usb_msc_ep_num_mask);
      out->out_mps = mps;
    }
  }
}

/**
 * @brief Walk a configuration-descriptor blob and fill the MSC snapshot.
 *
 * @details Iterates descriptor-by-descriptor (bLength strides); interface
 * descriptors flip the in-MSC flag via ::usb_msc_iface_is_msc, endpoint
 * descriptors inside the MSC interface are recorded via
 * ::usb_msc_note_endpoint.
 *
 * @param[in]     cfg   Configuration descriptor bytes.
 * @param[in]     len   Valid byte count in @p cfg.
 * @param[in,out] out   Target snapshot being filled.
 * @return Walk outcome.
 * @retval k_ra_ok           Both bulk endpoints were found.
 * @retval k_ra_err_hw_error No MSC bulk endpoint pair in the blob.
 * @pre @p cfg / @p out are non-NULL.
 * @pre @p out endpoint slots start zeroed.
 * @post On k_ra_ok @p out carries both endpoints and the iface number.
 * @post @p cfg is unmodified.
 * @note Helper split out for the clang-tidy size/complexity gate.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_walk_config(const uint8_t* cfg, uint16_t len, usb_msc_target_t* out)
{
  uint16_t off    = 0U;
  bool     in_msc = false;
  while (off < len) {
    const uint8_t dlen = cfg[off + (uint16_t)k_usb_msc_desc_off_len];
    if (dlen == 0U) {
      break;
    }
    const uint8_t dtype = cfg[off + (uint16_t)k_usb_msc_desc_off_type];
    if (dtype == (uint8_t)k_usb_msc_dtype_iface) {
      in_msc = usb_msc_iface_is_msc(&cfg[off]);
      if (in_msc) {
        out->iface = cfg[off + (uint16_t)k_usb_msc_if_off_number];
      }
    }
    if (dtype == (uint8_t)k_usb_msc_dtype_ep) {
      if (in_msc) {
        usb_msc_note_endpoint(&cfg[off], out);
      }
    }
    off = (uint16_t)(off + dlen);
  }
  if (out->in_ep == 0U) {
    return k_ra_err_hw_error;
  }
  if (out->out_ep == 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Read the configuration descriptor and extract the MSC endpoints.
 *
 * @details Reads the 9-byte configuration header for wTotalLength and
 * bConfigurationValue, re-reads the full set (clamped to the local
 * buffer), then hands the blob to ::usb_msc_walk_config. Prints the
 * endpoint report line on success.
 *
 * @param[out] out Target snapshot to fill.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Both bulk endpoints were found.
 * @retval k_ra_err_hw_error The walk found no MSC bulk endpoint pair.
 * @pre The device is addressed and answering control reads.
 * @pre @p out is non-NULL.
 * @post On success @p out carries eps, max packets, iface, cfg value.
 * @post The endpoint report line is printed on success.
 * @note Blocking (two polled control reads).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_read_config(usb_msc_target_t* out)
{
  uint8_t        cfg[k_usb_msc_cfg_buf_len] = {};
  uint16_t       rx                         = 0U;
  ra_usb_setup_t setup                      = {
    .bm_request_type = (uint8_t)k_usb_msc_bmreq_dev_in,
    .b_request       = (uint8_t)k_usb_msc_breq_get_desc,
    .w_value         = (uint16_t)k_usb_msc_wval_cfg_desc,
    .w_index         = 0U,
    .w_length        = (uint16_t)k_usb_msc_cfg_hdr_len,
  };
  ra_err_t err =
    ra_usb_host_control_xfer(k_ra_usb_speed_fs, &setup, cfg, (uint16_t)k_usb_msc_cfg_hdr_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  uint16_t total = (uint16_t)((uint16_t)cfg[k_usb_msc_cfg_off_total] |
                              (uint16_t)((uint16_t)cfg[k_usb_msc_cfg_off_total + 1U]
                                         << (uint16_t)k_usb_msc_bits_per_byte));
  if (total > (uint16_t)k_usb_msc_cfg_buf_len) {
    total = (uint16_t)k_usb_msc_cfg_buf_len;
  }
  out->cfg_value = cfg[k_usb_msc_cfg_off_value];
  setup.w_length = total;
  err            = ra_usb_host_control_xfer(k_ra_usb_speed_fs, &setup, cfg, total, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_msc_walk_config(cfg, rx, out);
  if (err != k_ra_ok) {
    return err;
  }
  (void)usb_msc_sci_write(k_usb_msc_msg_eps_pre, (uint32_t)(sizeof(k_usb_msc_msg_eps_pre) - 1U));
  uint8_t hexb[k_usb_msc_hex_chars_u8] = {};
  usb_msc_format_hex_u8(out->in_ep, hexb);
  (void)usb_msc_sci_write(hexb, (uint32_t)k_usb_msc_hex_chars_u8);
  (void)usb_msc_sci_write(k_usb_msc_msg_eps_mid, (uint32_t)(sizeof(k_usb_msc_msg_eps_mid) - 1U));
  usb_msc_format_hex_u8(out->out_ep, hexb);
  (void)usb_msc_sci_write(hexb, (uint32_t)k_usb_msc_hex_chars_u8);
  return usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
}

/**
 * @brief SET_CONFIGURATION, fetch max LUN (best effort), set up the pipes.
 *
 * @details Activates the parsed bConfigurationValue (no-data control, the
 * strict status path), issues the class GET_MAX_LUN (devices may STALL it,
 * which legally means LUN 0, so failures default to 0), then programs
 * PIPE1 against the bulk-IN endpoint and PIPE2 against the bulk-OUT
 * endpoint at address 1.
 *
 * @param[in]  tgt      Parsed MSC target snapshot.
 * @param[in]  dev_addr Address the device answers at (from the addr phase).
 * @param[out] out_lun  Receives the max LUN (0 on GET_MAX_LUN failure).
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Device configured and both pipes are ready.
 * @pre ::usb_msc_read_config filled @p tgt.
 * @pre The DCP targets @p dev_addr.
 * @post On success the device is configured and PIPE1/PIPE2 are armed-able.
 * @post The "configured, pipes ready" line is printed on success.
 * @note Blocking (polled control transfers).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_configure(const usb_msc_target_t* tgt, uint8_t dev_addr, uint8_t* out_lun)
{
  const ra_usb_setup_t set_cfg = {
    .bm_request_type = (uint8_t)k_usb_msc_bmreq_dev_out,
    .b_request       = (uint8_t)k_usb_msc_breq_set_config,
    .w_value         = tgt->cfg_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_fs, &set_cfg, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_usb_setup_t get_lun = {
    .bm_request_type = (uint8_t)k_usb_msc_bmreq_class_if_in,
    .b_request       = (uint8_t)k_usb_msc_breq_get_max_lun,
    .w_value         = 0U,
    .w_index         = tgt->iface,
    .w_length        = 1U,
  };
  uint8_t        lun     = 0U;
  uint16_t       lun_rx  = 0U;
  const ra_err_t lun_err = ra_usb_host_control_xfer(k_ra_usb_speed_fs, &get_lun, &lun, 1U, &lun_rx);
  *out_lun               = 0U;
  if (lun_err == k_ra_ok) {
    if (lun_rx == 1U) {
      *out_lun = lun;
    }
  }
  err = ra_usb_host_pipe_setup(k_ra_usb_speed_fs,
                               (uint8_t)k_usb_msc_pipe_in,
                               dev_addr,
                               tgt->in_ep,
                               true,
                               tgt->in_mps);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_host_pipe_setup(k_ra_usb_speed_fs,
                               (uint8_t)k_usb_msc_pipe_out,
                               dev_addr,
                               tgt->out_ep,
                               false,
                               tgt->out_mps);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_msc_sci_write(k_usb_msc_msg_cfg_ok, (uint32_t)(sizeof(k_usb_msc_msg_cfg_ok) - 1U));
}

/**
 * @brief Run one MSC Bulk-Only-Transport exchange: CBW, data-IN, CSW.
 *
 * @details Ships the 31-byte CBW on the OUT pipe, gathers the optional
 * data-IN payload, then reads and validates the 13-byte CSW (signature,
 * tag echo recovered from the CBW bytes, status == passed). One LED2
 * blink marks each completed exchange.
 *
 * @param[in]  cbw      The 31-byte CBW to transmit.
 * @param[out] data     Data-IN destination (may be NULL when none).
 * @param[in]  data_len Expected data-IN length (0 for no data stage).
 * @param[out] out_rx   Receives the data-IN byte count (may be NULL).
 * @return First failing phase's error, or k_ra_ok.
 * @retval k_ra_ok           CSW says the command passed.
 * @retval k_ra_err_hw_error Short CSW or CSW status != passed.
 * @pre ::usb_msc_configure set up PIPE1/PIPE2.
 * @pre @p cbw holds a CBW built by ::ra_usb_hmsc_build_cbw.
 * @post On success the device accepted and completed the command.
 * @post @p out_rx reflects the data stage when present.
 * @note Blocking; one bounded wait per bulk phase.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_bot(const uint8_t* cbw, uint8_t* data, uint16_t data_len, uint16_t* out_rx)
{
  ra_err_t err = ra_usb_host_bulk_out(k_ra_usb_speed_fs,
                                      (uint8_t)k_usb_msc_pipe_out,
                                      cbw,
                                      (uint16_t)k_usb_msc_cbw_len);
  if (err != k_ra_ok) {
    return err;
  }
  if (data_len > 0U) {
    uint16_t rx = 0U;
    err = ra_usb_host_bulk_in(k_ra_usb_speed_fs, (uint8_t)k_usb_msc_pipe_in, data, data_len, &rx);
    if (err != k_ra_ok) {
      return err;
    }
    if (out_rx != nullptr) {
      *out_rx = rx;
    }
  }
  uint8_t  csw[k_usb_msc_csw_len] = {};
  uint16_t csw_rx                 = 0U;
  err                             = ra_usb_host_bulk_in(k_ra_usb_speed_fs,
                                                        (uint8_t)k_usb_msc_pipe_in,
                                                        csw,
                                                        (uint16_t)k_usb_msc_csw_len,
                                                        &csw_rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (csw_rx != (uint16_t)k_usb_msc_csw_len) {
    return k_ra_err_hw_error;
  }
  const uint32_t           tag    = usb_msc_le32(&cbw[k_usb_msc_cbw_tag_off]);
  ra_usb_hmsc_csw_status_t status = k_ra_hmsc_csw_status_phase_error;
  err                             = ra_usb_hmsc_decode_csw(csw, tag, &status);
  if (err != k_ra_ok) {
    return err;
  }
  if (status != k_ra_hmsc_csw_status_passed) {
    return k_ra_err_hw_error;
  }
  return usb_msc_blink_op();
}

/**
 * @brief SCSI INQUIRY over BOT; print the decoded identity strings.
 *
 * @details Builds the 6-byte INQUIRY CDB (36-byte allocation), runs the
 * exchange, copies the vendor/product/revision ASCII fields into the
 * decoded struct, and prints the INQUIRY line.
 *
 * @return First failing phase's error, or k_ra_ok.
 * @retval k_ra_ok INQUIRY passed and the line is printed.
 * @pre ::usb_msc_configure set up the pipes.
 * @pre The device passed enumeration at address 1.
 * @post The INQUIRY identity line is printed on success.
 * @post No persistent state changes.
 * @note Blocking (one BOT exchange).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_scsi_inquiry(void)
{
  uint8_t cdb[k_usb_msc_cdb6_len] = {};
  cdb[0]                          = (uint8_t)k_ra_hmsc_scsi_inquiry;
  cdb[k_usb_msc_cdb_alloc_off]    = (uint8_t)k_ra_hmsc_inquiry_resp_len;
  uint8_t  cbw[k_usb_msc_cbw_len] = {};
  ra_err_t err                    = ra_usb_hmsc_build_cbw(0U,
                                                          (uint32_t)k_ra_hmsc_inquiry_resp_len,
                                                          true,
                                                          cdb,
                                                          (uint8_t)k_usb_msc_cdb6_len,
                                                          cbw);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t  data[k_ra_hmsc_inquiry_resp_len] = {};
  uint16_t rx                               = 0U;
  err = usb_msc_bot(cbw, data, (uint16_t)k_ra_hmsc_inquiry_resp_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  ra_usb_hmsc_inquiry_response_t resp = {};
  for (uint8_t i = 0U; i < (uint8_t)sizeof resp.vendor_id; i++) {
    resp.vendor_id[i] = data[(uint8_t)k_usb_msc_inq_vendor_off + i];
  }
  for (uint8_t i = 0U; i < (uint8_t)sizeof resp.product_id; i++) {
    resp.product_id[i] = data[(uint8_t)k_usb_msc_inq_product_off + i];
  }
  for (uint8_t i = 0U; i < (uint8_t)sizeof resp.product_revision; i++) {
    resp.product_revision[i] = data[(uint8_t)k_usb_msc_inq_rev_off + i];
  }
  return usb_msc_print_inquiry(&resp);
}

/**
 * @brief SCSI READ_CAPACITY(10) over BOT; print blocks and block size.
 *
 * @details Builds the 10-byte CDB, runs the exchange, decodes the two
 * big-endian fields (last LBA and block length), and prints the capacity
 * line (block count = last LBA + 1).
 *
 * @return First failing phase's error, or k_ra_ok.
 * @retval k_ra_ok Capacity decoded and printed.
 * @pre ::usb_msc_configure set up the pipes.
 * @pre The device answers SCSI commands (INQUIRY passed).
 * @post The capacity line is printed on success.
 * @post No persistent state changes.
 * @note Blocking (one BOT exchange).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_scsi_capacity(void)
{
  uint8_t cdb[k_usb_msc_cdb10_len] = {};
  cdb[0]                           = (uint8_t)k_ra_hmsc_scsi_read_capacity_10;
  uint8_t  cbw[k_usb_msc_cbw_len]  = {};
  ra_err_t err = ra_usb_hmsc_build_cbw(0U,
                                       (uint32_t)k_ra_hmsc_read_capacity_resp_len,
                                       true,
                                       cdb,
                                       (uint8_t)k_usb_msc_cdb10_len,
                                       cbw);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t  data[k_ra_hmsc_read_capacity_resp_len] = {};
  uint16_t rx                                     = 0U;
  err = usb_msc_bot(cbw, data, (uint16_t)k_ra_hmsc_read_capacity_resp_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t last_lba   = usb_msc_be32(&data[0]);
  const uint32_t block_size = usb_msc_be32(&data[(uint8_t)k_usb_msc_cdb_alloc_off]);
  return usb_msc_print_capacity((uint32_t)(last_lba + 1U), block_size);
}

/**
 * @brief SCSI READ(10) of LBA 0 over BOT; dump and verify the MBR.
 *
 * @details Builds the 10-byte READ(10) CDB for one block at LBA 0, runs
 * the exchange into @p sector, prints the 64-byte hex dump, then prints
 * the boot-signature verdict from bytes 510/511 (expect 55 AA).
 *
 * @param[out] sector Buffer of one 512-byte block.
 * @return First failing phase's error, or k_ra_ok.
 * @retval k_ra_ok           Sector read; dump and verdict printed.
 * @retval k_ra_err_hw_error The read returned fewer than 512 bytes.
 * @pre ::usb_msc_configure set up the pipes.
 * @pre @p sector holds at least 512 bytes.
 * @post @p sector carries LBA 0 on success.
 * @post The dump and signature verdict are printed on success.
 * @note Blocking (one BOT exchange).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_scsi_read_mbr(uint8_t* sector)
{
  uint8_t cdb[k_usb_msc_cdb10_len] = {};
  cdb[0]                           = (uint8_t)k_ra_hmsc_scsi_read_10;
  cdb[k_usb_msc_read10_cnt_off]    = (uint8_t)k_usb_msc_one_block;
  uint8_t  cbw[k_usb_msc_cbw_len]  = {};
  ra_err_t err                     = ra_usb_hmsc_build_cbw(0U,
                                                           (uint32_t)k_usb_msc_block_size_bytes,
                                                           true,
                                                           cdb,
                                                           (uint8_t)k_usb_msc_cdb10_len,
                                                           cbw);
  if (err != k_ra_ok) {
    return err;
  }
  uint16_t rx = 0U;
  err         = usb_msc_bot(cbw, sector, (uint16_t)k_usb_msc_block_size_bytes, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != (uint16_t)k_usb_msc_block_size_bytes) {
    return k_ra_err_hw_error;
  }
  err = usb_msc_dump_first_bytes(sector);
  if (err != k_ra_ok) {
    return err;
  }
  (void)usb_msc_sci_write(k_usb_msc_msg_sig_pre, (uint32_t)(sizeof(k_usb_msc_msg_sig_pre) - 1U));
  uint8_t hexb[k_usb_msc_hex_chars_u8] = {};
  usb_msc_format_hex_u8(sector[k_usb_msc_mbr_sig_off_lo], hexb);
  (void)usb_msc_sci_write(hexb, (uint32_t)k_usb_msc_hex_chars_u8);
  (void)usb_msc_sci_write(k_usb_msc_msg_space, (uint32_t)(sizeof(k_usb_msc_msg_space) - 1U));
  usb_msc_format_hex_u8(sector[k_usb_msc_mbr_sig_off_hi], hexb);
  (void)usb_msc_sci_write(hexb, (uint32_t)k_usb_msc_hex_chars_u8);
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
 * @brief Move the device from address 0 to address 1 and verify.
 *
 * @details SET_CONFIGURATION is only legal from the Address state (the
 * stick STALLs it at the default address), so assign address 1, honour
 * the set-address recovery, retarget the DCP, and verify with a full
 * descriptor re-read at the new address.
 *
 * @param[in,out] dev_addr In: address the device answered at. Out: the
 *                         address after the phase (1 on success).
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The device answers at address 1.
 * @pre The enumeration hunt found the device at `*dev_addr`.
 * @pre The bus is active (UACT on).
 * @post On success the DCP targets address 1 for all later transfers.
 * @post On failure the DCP target is left at the hunt's address.
 * @note Skipped when the hunt already found the device addressed.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_assign_addr(uint8_t* dev_addr)
{
  if (*dev_addr != 0U) {
    return k_ra_ok;
  }
  const ra_usb_setup_t set_addr = {
    .bm_request_type = (uint8_t)k_usb_msc_bmreq_dev_out,
    .b_request       = (uint8_t)k_usb_msc_breq_set_addr,
    .w_value         = (uint16_t)k_usb_msc_test_address,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_fs, &set_addr, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_usb_msc_addr_settle_ms);
  err = ra_usb_host_set_target(k_ra_usb_speed_fs, (uint8_t)k_usb_msc_test_address);
  if (err != k_ra_ok) {
    return err;
  }
  /* No verify read here: the configuration-descriptor read that follows
   * is the verification, and skipping the extra transfer keeps the
   * exchange count minimal for protocol-fragile sticks. */
  *dev_addr = (uint8_t)k_usb_msc_test_address;
  return usb_msc_sci_write(k_usb_msc_msg_addr1, (uint32_t)(sizeof(k_usb_msc_msg_addr1) - 1U));
}

/**
 * @brief Report a ladder failure: step id, error code, register snapshot.
 *
 * @details Single failure sink so every step prints the same triple:
 * "FAIL step=<n>", the error code, and the controller diagnostic line.
 *
 * @param[in] step Ladder step identifier that failed.
 * @param[in] err  Error the step returned.
 * @pre SCI8 init already ran.
 * @pre @p err is not k_ra_ok.
 * @post The failure lines are queued on SCI8.
 * @post No other state changes.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void usb_msc_report_fail(usb_host_msc_step_t step, ra_err_t err)
{
  (void)usb_msc_sci_write(k_usb_msc_msg_step_fail,
                          (uint32_t)(sizeof(k_usb_msc_msg_step_fail) - 1U));
  (void)usb_msc_print_dec_u32((uint32_t)step);
  (void)usb_msc_sci_write(k_usb_msc_msg_crlf, (uint32_t)(sizeof(k_usb_msc_msg_crlf) - 1U));
  (void)usb_msc_print_scsi_err(err);
  usb_msc_print_enum_diag();
}

/**
 * @brief Climb the full host-MSC ladder: enumerate, configure, read LBA 0.
 *
 * @details Runs every bring-up step in order and stops at the first
 * failure, which is reported through ::usb_msc_report_fail. On full
 * success LED1 is lit and the MBR dump plus signature verdict are on the
 * console.
 *
 * @return Outcome of the full climb.
 * @retval k_ra_ok Every step passed; the MBR dump is on the console.
 * @pre ::usb_msc_setup_or_halt completed (clocks, console, pins, host).
 * @pre The USB drive is inserted in the FS receptacle.
 * @post On success the MBR dump and "(ok)" verdict have been printed.
 * @post On failure the step id, error, and register snapshot are printed.
 * @note The caller retries on failure (reseated drives are picked up).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_run_ladder(void)
{
  static uint8_t s_sector[k_usb_msc_block_size_bytes] = {};

  uint8_t  dev_addr = 0U;
  ra_err_t err      = usb_msc_enumerate(&dev_addr);
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_dev_desc, err);
    return err;
  }
  err = usb_msc_assign_addr(&dev_addr);
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_set_addr, err);
    return err;
  }
  usb_msc_target_t tgt = {};
  err                  = usb_msc_read_config(&tgt);
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_config, err);
    return err;
  }
  uint8_t max_lun = 0U;
  err             = usb_msc_configure(&tgt, dev_addr, &max_lun);
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_set_cfg, err);
    return err;
  }
  err = usb_msc_scsi_inquiry();
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_inquiry, err);
    return err;
  }
  err = usb_msc_scsi_capacity();
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_capacity, err);
    return err;
  }
  err = usb_msc_scsi_read_mbr(s_sector);
  if (err != k_ra_ok) {
    usb_msc_report_fail(k_usb_msc_step_read_mbr, err);
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led1);
  return k_ra_ok;
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
 * @brief Application entry. Brings up the FS host and climbs the MSC
 *        ladder once: enumerate, configure, INQUIRY, capacity, MBR dump.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On a clean run the MBR dump + signature verdict are printed and
 *       LED1 is lit; the CPU then parks in WFI.
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

  /* Climb the ladder until it succeeds: a freshly inserted (or reseated)
   * drive is picked up by the next retry cycle automatically. */
  while (usb_msc_run_ladder() != k_ra_ok) {
    (void)usb_msc_sci_write(k_usb_msc_msg_retry, (uint32_t)(sizeof(k_usb_msc_msg_retry) - 1U));
    ra_delay_ms(k_usb_msc_retry_ms);
  }

  while (1) {
    ra_delay_ms(k_usb_msc_idle_ms);
    __asm__ volatile("wfi");
  }

  return 0;
}
#pragma GCC diagnostic pop
