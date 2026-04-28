/**
 * @file ra8d2_elc_regs.h
 * @brief Event Link Controller (ELC) register layout for the Renesas RA8D2
 *
 * @details
 * The ELC lets one peripheral's event directly trigger another
 * peripheral's input without CPU involvement. The RA8D2 has 500+
 * defined ELC events (see the full enumeration list below) which
 * are identical to those in FSP's `bsp_elc.h`.
 *
 * Register map (at `0x40201000`):
 *
 * | Offset | Name    | Width | Purpose                              |
 * |-------:|---------|------:|--------------------------------------|
 * | 0x00   | ELCR    | 8     | Event Link Control Register          |
 * | 0x02   | ELSEGR0 | 8     | Event Link Software Event Gen 0      |
 * | 0x04   | ELSEGR1 | 8     | Event Link Software Event Gen 1      |
 * | 0x10   | ELSRn   | 16    | Event Link Setting Register n (n = 0..22) |
 *
 * @note This header exposes the base + offsets only. The full 500+
 *       event enum is in the RA8D2 HUM section 18 and the FSP
 *       `bsp_elc.h`. Copy in the entries we actually wire up rather
 *       than the whole list.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_elc_base_addr = 0x40201000UL,
} ra_elc_addr_t;

typedef enum : uint16_t {
  k_ra_elc_off_elcr      = 0x000U, /**< ELCR: global enable (8b).          */
  k_ra_elc_off_elsegr0   = 0x004U, /**< ELSEGR0 (8b, stride 4).            */
  k_ra_elc_off_elsegr1   = 0x008U, /**< ELSEGR1 (8b, stride 4).            */
  k_ra_elc_off_elsegr2   = 0x00CU, /**< ELSEGR2 (8b, stride 4).            */
  k_ra_elc_off_elsegr3   = 0x010U, /**< ELSEGR3 (8b, stride 4).            */
  k_ra_elc_off_elsr0     = 0x020U, /**< ELSR0..52 (16b, stride 4).         */
  k_ra_elc_off_elcsara   = 0x100U, /**< ELCSARA: security attribution A.   */
  k_ra_elc_elsr_stride   = 0x004U, /**< Stride between ELSR slots.         */
  k_ra_elc_elsegr_stride = 0x004U, /**< Stride between ELSEGR slots.       */
} ra_elc_off_t;

/**
 * @enum ra_elc_event_t
 * @brief Partial list of ELC events (populate as drivers need them).
 *
 * @details
 * Values taken from the RA8D2 Hardware User's Manual section 18.
 * Only the entries the firmware actually wires up live here; see the
 * manual (or `fsp/ra/fsp/src/bsp/mcu/ra8d2/bsp_elc.h`) for the full
 * list.
 */
typedef enum : uint16_t {
  k_ra_elc_event_none          = 0x000U,
  k_ra_elc_event_icu_irq0      = 0x001U,
  k_ra_elc_event_icu_irq1      = 0x002U,
  k_ra_elc_event_icu_irq15     = 0x010U,
  k_ra_elc_event_can0_mram_eri = 0x100U,
  k_ra_elc_event_can1_mram_eri = 0x101U,
} ra_elc_event_t;

#ifdef __cplusplus
}
#endif
