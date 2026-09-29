/**
 * @file ra8_fuelgauge_max17048_regs.h
 * @brief Register layout for the Analog Devices MAX17048 fuel gauge
 * @ingroup grp_hal_usb
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The MAX17048 is a 1-cell ModelGauge host-side fuel gauge on a 7-bit
 * I2C address of 0x36. It is the gauge on the EK-RA8D2 ereader carrier
 * this project targets, sitting on the same low-speed I2C segment as the
 * touch controller.
 *
 * Every register is 16 bits wide and is moved MSB-first, addressed by a
 * one-byte register pointer, so a read is the usual
 * "write pointer, RESTART, read two bytes" transfer.
 *
 * | Pointer | Name    | Purpose                                         |
 * |--------:|---------|-------------------------------------------------|
 * |  0x02   | VCELL   | Cell voltage, 78.125 uV per LSB                 |
 * |  0x04   | SOC     | State of charge, 1/256 %% per LSB               |
 * |  0x06   | MODE    | QuickStart / sleep controls (write-only use)    |
 * |  0x08   | VERSION | Silicon revision, read back as a liveness probe |
 * |  0x0C   | CONFIG  | Alert threshold + sleep bit                     |
 * |  0x16   | CRATE   | Signed charge rate, 0.208 %%/hr per LSB         |
 *
 * The scale factors are the two this driver actually applies:
 *   - VCELL: 78.125 uV per LSB, which is exactly 5/64 mV, so millivolts
 *     come out of integer arithmetic with no rounding step.
 *   - SOC: the high byte is the integer percent, which is what both
 *     in-tree consumers already read; the low byte is the 1/256
 *     fraction and is dropped by ::ra8_fuelgauge_state_t.
 *
 * Cross-checked against the Analog Devices MAX17048/MAX17049 data sheet
 * (rev 11, publicly distributed; this project does not vendor the
 * document) and against the two in-tree readers this driver replaces,
 * `examples/ek_ra8d2/hw_validated/hil/ereader_ui` and
 * `examples/ek_ra8d2/hw_pending/battery_monitor_demo`, which both use
 * the 0x36 / 0x04 / 0x16 / sign-bit spelling repeated here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_fuelgauge_max17048_addr_t
 * @brief I2C target address for the MAX17048.
 *
 * @details
 * The part has one fixed address; it is not strap-selectable. Boards
 * that put a second gauge on the same segment need a second segment.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fuelgauge_max17048_addr_7b = 0x36U, /**< Fixed 7-bit address. */
} ra8_fuelgauge_max17048_addr_t;

/**
 * @enum ra8_fuelgauge_max17048_reg_t
 * @brief One-byte register pointers, MSB-first 16-bit registers.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fuelgauge_max17048_reg_vcell   = 0x02U, /**< Cell voltage.       */
  k_ra8_fuelgauge_max17048_reg_soc     = 0x04U, /**< State of charge.    */
  k_ra8_fuelgauge_max17048_reg_mode    = 0x06U, /**< Mode controls.      */
  k_ra8_fuelgauge_max17048_reg_version = 0x08U, /**< Silicon revision.   */
  k_ra8_fuelgauge_max17048_reg_config  = 0x0CU, /**< Alert + sleep.      */
  k_ra8_fuelgauge_max17048_reg_crate   = 0x16U, /**< Signed charge rate. */
} ra8_fuelgauge_max17048_reg_t;

/**
 * @enum ra8_fuelgauge_max17048_wire_t
 * @brief Wire-format widths and the scale factors applied by the driver.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_fuelgauge_max17048_reg_ptr_bytes = 1U,  /**< Pointer width. */
  k_ra8_fuelgauge_max17048_reg_bytes     = 2U,  /**< 16-bit regs.   */
  k_ra8_fuelgauge_max17048_vcell_mv_num  = 5U,  /**< 5/64 mV, num.  */
  k_ra8_fuelgauge_max17048_vcell_mv_den  = 64U, /**< 5/64 mV, den.  */
} ra8_fuelgauge_max17048_wire_t;

#ifdef __cplusplus
}
#endif
