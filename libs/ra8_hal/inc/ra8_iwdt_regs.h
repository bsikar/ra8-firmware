/**
 * @file ra8_iwdt_regs.h
 * @brief Independent Watchdog Timer (IWDT) register layout for the RA8D2
 * @ingroup grp_hal_timers
 *
 * @details
 * The IWDT is a free-running 14-bit down-counter clocked from the
 * IWDT-dedicated low-speed oscillator (IWDTCLK, ~15 kHz on RA8D2),
 * completely independent of the CPU clock tree -- so it survives
 * MOCO / HOCO failure. Once started it cannot be stopped by software;
 * the only way to prevent a reset is to refresh it before the counter
 * reaches zero by writing the two-byte sequence ``0x00`` then ``0xFF``
 * to ``IWDTRR``.
 *
 * Most of the IWDT configuration (period, window, reset-vs-NMI,
 * sleep-stop, clock divider) lives in the ``OFS0`` option-setting
 * register, which is read out of MRAM at boot. These fields are NOT
 * runtime-configurable like WDT in register-start mode -- the IWDT
 * has only auto-start mode on this part. The ``IWDTCR`` / ``IWDTRCR``
 * / ``IWDTCSTPR`` registers exist in the memory map (and FSP exposes
 * them) but they are write-locked once OFS0 has booted the timer.
 *
 * | Offset | Reg       | Width | RW | Purpose                       |
 * |-------:|-----------|------:|----|-------------------------------|
 * | 0x00   | IWDTRR    |     8 | RW | Refresh register              |
 * | 0x01   | (rsv)     |     8 |  - | Reserved                      |
 * | 0x02   | IWDTCR    |    16 | RW | Control (TOPS/CKS/RPES/RPSS)  |
 * | 0x04   | IWDTSR    |    16 | RW | Status (CNTVAL+UNDFF+REFEF)   |
 * | 0x06   | IWDTRCR   |     8 | RW | Reset Control (RSTIRQS)       |
 * | 0x07   | (rsv)     |     8 |  - | Reserved                      |
 * | 0x08   | IWDTCSTPR |     8 | RW | Count-Stop Control (SLCSTP)   |
 * | 0x09   | (rsv)     |     8 |  - | Reserved                      |
 * | 0x0A   | (rsv)     |    16 |  - | Reserved (pads to 0x0C)       |
 *
 * Block size = 12 (0x0C). Layout matches FSP ``R_IWDT_Type`` in
 * ``R7KA8D2KF_core0.h`` exactly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_iwdt_addr_t
 * @brief Base address of the RA8D2 IWDT instance.
 *
 * @details
 * The RA8D2 carries a single IWDT (no per-core duplicate the way WDT0 /
 * WDT1 are split across M85 / M33). The base address comes from FSP
 * ``R7KA8D2KF_core0.h`` ``R_IWDT_BASE = 0x40202200``.
 *
 * @see HUM Ch 28 "Independent Watchdog Timer (IWDT)" p 1271.
 */
typedef enum : uintptr_t {
  k_ra8_iwdt_base_addr = 0x40202200UL, /**< IWDT register block base. */
} ra8_iwdt_addr_t;

/**
 * @enum ra8_iwdt_refresh_t
 * @brief Two-byte unlock pattern written to ``IWDTRR`` to refresh.
 *
 * @details
 * Per HUM Ch 28.2.1 "IWDTRR : IWDT Refresh Register" p 1273, the only
 * legal way to reload the down-counter is to write ``0x00`` followed
 * by ``0xFF``; any other sequence is silently dropped. A SINGLE write
 * does not refresh -- the FSP ``r_iwdt_refresh`` and our
 * ``ra8_iwdt_refresh`` both perform both writes.
 */
typedef enum : uint8_t {
  k_ra8_iwdt_refresh_a = 0x00U, /**< First byte of the refresh sequence.  */
  k_ra8_iwdt_refresh_b = 0xFFU, /**< Second byte of the refresh sequence. */
} ra8_iwdt_refresh_t;

/**
 * @enum ra8_iwdt_iwdtcr_layout_t
 * @brief Bit-field layout of the IWDTCR (control) register.
 *
 * @details
 * HUM Ch 28.2.3 "IWDTCR : IWDT Control Register" p 1276 + FSP
 * ``r_iwdt.c`` ``IWDT_PRV_IWDTCR_*_BIT/MASK`` macros:
 *  - ``TOPS[1:0]``   bits  1:0  -- timeout period select
 *  - ``CKS[3:0]``    bits  7:4  -- clock divider select
 *  - ``RPES[1:0]``   bits  9:8  -- refresh window end position
 *  - ``RPSS[1:0]``   bits 13:12 -- refresh window start position
 *
 * Masks below are the field width (post-shift); shifts are bit
 * positions. The IWDTCR register only takes effect in register-start
 * mode -- on the RA8D2 only auto-start is supported, so OFS0 is the
 * effective control surface. The fields exist for symmetry with WDT.
 */
