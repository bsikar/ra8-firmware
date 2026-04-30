/**
 * @file ra8d2_opamp_regs.h
 * @brief Op-amp (AMP) register layout for the Renesas RA8D2
 *
 * @details
 * Models the FSP `r_opamp`-shaped block (AMP module) as a thin
 * register window so the `ra_opamp` driver can drive per-channel
 * enable, gain, and input-pin selection without reaching into the
 * driver's private state. The block is laid out as:
 *
 *  - `AMPMC`    Mode control (output buffer + low-power mode).
 *  - `AMPC`     Channel enable (one bit per AMP channel).
 *  - `AMPMS`    Mode-select per channel.
 *  - `AMPTRS`   Trigger select per channel.
 *  - `AMPGAIN`  Gain selector (4 channels x 4 bits).
 *  - `AMPINSEL` Input pin selector (4 channels x 4 bits).
 *
 * On RA8D2 the AMP block is reachable via the peripheral bus at
 * `0x40313000` (within the analog peripheral cluster). The
 * `RA_SIMULATOR_MODE` host-test backing maps the whole peripheral
 * window so tests can drive the registers without faulting.
 *
 * Reference: FSP `r_opamp` BSP register layout. The exact bit
 * positions are MCU-group-specific; this header models the subset
 * the `ra_opamp` HAL touches and keeps the rest reserved.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra_opamp_addr_t
 * @brief Memory-mapped base address for the AMP block.
 */
typedef enum : uintptr_t {
  k_ra_opamp_base_addr = 0x40313000UL, /**< AMP peripheral base. */
} ra_opamp_addr_t;

/**
 * @enum ra_opamp_layout_t
 * @brief Authoritative struct offsets used by the layout asserts below.
 */
typedef enum : uint8_t {
  k_ra_opamp_layout_size       = 0x10U, /**< Block size in bytes.   */
  k_ra_opamp_layout_off_ampmc  = 0x00U, /**< AMPMC offset.          */
  k_ra_opamp_layout_off_ampc   = 0x01U, /**< AMPC offset.           */
  k_ra_opamp_layout_off_ampms  = 0x02U, /**< AMPMS offset.          */
  k_ra_opamp_layout_off_amptrs = 0x03U, /**< AMPTRS offset.         */
  k_ra_opamp_layout_off_gain   = 0x04U, /**< AMPGAIN offset.        */
  k_ra_opamp_layout_off_insel  = 0x08U, /**< AMPINSEL offset.       */
} ra_opamp_layout_t;

/**
 * @struct r_opamp_regs_t
 * @brief AMP register window.
 */
typedef struct {
  volatile uint8_t  AMPMC;    /**< +0x00 Mode control.                  */
  volatile uint8_t  AMPC;     /**< +0x01 Channel enable bitmask.         */
  volatile uint8_t  AMPMS;    /**< +0x02 Mode select per channel.        */
  volatile uint8_t  AMPTRS;   /**< +0x03 Trigger select per channel.     */
  volatile uint16_t AMPGAIN;  /**< +0x04 Gain selector (4 ch x 4 bits).  */
  volatile uint16_t _r0;      /**< +0x06 reserved.                       */
  volatile uint16_t AMPINSEL; /**< +0x08 Input pin select (4 ch x 4 b).  */
  volatile uint16_t _r1[3];   /**< +0x0A..+0x0F reserved.                */
} r_opamp_regs_t;

static_assert(sizeof(r_opamp_regs_t) == (size_t)k_ra_opamp_layout_size,
              "r_opamp_regs_t must be 16 bytes");
static_assert(offsetof(r_opamp_regs_t, AMPMC) == (size_t)k_ra_opamp_layout_off_ampmc,
              "AMPMC at +0x00");
static_assert(offsetof(r_opamp_regs_t, AMPC) == (size_t)k_ra_opamp_layout_off_ampc,
              "AMPC at +0x01");
static_assert(offsetof(r_opamp_regs_t, AMPGAIN) == (size_t)k_ra_opamp_layout_off_gain,
              "AMPGAIN at +0x04");
static_assert(offsetof(r_opamp_regs_t, AMPINSEL) == (size_t)k_ra_opamp_layout_off_insel,
              "AMPINSEL at +0x08");

/**
 * @brief Get pointer to the AMP register window.
 */
static inline volatile r_opamp_regs_t* ra_opamp(void)
{
  return (volatile r_opamp_regs_t*)k_ra_opamp_base_addr;
}

/**
 * @enum ra_opamp_channel_t
 * @brief Op-amp channel indices (0..3).
 *
 * @details FSP `r_opamp` exposes up to four channels per AMP block.
 */
typedef enum : uint8_t {
  k_ra_opamp_ch0    = 0U,
  k_ra_opamp_ch1    = 1U,
  k_ra_opamp_ch2    = 2U,
  k_ra_opamp_ch3    = 3U,
  k_ra_opamp_ch_max = 4U, /**< Channel count.                  */
} ra_opamp_channel_t;

/**
 * @enum ra_opamp_field_t
 * @brief Per-channel field widths and shifts.
 *
 * @details Each gain / input-select nibble occupies four bits in the
 * 16-bit AMPGAIN / AMPINSEL registers, channel `ch` at bit
 * `ch * k_ra_opamp_field_width`.
 */
typedef enum : uint8_t {
  k_ra_opamp_field_width = 4U, /**< Bits per channel.           */
} ra_opamp_field_t;

/**
 * @enum ra_opamp_field_mask_t
 * @brief Per-channel nibble mask.
 *
 * @details Stored in the wider underlying type so the mask survives
 * shifts into a 16-bit register without truncation.
 */
typedef enum : uint16_t {
  k_ra_opamp_field_mask = 0x000FU, /**< Mask for one nibble.        */
} ra_opamp_field_mask_t;

#ifdef __cplusplus
}
#endif
