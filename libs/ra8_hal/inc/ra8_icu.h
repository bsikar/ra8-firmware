/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_icu.h
 * @brief Interrupt Control Unit driver (IRQ pins, NMI)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The low-level IELSR event allocator moved to ``ra8_isr``;
 * ``ra8_icu`` now owns the remaining ICU functionality:
 *
 * - ``ra8_icu_init()`` resets IRQCRa[0..15] + IRQCRb[0..15] (32 channels
 * total on RA8D2, cross-checked against FSP R_ICU_Type), disables
 * every NMI source, clears pending NMI status flags, and clears
 * WUPEN0/WUPEN1 so no source can wake the core unexpectedly.
 * - ``ra8_icu_configure_irq_pin(num, cfg)`` programs one of the
 * 32 external IRQ channels (IRQCRi): edge sensitivity, digital
 * filter clock divider, filter enable.
 * - ``ra8_icu_nmi_enable(sources)`` / ``ra8_icu_nmi_disable`` /
 * ``ra8_icu_nmi_clear`` manage the NMI sources mapped through
 * NMIER / NMICR / NMICLR (HUM Ch 14 p 524..582).
 *
 * All register writes go through ``ra8_icu_irqcr`` / ``ra8_icu_nmier``
 * accessors from ``ra8_icu_regs.h`` (HUM Ch 14 p 524).
 *
 * ## Threading
 *
 * Single-threaded init context only.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_icu_regs.h"

/* =============================================================================
 * IRQ pin configuration
 * =============================================================================
 */

/**
 * @struct ra8_icu_irq_cfg_t
 * @brief One external-IRQ-pin configuration descriptor.
 *
 * @details
 * Populate one of these and hand it to
 * ``ra8_icu_configure_irq_pin()``. Fields map directly onto
 * IRQCRi bit fields per HUM Ch 14.2.12 (p 535).
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_icu_configure_irq_pin`` in
 * ``libs/ra8_hal/src/ra8_icu.c``.
 */
typedef struct {
  ra8_icu_irqmd_t   sense;      /**< Edge / level detection mode.   */
  ra8_icu_fclksel_t filter_div; /**< Filter sampling clock divider. */
  bool              filter_en;  /**< True = enable digital filter.  */
} ra8_icu_irq_cfg_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Reset every ICU-owned register to its post-reset state.
 *
 * @details
 * Writes 0 to IRQCRa[0..15] and IRQCRb[0..15] (all 32 RA8D2 IRQ
 * channels), NMIER, and WUPEN0/WUPEN1, then clears every NMISR
 * status bit via NMICLR.  Does NOT touch IELSR -- those slots
 * belong to ``ra8_isr``.  Call this from early init before any
 * driver registers an IRQ pin.
 *
 * @return ``k_ra8_ok``.
 *
 * @pre Caller holds single-threaded init context.
 * @post IRQCRa[0..15] and IRQCRb[0..15] all read as 0.
 * @post NMIER == 0.
 * @post NMISR == 0 (all status bits cleared).
 * @post WUPEN0 == 0 and WUPEN1 == 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_init(void);

/* =============================================================================
 * External IRQ pin programming
 * =============================================================================
 */

/**
 * @brief Programme one IRQCRi register.
 *
 * @details
 * Writes the edge-sensitivity, filter-divider, and filter-enable
 * fields per HUM 14.2.12 (p 535). Reserved bits in IRQCRi are
 * always written 0 (the HUM says "should be 0" for the read-zero
 * bits). The driver does NOT touch IELSR or enable the NVIC
 * line -- that is the ``ra8_isr_register`` responsibility.
 *
 * @param[in] irq_num IRQ channel 0..31 (IRQCRa for 0..15, IRQCRb for 16..31).
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Register programmed.
 * @retval k_ra8_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra8_err_null_ptr ``cfg`` was NULL.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post IRQCR[irq_num] reflects the requested config.
 *
 * @note Per HUM 14.2.12, IRQCRi may only be rewritten while the
 * matching IELSRn register is 0 (no event routed) -- the
 * caller must order the init sequence accordingly.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_configure_irq_pin(uint8_t irq_num, const ra8_icu_irq_cfg_t* cfg);

/**
 * @brief Read the raw 8-bit IRQCRi value.
 *
 * @param[in] irq_num Pin 0..31.
 * @param[out] out_val On success, the current register value.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_invalid_arg`` on
 * out-of-range pin, or ``k_ra8_err_null_ptr``.
 *
 * @pre ``out_val`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_read_irqcr(uint8_t irq_num, uint8_t* out_val);

/* =============================================================================
 * NMI handling
 * =============================================================================
 */

/**
 * @brief Enable one or more NMI sources by OR'ing into NMIER.
 *
 * @details
 * The NMIER layout is source-specific; pass a mask of the bits
 * that correspond to the sources the caller wants enabled. HUM
 * 14.2.14 (p 542) lists the bit assignments.
 *
 * @param[in] mask Bit mask to OR into NMIER.
 *
 * @return ``k_ra8_ok``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NMIER has the requested bits set.
 *
 * @note NMIER bits are sticky-set -- to disable, rewrite the
 * register via ``ra8_icu_nmi_disable``.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_nmi_enable(uint32_t mask);

/**
 * @brief Disable NMI sources by clearing bits in NMIER.
 *
 * @param[in] mask Bit mask to clear in NMIER (21 bits, FSP R_ICU_NMIER).
 *
 * @return ``k_ra8_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @post NMIER has the requested bits clear.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_nmi_disable(uint32_t mask);

/**
 * @brief Clear pending NMI status flags via NMICLR (HUM 14.2.15 p 544).
 *
 * @param[in] mask Bit mask of NMICLR bits to set (write-1-to-clear).
 *
 * @return ``k_ra8_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @post The targeted NMISR bits read as 0.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_nmi_clear(uint32_t mask);

/**
 * @brief Read the current NMISR value.
 *
 * @param[out] out_status NMISR value on success (32-bit).
 *
 * @return ``k_ra8_ok`` or ``k_ra8_err_null_ptr``.
 * @pre ``out_status`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_icu_nmi_status(uint32_t* out_status);

#ifdef __cplusplus
}
#endif
