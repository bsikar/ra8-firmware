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
  k_ra_icu_num_ielsr = 96U, /**< IELSR slot count on RA8D2 (FSP R_ICU_Type). */
  k_ra_icu_num_irqs  = 16U, /**< IRQ0..IRQ15 external IRQ pins.              */
} ra_icu_limits_t;

/* =============================================================================
 * Register offsets (verified against FSP R_ICU_Type in
 * R7KA8D2KF_core0.h lines 8670-9061)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_icu_off_irqcra0 = 0x0000U, /**< IRQCRa[0..15] (8 bits each) core0.    */
  k_ra_icu_off_nmicr   = 0x0010U, /**< NMICR: NMI pin control (8b).          */
  k_ra_icu_off_irqcrb0 = 0x0014U, /**< IRQCRb[0..15] (8 bits each) core1.    */
  k_ra_icu_off_intselr = 0x0040U, /**< INTSELR[32] (32 bits each): IRQ core. */
  k_ra_icu_off_nmier   = 0x6100U, /**< NMIER: NMI enable (32b).              */
  k_ra_icu_off_nmiclr  = 0x6110U, /**< NMICLR: NMI status clear (32b).       */
  k_ra_icu_off_nmisr   = 0x6120U, /**< NMISR: NMI status (32b).              */
  k_ra_icu_off_ielsr0  = 0x6300U, /**< IELSR[0..95] (32 bits each).          */
} ra_icu_off_t;

/**
 * @enum ra_icu_irqmd_t
 * @brief IRQCRi.IRQMD[1:0] detection sense values (HUM 14.2.12 p 535).
 */
typedef enum : uint8_t {
  k_ra_icu_irqmd_falling = 0U, /**< 00: falling edge.        */
  k_ra_icu_irqmd_rising  = 1U, /**< 01: rising edge.         */
  k_ra_icu_irqmd_both    = 2U, /**< 10: rising OR falling.   */
  k_ra_icu_irqmd_low     = 3U, /**< 11: low-level sensitive. */
} ra_icu_irqmd_t;

/**
 * @enum ra_icu_fclksel_t
 * @brief IRQCRi.FCLKSEL[1:0] digital filter sampling clock (HUM 14.2.12 p 535).
 */
typedef enum : uint8_t {
  k_ra_icu_fclksel_pclkb    = 0U, /**< 00: sample every PCLKB cycle.   */
  k_ra_icu_fclksel_pclkb_8  = 1U, /**< 01: sample once per 8 cycles.    */
  k_ra_icu_fclksel_pclkb_32 = 2U, /**< 10: sample once per 32 cycles.   */
  k_ra_icu_fclksel_pclkb_64 = 3U, /**< 11: sample once per 64 cycles.   */
} ra_icu_fclksel_t;

/**
 * @enum ra_icu_irqcr_bit_t
 * @brief IRQCRi bit positions.
 */
typedef enum : uint8_t {
  k_ra_icu_irqcr_bit_fclksel = 4U, /**< FCLKSEL[1:0] at bits 5..4. */
  k_ra_icu_irqcr_bit_flten   = 7U, /**< FLTEN at bit 7.             */
} ra_icu_irqcr_bit_t;

/**
 * @enum ra_icu_irqcr_mask_t
 * @brief IRQCRi bit masks.
 */
typedef enum : uint8_t {
  k_ra_icu_irqcr_mask_irqmd   = 0x03U, /**< IRQMD[1:0] bits 1..0. */
  k_ra_icu_irqcr_mask_fclksel = 0x30U, /**< FCLKSEL[1:0] bits 5..4. */
  k_ra_icu_irqcr_mask_flten   = 0x80U, /**< FLTEN bit 7. */
} ra_icu_irqcr_mask_t;

/** @brief Get pointer to the 32-bit NMIER register. */
static inline volatile uint32_t* ra_icu_nmier(void)
{
  return (volatile uint32_t*)(k_ra_icu_base_addr + k_ra_icu_off_nmier);
}

/** @brief Get pointer to the 32-bit NMISR register. */
static inline volatile uint32_t* ra_icu_nmisr(void)
{
  return (volatile uint32_t*)(k_ra_icu_base_addr + k_ra_icu_off_nmisr);
}

/** @brief Get pointer to the 32-bit NMICLR register. */
static inline volatile uint32_t* ra_icu_nmiclr(void)
{
  return (volatile uint32_t*)(k_ra_icu_base_addr + k_ra_icu_off_nmiclr);
}

/** @brief Get pointer to the 8-bit NMICR register. */
static inline volatile uint8_t* ra_icu_nmicr(void)
{
  return (volatile uint8_t*)(k_ra_icu_base_addr + k_ra_icu_off_nmicr);
}

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
  return (volatile uint8_t*)(k_ra_icu_base_addr + k_ra_icu_off_irqcra0 + (uintptr_t)irq_num);
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
