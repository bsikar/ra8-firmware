/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_touch_gt911_regs.h
 * @brief Register layout for the GoodIX GT911 capacitive touch controller
 * @ingroup grp_hal_usb
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The GT911 is a 5-point projected-capacitive touch IC commonly paired
 * with parallel TFT panels in the 5"-10" size range. It is the touch
 * backend for the EK-RA8D2 ereader carrier this project targets, sitting
 * on the I2C0 bus alongside other low-speed peripherals.
 *
 * Address space (16-bit register pointers):
 *
 * | Offset   | Name                | Width | Purpose                           |
 * |---------:|---------------------|------:|-----------------------------------|
 * |  0x8040  | GTP_COMMAND         |     8 | Command (0x00 sleep, 0x01 wake)   |
 * |  0x8047  | CONFIG_VERSION      |     8 | First byte of config block        |
 * |  0x8140  | PRODUCT_ID (4 char) |    32 | "911"\0 ASCII id                  |
 * |  0x8144  | FIRMWARE_VERSION    |    16 | Little-endian firmware revision   |
 * |  0x8146  | X_RESOLUTION        |    16 | Reported panel X resolution       |
 * |  0x8148  | Y_RESOLUTION        |    16 | Reported panel Y resolution       |
 * |  0x814E  | STATUS              |     8 | bit7 buffer-ready, bits[3:0] cnt  |
 * |  0x814F  | POINT[0..4] base    |   8x5 | Five 8-byte point records         |
 *
 * Per-point record (8 bytes, little-endian for the 16-bit fields):
 *
 * | Off | Field      | Notes                                 |
 * |----:|------------|---------------------------------------|
 * |  0  | track_id   | Persistent contact id, 0..15          |
 * |  1  | x_lsb      | X coordinate low byte                 |
 * |  2  | x_msb      | X coordinate high byte                |
 * |  3  | y_lsb      | Y coordinate low byte                 |
 * |  4  | y_msb      | Y coordinate high byte                |
 * |  5  | size_lsb   | Contact area size low byte (pressure) |
 * |  6  | size_msb   | Contact area size high byte           |
 * |  7  | reserved   | Always 0                              |
 *
 * Cross-checked against GoodIX "GT911 Programming Guide" rev 0.1
 * (publicly distributed reference; this project does not vendor the
 * document).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_touch_gt911_addr_t
 * @brief I2C target addresses for the GT911.
 *
 * @details
 * The GT911 latches its 7-bit address from the INT pin level at reset:
 * INT low yields 0x5D, INT high yields 0x14. The factory default on the
 * EK-RA8D2 ereader carrier is 0x5D.
 */
typedef enum : uint8_t {
  k_ra8_touch_gt911_addr_low  = 0x5DU, /**< Default 7-bit target address.   */
  k_ra8_touch_gt911_addr_high = 0x14U, /**< Alternate 7-bit target address. */
} ra8_touch_gt911_addr_t;

/**
 * @enum ra8_touch_gt911_reg_t
 * @brief 16-bit register pointers used by the driver.
 */
typedef enum : uint16_t {
  k_ra8_touch_gt911_reg_command  = 0x8040U, /**< Command register.           */
  k_ra8_touch_gt911_reg_config   = 0x8047U, /**< First byte of config block. */
  k_ra8_touch_gt911_reg_product  = 0x8140U, /**< 4-byte product id.          */
  k_ra8_touch_gt911_reg_firmware = 0x8144U, /**< Firmware revision.          */
  k_ra8_touch_gt911_reg_xres     = 0x8146U, /**< Reported X resolution.      */
  k_ra8_touch_gt911_reg_yres     = 0x8148U, /**< Reported Y resolution.      */
  k_ra8_touch_gt911_reg_status   = 0x814EU, /**< Status / point-count byte.  */
  k_ra8_touch_gt911_reg_point0   = 0x814FU, /**< First per-point record.     */
} ra8_touch_gt911_reg_t;

/**
 * @enum ra8_touch_gt911_cmd_t
 * @brief Values written into ::k_ra8_touch_gt911_reg_command.
 */
typedef enum : uint8_t {
  k_ra8_touch_gt911_cmd_clear_status   = 0x00U, /**< Acknowledge a touch frame. */
  k_ra8_touch_gt911_cmd_software_reset = 0x02U, /**< Soft reset.                */
} ra8_touch_gt911_cmd_t;

/**
 * @enum ra8_touch_gt911_status_mask_t
 * @brief Bit layout of the status byte at ::k_ra8_touch_gt911_reg_status.
 */
typedef enum : uint8_t {
  k_ra8_touch_gt911_status_count_mask = 0x0FU, /**< bits[3:0] active touch count. */
  k_ra8_touch_gt911_status_large_mask = 0x40U, /**< bit6 large-detect.            */
  k_ra8_touch_gt911_status_ready_mask = 0x80U, /**< bit7 buffer-ready.            */
} ra8_touch_gt911_status_mask_t;

/**
 * @enum ra8_touch_gt911_geometry_t
 * @brief Compile-time geometry constants for the controller.
 */
typedef enum : uint8_t {
  k_ra8_touch_gt911_max_points    = 5U, /**< Max simultaneous contacts.     */
  k_ra8_touch_gt911_point_bytes   = 8U, /**< Bytes per per-point record.    */
  k_ra8_touch_gt911_id_bytes      = 4U, /**< Length of PRODUCT_ID payload.  */
  k_ra8_touch_gt911_reg_ptr_bytes = 2U, /**< 16-bit register-pointer width. */
} ra8_touch_gt911_geometry_t;

/**
 * @enum ra8_touch_gt911_point_offset_t
 * @brief Byte offsets of each field inside one 8-byte point record.
 */
typedef enum : uint8_t {
  k_ra8_touch_gt911_point_off_track    = 0U, /**< track_id field. */
  k_ra8_touch_gt911_point_off_x_lsb    = 1U, /**< X low byte.     */
  k_ra8_touch_gt911_point_off_x_msb    = 2U, /**< X high byte.    */
  k_ra8_touch_gt911_point_off_y_lsb    = 3U, /**< Y low byte.     */
  k_ra8_touch_gt911_point_off_y_msb    = 4U, /**< Y high byte.    */
  k_ra8_touch_gt911_point_off_size_lsb = 5U, /**< Size low byte.  */
  k_ra8_touch_gt911_point_off_size_msb = 6U, /**< Size high byte. */
  k_ra8_touch_gt911_point_off_reserved = 7U, /**< Always 0.       */
} ra8_touch_gt911_point_offset_t;

#ifdef __cplusplus
}
#endif
