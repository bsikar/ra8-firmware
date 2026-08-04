/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_icu_regs.h
 * @brief Interrupt Control Unit (ICU) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
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
 *   ra8_icu()->IELSR[3] = 0x050UL;
 *   NVIC_EnableIRQ(3);
 * @endcode
 *
 * @note The Cortex-M85 core has 480 NVIC interrupts, but only the
 *       first N are wired to the ICU -- N depends on MCU variant.
 *       The Cortex-M33 on RA8D2 sees its own disjoint set.
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
  k_ra8_icu_base_addr = 0x40006000UL, /**< R_ICU block base. */
} ra8_icu_addr_t;

typedef enum : uint16_t {
  k_ra8_icu_num_ielsr     = 96U, /**< IELSR slot count on RA8D2 (FSP R_ICU_Type). */
  k_ra8_icu_num_irqs      = 32U, /**< IRQ0..IRQ31, RA8D2 has 32 IRQ channels.     */
  k_ra8_icu_irqcra_count  = 16U, /**< IRQCRa[16] holds channels 0..15.            */
  k_ra8_icu_irqcrb_first  = 16U, /**< First IRQ channel served by IRQCRb.         */
  k_ra8_icu_irqcrb_offset = 4U,  /**< Byte gap between IRQCRa[15] and IRQCRb[0].  */
} ra8_icu_limits_t;

/* =============================================================================
 * Register offsets (verified against FSP R_ICU_Type in
 * R7KA8D2KF_core0.h lines 8670-9061)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra8_icu_off_irqcra0 = 0x0000U, /**< IRQCRa[0..15] (8 bits each), ch 0..15.  */
  k_ra8_icu_off_nmicr   = 0x0010U, /**< NMICR: NMI pin control (8b).            */
  k_ra8_icu_off_irqcrb0 = 0x0014U, /**< IRQCRb[0..15] (8 bits each), ch 16..31. */
  k_ra8_icu_off_intselr = 0x0040U, /**< INTSELR[32] (32 bits each): IRQ core.   */
  k_ra8_icu_off_nmier   = 0x6100U, /**< NMIER: NMI enable (32b).                */
  k_ra8_icu_off_nmiclr  = 0x6110U, /**< NMICLR: NMI status clear (32b).         */
  k_ra8_icu_off_nmisr   = 0x6120U, /**< NMISR: NMI status (32b).                */
  k_ra8_icu_off_wupen0  = 0x61A0U, /**< WUPEN0: wake-up enable register 0.      */
  k_ra8_icu_off_wupen1  = 0x61A4U, /**< WUPEN1: wake-up enable register 1.      */
  k_ra8_icu_off_dslpwup = 0x6210U, /**< DSLPWUPIRQEN[3]: deep-sleep IRQ enable. */
  k_ra8_icu_off_delsr0  = 0x6280U, /**< DELSR[8]: DMAC event link select.       */
  k_ra8_icu_off_ielsr0  = 0x6300U, /**< IELSR[0..95] (32 bits each).            */
} ra8_icu_off_t;

/**
 * @enum ra8_icu_irqmd_t
 * @brief IRQCRi.IRQMD[1:0] detection sense values (HUM 14.2.12 p 535).
 */
typedef enum : uint8_t {
  k_ra8_icu_irqmd_falling = 0U, /**< 00: falling edge.        */
  k_ra8_icu_irqmd_rising  = 1U, /**< 01: rising edge.         */
  k_ra8_icu_irqmd_both    = 2U, /**< 10: rising OR falling.   */
  k_ra8_icu_irqmd_low     = 3U, /**< 11: low-level sensitive. */
} ra8_icu_irqmd_t;

/**
 * @enum ra8_icu_fclksel_t
 * @brief IRQCRi.FCLKSEL[1:0] digital filter sampling clock (HUM 14.2.12 p 535).
 */