typedef enum : uint16_t {
  k_ra8_iwdt_mask_tops  = 0x0003U, /**< TOPS field width (2 bits). */
  k_ra8_iwdt_mask_cks   = 0x000FU, /**< CKS field width  (4 bits). */
  k_ra8_iwdt_mask_rpes  = 0x0003U, /**< RPES field width (2 bits). */
  k_ra8_iwdt_mask_rpss  = 0x0003U, /**< RPSS field width (2 bits). */
  k_ra8_iwdt_shift_tops = 0U,      /**< TOPS occupies bits 1:0.    */
  k_ra8_iwdt_shift_cks  = 4U,      /**< CKS  occupies bits 7:4.    */
  k_ra8_iwdt_shift_rpes = 8U,      /**< RPES occupies bits 9:8.    */
  k_ra8_iwdt_shift_rpss = 12U,     /**< RPSS occupies bits 13:12.  */
} ra8_iwdt_iwdtcr_layout_t;

/**
 * @enum ra8_iwdt_iwdtsr_layout_t
 * @brief Bit definitions of the IWDTSR (status) register.
 *
 * @details
 * HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1274 + FSP
 * ``IWDT_PRV_IWDTSR_COUNTER_MASK`` / ``IWDT_PRV_STATUS_START_BIT``:
 *  - ``CNTVAL[13:0]`` bits 13:0  -- live down-counter value (RO)
 *  - ``UNDFF``        bit  14    -- underflow flag (W0C)
 *  - ``REFEF``        bit  15    -- refresh-error flag (W0C)
 *
 * Per the HUM both flag bits are write-zero-to-clear AFTER at least
 * one IWDTCLK cycle has elapsed since the flag was set; FSP polls in
 * a do/while loop until the read-back confirms the bits are gone.
 */
typedef enum : uint16_t {
  k_ra8_iwdt_sr_cnt_mask     = 0x3FFFU, /**< CNTVAL[13:0] mask.                 */
  k_ra8_iwdt_sr_undff        = 0x4000U, /**< UNDFF bit 14 (underflow flag).     */
  k_ra8_iwdt_sr_refef        = 0x8000U, /**< REFEF bit 15 (refresh-error).      */
  k_ra8_iwdt_sr_status_shift = 14U,     /**< Shift to extract UNDFF/REFEF pair. */
} ra8_iwdt_iwdtsr_layout_t;

/**
 * @enum ra8_iwdt_iwdtrcr_layout_t
 * @brief Bit definitions of the IWDTRCR (reset control) register.
 *
 * @details
 * HUM Ch 28.2.4 "IWDTRCR : IWDT Reset Control Register" p 1277 -- only
 * ``RSTIRQS`` (bit 7) is implemented. Mirrors FSP
 * ``IWDT_PRV_IWDTRCR_RESET_CONTROL_BIT``.
 */
typedef enum : uint8_t {
  k_ra8_iwdt_rcr_rstirqs = 0x80U, /**< RSTIRQS = bit 7 (1 = reset, 0 = NMI / IRQ). */
} ra8_iwdt_iwdtrcr_layout_t;

/**
 * @enum ra8_iwdt_iwdtcstpr_layout_t
 * @brief Bit definitions of the IWDTCSTPR (count-stop control) register.
 *
 * @details
 * HUM Ch 28.2.5 "IWDTCSTPR : IWDT Count Stop Control Register" p 1278
 * -- only ``SLCSTP`` (bit 7) is implemented. Mirrors FSP
 * ``IWDT_PRV_IWDTCSTPR_STOP_CONTROL_BIT``.
 */
typedef enum : uint8_t {
  k_ra8_iwdt_cstpr_slcstp = 0x80U, /**< SLCSTP = bit 7 (1 = halt counter in Sleep). */
} ra8_iwdt_iwdtcstpr_layout_t;

/**
 * @enum ra8_iwdt_ofs0_layout_t
 * @brief Bit positions and field widths of the IWDT view of OFS0.
 *
 * @details
 * HUM Ch 7 + FSP ``r_iwdt.c`` ``IWDT_PRV_OFS0_*_BIT/MASK``:
 *  - ``IWDTSTRT``     bit  1     -- 0 = auto-start, 1 = stopped
 *                                   (RA8D2 supports auto-start only)
 *  - ``IWDTTOPS``     bits 3:2   -- timeout period
 *  - ``IWDTCKS``      bits 7:4   -- clock divider
 *  - ``IWDTRPES``     bits 9:8   -- refresh window end (mirrors WDT)
 *  - ``IWDTRPSS``     bits 13:12 -- refresh window start (mirrors WDT)
 *  - ``IWDTRSTIRQS``  bit  12    -- ALSO bit 12 in some doc revisions;
 *                                   FSP defines NMI request mask
 *                                   = 0x1000 (bit 12). Use this enum
 *                                   value (``k_ra8_iwdt_ofs0_mask_nmi``)
 *                                   to test whether NMI vs reset is
 *                                   selected for IWDT underflow.
 *  - ``IWDTSTPCTL``   bit  14    -- 1 = halt counter in Sleep, 0 = run
 *
 * Note: The HUM uses ``IWDT`` field-name prefixes; FSP uses bare names.
 * We document the HUM names; the macros are identical to the WDT
 * layout in ``ra8_wdt_regs.h`` because OFS0 packs them in the same
 * relative positions inside the lower 16 bits.
 */
