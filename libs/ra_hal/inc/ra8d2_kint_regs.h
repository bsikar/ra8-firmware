/**
 * @file ra8d2_kint_regs.h
 * @brief Key-matrix interrupt (KINT) register layout for the Renesas RA8D2
 *
 * @details
 * Models the FSP `r_kint`-shaped block. The KINT peripheral
 * implements a small column-driven row-scan engine for keypad-style
 * input arrays:
 *
 *  - `KRCTL` Control register (column scan enable / mode select).
 *  - `KRF`   Flag register (latched per-row press flags).
 *  - `KRM`   Mask register (per-row IRQ mask).
 *  - `KRSTR` Status register (live row state, snapshot of input pins).
 *
 * The block lives at `0x40080500` (peripheral bus, near IOPORT). The
 * test backing for `RA_SIMULATOR_MODE` covers the whole peri window
 * so register accesses land in host RAM.
 *
 * Reference: FSP `r_kint` driver shape. Layout is MCU-group specific;
 * this header models the subset the `ra_kint` HAL touches.
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
 * @enum ra_kint_addr_t
 * @brief Memory-mapped base address for the KINT block.
 */
typedef enum : uintptr_t {
  k_ra_kint_base_addr = 0x40080500UL, /**< KINT peripheral base. */
} ra_kint_addr_t;

/**
 * @enum ra_kint_layout_t
 * @brief Authoritative struct offsets used by the layout asserts below.
 */
typedef enum : uint8_t {
  k_ra_kint_layout_size      = 0x08U, /**< Block size in bytes. */
  k_ra_kint_layout_off_krctl = 0x00U,
  k_ra_kint_layout_off_krf   = 0x02U,
  k_ra_kint_layout_off_krm   = 0x04U,
  k_ra_kint_layout_off_krstr = 0x06U,
} ra_kint_layout_t;

/**
 * @struct r_kint_regs_t
 * @brief KINT register window.
 */
typedef struct {
  volatile uint8_t KRCTL; /**< +0x00 Control (KRMD scan / IRQ enable).  */
  volatile uint8_t _r0;   /**< +0x01 reserved.                          */
  volatile uint8_t KRF;   /**< +0x02 Flags (latched per-row).           */
  volatile uint8_t _r1;   /**< +0x03 reserved.                          */
  volatile uint8_t KRM;   /**< +0x04 Mask (per-row IRQ enable).         */
  volatile uint8_t _r2;   /**< +0x05 reserved.                          */
  volatile uint8_t KRSTR; /**< +0x06 Status (live row state).           */
  volatile uint8_t _r3;   /**< +0x07 reserved.                          */
} r_kint_regs_t;

static_assert(sizeof(r_kint_regs_t) == (size_t)k_ra_kint_layout_size,
              "r_kint_regs_t must be 8 bytes");
static_assert(offsetof(r_kint_regs_t, KRCTL) == (size_t)k_ra_kint_layout_off_krctl,
              "KRCTL at +0x00");
static_assert(offsetof(r_kint_regs_t, KRF) == (size_t)k_ra_kint_layout_off_krf, "KRF at +0x02");
static_assert(offsetof(r_kint_regs_t, KRM) == (size_t)k_ra_kint_layout_off_krm, "KRM at +0x04");
static_assert(offsetof(r_kint_regs_t, KRSTR) == (size_t)k_ra_kint_layout_off_krstr,
              "KRSTR at +0x06");

/**
 * @brief Get pointer to the KINT register window.
 */
static inline volatile r_kint_regs_t* ra_kint(void)
{
  return (volatile r_kint_regs_t*)k_ra_kint_base_addr;
}

/**
 * @enum ra_krctl_bit_t
 * @brief KRCTL bit positions.
 */
typedef enum : uint8_t {
  k_ra_krctl_bit_kreg = 0U, /**< 0=falling edge, 1=rising edge. */
  k_ra_krctl_bit_kre  = 7U, /**< Key interrupt enable.          */
} ra_krctl_bit_t;

/**
 * @enum ra_kint_row_t
 * @brief Key-matrix row indices (0..7).
 */
typedef enum : uint8_t {
  k_ra_kint_row_max = 8U, /**< KRF / KRM / KRSTR are 8-bit. */
} ra_kint_row_t;

#ifdef __cplusplus
}
#endif