typedef enum : uint8_t {
  k_ra8_icu_fclksel_pclkb    = 0U, /**< 00: sample every PCLKB cycle.  */
  k_ra8_icu_fclksel_pclkb_8  = 1U, /**< 01: sample once per 8 cycles.  */
  k_ra8_icu_fclksel_pclkb_32 = 2U, /**< 10: sample once per 32 cycles. */
  k_ra8_icu_fclksel_pclkb_64 = 3U, /**< 11: sample once per 64 cycles. */
} ra8_icu_fclksel_t;

/**
 * @enum ra8_icu_irqcr_bit_t
 * @brief IRQCRi bit positions.
 */
typedef enum : uint8_t {
  k_ra8_icu_irqcr_bit_fclksel = 4U, /**< FCLKSEL[1:0] at bits 5..4. */
  k_ra8_icu_irqcr_bit_flten   = 7U, /**< FLTEN at bit 7.            */
} ra8_icu_irqcr_bit_t;

/**
 * @enum ra8_icu_irqcr_mask_t
 * @brief IRQCRi bit masks.
 */
typedef enum : uint8_t {
  k_ra8_icu_irqcr_mask_irqmd   = 0x03U, /**< IRQMD[1:0] bits 1..0.   */
  k_ra8_icu_irqcr_mask_fclksel = 0x30U, /**< FCLKSEL[1:0] bits 5..4. */
  k_ra8_icu_irqcr_mask_flten   = 0x80U, /**< FLTEN bit 7.            */
} ra8_icu_irqcr_mask_t;

/** @brief Get pointer to the 32-bit NMIER register. */
static inline volatile uint32_t* ra8_icu_nmier(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_nmier);
}

/** @brief Get pointer to the 32-bit NMISR register. */
static inline volatile uint32_t* ra8_icu_nmisr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_nmisr);
}

/** @brief Get pointer to the 32-bit NMICLR register. */
static inline volatile uint32_t* ra8_icu_nmiclr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_nmiclr);
}

/** @brief Get pointer to the 8-bit NMICR register. */
static inline volatile uint8_t* ra8_icu_nmicr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_nmicr);
}

/**
 * @brief Get pointer to the 32-bit WUPEN0 register.
 *
 * @details
 * WUPEN0 holds 16 IRQWUPENn bits (channels 0..15) at [15:0] and 16
 * peripheral WUPENn bits at [31:16].  HUM Ch 14 "WUPEN : Wake Up
 * Interrupt Enable Register 0", p 524..582.
 */
static inline volatile uint32_t* ra8_icu_wupen0(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_wupen0);
}

/**
 * @brief Get pointer to the 32-bit WUPEN1 register.
 *
 * @details
 * WUPEN1 holds 16 peripheral WUPEN16..31 bits at [15:0] and 16
 * IRQWUPEN16..31 bits at [31:16].  HUM Ch 14 "WUPEN : Wake Up
 * Interrupt Enable Register 1", p 524..582.
 */
static inline volatile uint32_t* ra8_icu_wupen1(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_wupen1);
}

/**
 * @brief Get pointer to DELSR[index], the DMAC Event Link Setting Register.
 *
 * @details
 * DELSR has 8 entries, each a 32-bit register with DELS at [9:0] and
 * IR at [16].  HUM Ch 14 "DELSRn : DMAC Event Link Setting Register
 * n", p 524..582.
 *
 * @param[in] index DELSR slot 0..7.
 * @return Pointer to the slot, or ``nullptr`` on out-of-range.
 */
static inline volatile uint32_t* ra8_icu_delsr(uint8_t index)
{
  enum ra8_icu_delsr_bound : uint8_t {
    k_ra8_icu_num_delsr = 8U, /**< RA8 icu number delsr. */
  };
  if (index >= k_ra8_icu_num_delsr) {
    return nullptr;
  }
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_delsr0 +
                              ((uintptr_t)index * sizeof(uint32_t)));
}

/* =============================================================================
 * Accessors
 * =============================================================================
 */