typedef enum : uint32_t {
  k_ra8_iwdt_ofs0_shift_strt = 1U,           /**< IWDTSTRT bit.               */
  k_ra8_iwdt_ofs0_shift_tops = 2U,           /**< IWDTTOPS shift (bits 3:2).  */
  k_ra8_iwdt_ofs0_shift_cks  = 4U,           /**< IWDTCKS shift (bits 7:4).   */
  k_ra8_iwdt_ofs0_mask_strt  = 0x00000002UL, /**< IWDTSTRT (bit 1) full mask. */
  k_ra8_iwdt_ofs0_mask_tops  = 0x00000003UL, /**< IWDTTOPS field width.       */
  k_ra8_iwdt_ofs0_mask_cks   = 0x0000000FUL, /**< IWDTCKS field width.        */
  k_ra8_iwdt_ofs0_mask_nmi   = 0x00001000UL, /**< NMI request select bit 12.  */
} ra8_iwdt_ofs0_layout_t;

/**
 * @struct r_iwdt_regs_t
 * @brief Memory-mapped layout of the IWDT block.
 *
 * @details
 * Matches FSP ``R_IWDT_Type`` (R7KA8D2KF_core0.h) byte-for-byte. The
 * total size is 12 bytes (0x0C). All reserved bytes are named ``_rN``
 * so a debugger / static analyzer can see exactly which bytes are
 * implementation-padding.
 *
 * The struct is over-decorated on purpose: the IWDTCR / IWDTRCR /
 * IWDTCSTPR registers are read-only-after-OFS0-boot on RA8D2 (no
 * register-start mode), but exposing them lets debuggers and unit
 * tests inspect the silicon view without a custom CMSIS dump.
 *
 * @see HUM Ch 28.2 "Register Descriptions" p 1273.
 */
typedef struct {
  /* HUM Ch 28.2 "Register Descriptions" p 1273 */ /* + FSP R_IWDT_Type                    */
  volatile uint8_t  IWDTRR;                        /**< +0x00 Refresh register.            */
  volatile uint8_t  _r0;                           /**< +0x01 Reserved.                    */
  volatile uint16_t IWDTCR;                        /**< +0x02 Control register.            */
  volatile uint16_t IWDTSR;                        /**< +0x04 Status register.             */
  volatile uint8_t  IWDTRCR;                       /**< +0x06 Reset control register.      */
  volatile uint8_t  _r1;                           /**< +0x07 Reserved.                    */
  volatile uint8_t  IWDTCSTPR;                     /**< +0x08 Count-stop control register. */
  volatile uint8_t  _r2;                           /**< +0x09 Reserved.                    */
  volatile uint16_t _r3;                           /**< +0x0A Reserved (pads to 0x0C).     */
} r_iwdt_regs_t;

/**
 * @brief Get pointer to the IWDT register block.
 *
 * @return Pointer to the IWDT registers.
 *
 * @pre IWDTCLK (the dedicated low-speed oscillator) is enabled at
 *      reset by hardware; nothing software-side is required to bring
 *      this peripheral online.
 * @post No hardware state is touched.
 *
 * @see HUM Ch 28.1 "Overview" p 1271.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_iwdt_regs_t* ra8_iwdt(void)
{
  /* HUM Ch 28.1 "Overview" p 1271 */
  return (volatile r_iwdt_regs_t*)k_ra8_iwdt_base_addr;
}

/**
 * @brief Refresh the IWDT counter using the two-byte unlock sequence.
 *
 * @details
 * Writes ``0x00`` then ``0xFF`` to ``IWDTRR`` -- the only legal arming
 * sequence per HUM Ch 28.2.1 p 1273 + FSP ``r_iwdt_refresh``. Any
 * deviation (single write, wrong order, intermediate write to another
 * register that maps to bit-0..7 of IWDTRR) is silently dropped by
 * silicon and the next underflow will reset the chip.
 *
 * Must happen strictly faster than the IWDT period programmed in
 * OFS0; the actual deadline depends on IWDTCLK frequency (~15 kHz)
 * times ``IWDTTOPS`` cycles divided by ``IWDTCKS``.
 *
 * @pre OFS0 has armed the IWDT (auto-start mode).
 * @post IWDTRR holds 0xFF and the down-counter has reloaded.
 *
 * @note Not thread-safe. For a multi-task RTOS port, route refreshes
 *       through a single heartbeat aggregator task.
 *
 * @warning A SINGLE write to IWDTRR does NOT refresh the counter;
 *          both bytes are required, in order.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static inline void ra8_iwdt_refresh(void)
{
  /* HUM Ch 28.2.1 "IWDTRR : IWDT Refresh Register", p 1273
   * + FSP r_iwdt_refresh (writes IWDT_PRV_REFRESH_STEP_1 then _2). */
  volatile r_iwdt_regs_t* w = ra8_iwdt();
  w->IWDTRR                 = k_ra8_iwdt_refresh_a;
  w->IWDTRR                 = k_ra8_iwdt_refresh_b;
}

#ifdef __cplusplus
}
#endif
