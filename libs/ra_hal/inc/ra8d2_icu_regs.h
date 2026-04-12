/**
 * @file ra8d2_icu_regs.h
 * @brief Interrupt Control Unit (ICU) register layout for the Renesas RA8D2
 *
 * @details
 * The RA Interrupt Control Unit sits between the peripheral event
 * sources and the two Cortex-M NVICs (one per core). Unlike most
 * Cortex-M chips, peripheral interrupt numbers on RA are
 * *programmable*: you tell the ICU "please route event X to NVIC
 * line Y", then enable line Y in the NVIC as usual. The event-to-line
 * map lives in the IELSR registers below.
 *
 * ## Register map (partial)
 *
 * | Offset | Name      | Width | Purpose                              |
 * |-------:|-----------|------:|--------------------------------------|
 * | 0x000  | IRQCR0-15 | 8     | External IRQ input digital filter    |
 * | 0x100  | NMICR     | 8     | NMI pin control                       |
 * | 0x300  | IELSR0..n | 32    | Event Link Setting Registers         |
 * | 0x1000 | IELSRN0..m| 32    | (secure copy / non-secure copies)    |
 *
 * ## IELSR usage
 *
 * @code{.c}
 *   // Route SCI0 RXI (event 0x050 from FSP bsp_elc.h) to NVIC line 3.
 *   ra_icu()->IELSR[3] = 0x050UL;
 *   NVIC_EnableIRQ(3);
 * @endcode
 *
 * @note The Cortex-M85 core has 480 NVIC interrupts, but only the
 *       first N are wired to the ICU -- N depends on MCU variant.
 *       The Cortex-M33 on RA8D2 sees its own disjoint set.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra_icu_base_addr = 0x40006000UL, /**< R_ICU block base. */
} ra_icu_addr_t;

typedef enum : uint16_t {
  k_ra_icu_num_ielsr = 112U, /**< IELSR slot count on RA8D2 (HUM section 13). */
  k_ra_icu_num_irqs  = 16U,  /**< IRQ0..IRQ15 external IRQ pins.              */
} ra_icu_limits_t;

/* =============================================================================
 * Register offsets
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra_icu_off_irqcr0 = 0x000U, /**< IRQCR0..15 (8 bits each). */
  k_ra_icu_off_nmier  = 0x120U, /**< NMIER: NMI enable register. */
  k_ra_icu_off_ielsr0 = 0x300U, /**< IELSR0..N (32 bits each). */
} ra_icu_off_t;

/* =============================================================================
 * Accessors
 * =============================================================================
 */

/** @brief Get pointer to IELSR register N. */
static inline volatile uint32_t* ra_icu_ielsr(uint16_t index)
{
  if (index >= k_ra_icu_num_ielsr) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_icu_base_addr + k_ra_icu_off_ielsr0 +
                              ((uintptr_t)index * sizeof(uint32_t)));
}

/** @brief Get pointer to one of the IRQCR registers (0..15). */
static inline volatile uint8_t* ra_icu_irqcr(uint8_t irq_num)
{
  if (irq_num >= (uint8_t)k_ra_icu_num_irqs) {
    return nullptr;
  }
  return (volatile uint8_t*)(k_ra_icu_base_addr + k_ra_icu_off_irqcr0 + (uintptr_t)irq_num);
}

/* =============================================================================
 * IELSR bit layout
 * =============================================================================
 */

/**
 * @enum ra_ielsr_bit_t
 * @brief Bit positions in an IELSR register.
 */
typedef enum : uint8_t {
  k_ra_ielsr_iels_shift = 0U,  /**< Event select (ELC event number) [8:0]. */
  k_ra_ielsr_ds_bit     = 16U, /**< DTC start (pseudo-field).              */
  k_ra_ielsr_dtce_bit   = 24U, /**< DTC enable on event.                   */
  k_ra_ielsr_ir_bit     = 16U, /**< Interrupt status flag (RW1C).          */
} ra_ielsr_bit_t;

typedef enum : uint32_t {
  k_ra_ielsr_iels_mask = 0x000001FFUL, /**< 9 bits. */
} ra_ielsr_mask_t;

#ifdef __cplusplus
}
#endif