/** @brief Get pointer to IELSR register N. */
static inline volatile uint32_t* ra8_icu_ielsr(uint16_t index)
{
  if (index >= k_ra8_icu_num_ielsr) {
    return nullptr;
  }
  return (volatile uint32_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_ielsr0 +
                              ((uintptr_t)index * sizeof(uint32_t)));
}

/**
 * @brief Get pointer to the IRQCRi register for IRQ channel ``irq_num``.
 *
 * @details
 * RA8D2 has 32 external IRQ channels split across two 16-deep arrays:
 *   - IRQCRa[0..15] at offsets 0x00..0x0F (channels 0..15).
 *   - IRQCRb[0..15] at offsets 0x14..0x23 (channels 16..31), separated
 *     from IRQCRa by NMICR (1 B) + 3 reserved bytes -- a 4-byte gap.
 *
 * Mirrors the FSP ``ICU_IRQCR_CH(c)`` macro: for c in 16..31 the byte
 * offset is ``c + 4`` from IRQCRa[0] which lands inside IRQCRb after
 * the 4-byte NMICR/reserved gap.
 *
 * HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535
 *
 * @param[in] irq_num IRQ channel 0..31.
 * @return Volatile pointer to the IRQCR byte, or ``nullptr`` if out of range.
 */
static inline volatile uint8_t* ra8_icu_irqcr(uint8_t irq_num)
{
  if (irq_num >= (uint8_t)k_ra8_icu_num_irqs) {
    return nullptr;
  }
  /* HUM Ch 14.2.12 "IRQCRi : IRQ Control Register", p 535: IRQCRa
   * occupies bytes 0..15, NMICR + reserved fill 16..19, IRQCRb starts
   * at 20.  Match FSP ICU_IRQCR_CH(c) = c + ((c >> 4) << 2). */
  const uintptr_t gap_bytes = ((uintptr_t)irq_num >= (uintptr_t)k_ra8_icu_irqcrb_first)
                                ? (uintptr_t)k_ra8_icu_irqcrb_offset
                                : (uintptr_t)0U;
  return (volatile uint8_t*)((uintptr_t)k_ra8_icu_base_addr + (uintptr_t)k_ra8_icu_off_irqcra0 +
                             (uintptr_t)irq_num + gap_bytes);
}

/* =============================================================================
 * IELSR bit layout
 * =============================================================================
 */

/**
 * @enum ra8_ielsr_bit_t
 * @brief Bit positions in an IELSR register.
 *
 * @details
 * Cross-checked against FSP ``R_ICU_Type::IELSR_b[96]`` in
 * ``R7KA8D2KF_core0.h``: ``IELS`` is a 10-bit field at [9:0],
 * ``IR`` (interrupt status flag, RW1C) at [16], ``DTCE`` at [24].
 * HUM Ch 14.2.17 "IELSRn : ICU Event Link Setting Register n",
 * p 524.
 */
typedef enum : uint8_t {
  k_ra8_ielsr_iels_shift = 0U,  /**< IELS event-select shift, bits [9:0]. */
  k_ra8_ielsr_ir_bit     = 16U, /**< IR interrupt status flag (W0C).      */
  k_ra8_ielsr_dtce_bit   = 24U, /**< DTCE: DTC activation enable.         */
} ra8_ielsr_bit_t;

typedef enum : uint32_t {
  /* HUM Ch 14.2.17 "IELSRn : ICU Event Link Setting Register n", p 547:
   * IELS occupies bits [9:0] -- 10-bit field, mask 0x3FF.  RA8 has
   * hundreds of ELC event sources so the FSP layout uses 10 bits. */
  k_ra8_ielsr_iels_mask = 0x000003FFUL, /**< IELS field mask (10 bits). */
  k_ra8_ielsr_ir_mask   = 0x00010000UL, /**< IR bit mask (RW1C).        */
  k_ra8_ielsr_dtce_mask = 0x01000000UL, /**< DTCE bit mask.             */
} ra8_ielsr_mask_t;

#ifdef __cplusplus
}
#endif
