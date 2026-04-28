/**
 * @file ra_icu.h
 * @brief Interrupt Control Unit driver (IRQ pins, NMI, legacy event routing)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * rewrite. The low-level IELSR event allocator moved
 * to ``ra_isr``; ``ra_icu`` now owns the remaining
 * ICU functionality:
 *
 * - ``ra_icu_init()`` resets IRQCR0..15, disables every NMI
 * source, clears any pending NMI status flags.
 * - ``ra_icu_configure_irq_pin(num, cfg)`` programs one of the
 * 16 external IRQ pins (IRQCRi): edge sensitivity, digital
 * filter clock divider, filter enable.
 * - ``ra_icu_nmi_enable(sources)`` / ``ra_icu_nmi_disable`` /
 * ``ra_icu_nmi_clear`` manage the NMI sources mapped through
 * NMIER / NMICR / NMICLR (HUM Ch 14.2.14..15).
 * - The ``ra_icu_route`` / ``ra_icu_nvic_*`` legacy functions
 * remain as thin wrappers around direct register access for
 * backward compatibility with pre-Wave-2 demo main.c code.
 * They are documented as @deprecated.
 *
 * All register writes go through ``ra_icu_irqcr`` / ``ra_icu_nmier``
 * accessors from ``ra8d2_icu_regs.h`` (HUM Ch 14 p 524).
 *
 * ## Threading
 *
 * Single-threaded init context only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_icu_regs.h"
#include "ra_err.h"

/* =============================================================================
 * IRQ pin configuration
 * =============================================================================
 */

/**
 * @struct ra_icu_irq_cfg_t
 * @brief One external-IRQ-pin configuration descriptor.
 *
 * @details
 * Populate one of these and hand it to
 * ``ra_icu_configure_irq_pin()``. Fields map directly onto
 * IRQCRi bit fields per HUM Ch 14.2.12 (p 535).
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_icu_configure_irq_pin`` in
 * ``libs/ra_hal/src/ra_icu.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_icu_irqmd_t   sense;      /**< Edge / level detection mode. */
  ra_icu_fclksel_t filter_div; /**< Filter sampling clock divider. */
  bool             filter_en;  /**< True = enable digital filter. */
} ra_icu_irq_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Reset every ICU-owned register to its post-reset state.
 *
 * @details
 * Writes 0 to IRQCR0..15, NMIER, NMICR and clears every NMISR
 * status bit via NMICLR. Does NOT touch IELSR -- those slots
 * belong to ``ra_isr``. Call this from early init before any
 * driver registers an IRQ pin.
 *
 * @return ``k_ra_ok``.
 *
 * @pre Caller holds single-threaded init context.
 * @post IRQCR0..15 == 0.
 * @post NMIER == 0.
 * @post NMISR == 0 (all status bits cleared).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_init(void);

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
 * line -- that is the ``ra_isr_register`` responsibility.
 *
 * @param[in] irq_num IRQ pin number 0..15.
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Register programmed.
 * @retval k_ra_err_invalid_arg ``irq_num`` out of range.
 * @retval k_ra_err_null_ptr ``cfg`` was NULL.
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
[[nodiscard]] ra_err_t ra_icu_configure_irq_pin(uint8_t irq_num, const ra_icu_irq_cfg_t* cfg);

/**
 * @brief Read the raw 8-bit IRQCRi value.
 *
 * @param[in] irq_num Pin 0..15.
 * @param[out] out_val On success, the current register value.
 *
 * @return ``k_ra_ok`` on success, ``k_ra_err_invalid_arg`` on
 * out-of-range pin, or ``k_ra_err_null_ptr``.
 *
 * @pre ``out_val`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_read_irqcr(uint8_t irq_num, uint8_t* out_val);

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
 * @return ``k_ra_ok``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post NMIER has the requested bits set.
 *
 * @note NMIER bits are sticky-set -- to disable, rewrite the
 * register via ``ra_icu_nmi_disable``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_nmi_enable(uint32_t mask);

/**
 * @brief Disable NMI sources by clearing bits in NMIER.
 *
 * @param[in] mask Bit mask to clear in NMIER (21 bits, FSP R_ICU_NMIER).
 *
 * @return ``k_ra_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @post NMIER has the requested bits clear.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_nmi_disable(uint32_t mask);

/**
 * @brief Clear pending NMI status flags via NMICLR (HUM 14.2.15 p 544).
 *
 * @param[in] mask Bit mask of NMICLR bits to set (write-1-to-clear).
 *
 * @return ``k_ra_ok``.
 * @pre IRQs masked or single-threaded init context.
 * @post The targeted NMISR bits read as 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_nmi_clear(uint32_t mask);

/**
 * @brief Read the current NMISR value.
 *
 * @param[out] out_status NMISR value on success (32-bit).
 *
 * @return ``k_ra_ok`` or ``k_ra_err_null_ptr``.
 * @pre ``out_status`` is non-NULL.
 * @post No hardware state is modified.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_nmi_status(uint32_t* out_status);

/* =============================================================================
 * Legacy API -- direct NVIC + IELSR access (deprecated)
 * =============================================================================
 */

/**
 * @brief Route an ELC event to a caller-specified IELSR slot.
 *
 * @deprecated New code should call ``ra_isr_register()`` instead.
 * Preserved so the pre-Wave-2 demo main and the existing
 * test_ra_icu.c suite keep compiling.
 *
 * @param[in] nvic_index IELSR slot index 0..111.
 * @param[in] event ELC event number.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Slot programmed.
 * @retval k_ra_err_out_of_range ``nvic_index`` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_icu_route(uint16_t nvic_index, ra_elc_event_t event);

/**
 * @brief Enable NVIC line N directly (legacy).
 * @deprecated Prefer ``ra_isr_register`` which also enables.
 */
void ra_icu_nvic_enable(uint16_t nvic_index);

/**
 * @brief Disable NVIC line N directly (legacy).
 * @deprecated Prefer ``ra_isr_unregister`` which also disables.
 */
void ra_icu_nvic_disable(uint16_t nvic_index);

/**
 * @brief Set the NVIC priority byte for a line directly (legacy).
 * @deprecated Prefer ``ra_isr_set_priority``.
 */
void ra_icu_nvic_set_priority(uint16_t nvic_index, uint8_t priority);

#ifdef __cplusplus
}
#endif
