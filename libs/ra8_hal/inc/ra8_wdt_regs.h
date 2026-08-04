/**
 * @file ra8_wdt_regs.h
 * @brief Watchdog Timer (WDT) register layout for the RA8D2
 * @ingroup grp_hal_timers
 *
 * @details
 * The WDT is the *software*-clocked companion to the IWDT. Where the
 * IWDT runs from its own oscillator and cannot be stopped, the WDT
 * shares PCLKB with the rest of the peripherals and can be halted by
 * debug. Use the IWDT for safety resets and the WDT for application
 * liveness checks that should pause when the debugger halts.
 *
 * This header is the single source of truth for WDT register-level
 * constants -- bit positions, masks, raw encoding values for the OFSm
 * mirror of the same fields, and the dispatch instance count. The
 * higher-level driver (``ra8_wdt.h``) layers semantic enum names on
 * top.
 *
 * | Offset | Reg     | Width | RW | Purpose                |
 * |-------:|---------|------:|----|------------------------|
 * | 0x00   | WDTRR   | 8     | RW | Refresh register        |
 * | 0x02   | WDTCR   | 16    | RW | Control register        |
 * | 0x04   | WDTSR   | 16    | R  | Status register         |
 * | 0x06   | WDTRCR  | 8     | RW | Reset control           |
 * | 0x08   | WDTCSTPR| 8     | RW | Count Stop control      |
 *
 * All citations point at the RA8D2 Hardware User's Manual
 * (R01UH1065EJ), chapter 27 "Watchdog Timer (WDT)" pages 1256-1270.
 * The block is 12 bytes (0x0C) total -- matches FSP ``R_WDT_Type``
 * in R7KA8D2KF_core0.h.
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
 * @enum ra8_wdt_addr_t
 * @brief Base addresses of the WDT instances on RA8D2.
 *
 * @details
 * The chip carries two WDTs: WDT0 lives on the Cortex-M85 side and
 * WDT1 on the Cortex-M33 side. Both share the same register layout
 * (this file's ``r_wdt_regs_t``) so a single driver reaches both.
 *
 * @see HUM Ch 27.1.2 "Block Diagram" p 1256.
 */
typedef enum : uintptr_t {
  /* HUM Ch 27.1 + FSP R7KA8D2KF_core0.h R_WDT_BASE = 0x40202600,
   * R_WDT1_BASE = 0x40202700. The two instances are 0x100 apart,
   * NOT 0x10000 as an earlier version of this header claimed. */
  k_ra8_wdt_base_addr  = 0x40202600UL, /**< WDT0 base (M85 side). */
  k_ra8_wdt0_base_addr = 0x40202600UL, /**< Alias for WDT0.       */
  k_ra8_wdt1_base_addr = 0x40202700UL, /**< WDT1 base (M33 side). */
} ra8_wdt_addr_t;

/**
 * @enum ra8_wdt_refresh_t
 * @brief Two-byte unlock pattern written to ``WDTRR`` to refresh.
 *
 * @details
 * Per HUM Ch 27.2.1 p 1257 the only legal way to reload the down-
 * counter is to write ``0x00`` followed by ``0xFF``; any other
 * sequence is silently dropped.
 */
typedef enum : uint8_t {
  k_ra8_wdt_refresh_a = 0x00U, /**< First byte of the refresh sequence.  */
  k_ra8_wdt_refresh_b = 0xFFU, /**< Second byte of the refresh sequence. */
} ra8_wdt_refresh_t;

/**
 * @enum ra8_wdt_instance_t
 * @brief Hardware instance selector used by per-instance helpers.
 *
 * @details
 * RA8D2 carries WDT0 (M85) and WDT1 (M33). The driver exposes
 * per-instance refresh / OFSm decode helpers so a single firmware
 * image can manage both cores' watchdogs.
 *
 * @see HUM Ch 27.1 "Overview" p 1256.
 */
typedef enum : uint8_t {
  k_ra8_wdt0               = 0U, /**< WDT0 -- Cortex-M85 side.          */
  k_ra8_wdt1               = 1U, /**< WDT1 -- Cortex-M33 side.          */
  k_ra8_wdt_instance_count = 2U, /**< Number of WDT instances on RA8D2. */
} ra8_wdt_instance_t;

/**
 * @enum ra8_wdt_wdtcr_layout_t
 * @brief Bit-field layout of the WDTCR (control) register.
 *
 * @details
 * HUM Ch 27.2.2 "WDTCR : WDT Control Register" p 1258 defines:
 *  - ``TOPS[1:0]``  bits  1:0  -- timeout period select
 *  - ``CKS[3:0]``   bits  7:4  -- clock divider select
 *  - ``RPES[1:0]``  bits  9:8  -- refresh window end
 *  - ``RPSS[1:0]``  bits 13:12 -- refresh window start
 *
 * Masks are the field-width before shifting; shifts are the bit
 * positions. ``internal_pack_wdtcr`` ANDs the caller-supplied enum
 * with the mask and shifts into place.
 */
typedef enum : uint16_t {
  k_ra8_wdt_mask_tops  = 0x0003U, /**< TOPS field width (2 bits). */
  k_ra8_wdt_mask_cks   = 0x000FU, /**< CKS field width  (4 bits). */
  k_ra8_wdt_mask_rpes  = 0x0003U, /**< RPES field width (2 bits). */
  k_ra8_wdt_mask_rpss  = 0x0003U, /**< RPSS field width (2 bits). */
  k_ra8_wdt_shift_tops = 0U,      /**< TOPS occupies bits 1:0.    */
  k_ra8_wdt_shift_cks  = 4U,      /**< CKS  occupies bits 7:4.    */
  k_ra8_wdt_shift_rpes = 8U,      /**< RPES occupies bits 9:8.    */
  k_ra8_wdt_shift_rpss = 12U,     /**< RPSS occupies bits 13:12.  */
} ra8_wdt_wdtcr_layout_t;

/**
 * @enum ra8_wdt_wdtsr_layout_t
 * @brief Bit definitions of the WDTSR (status) register.
 *
 * @details
 * HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1260:
 *  - ``CNTVAL[13:0]`` bits 13:0  -- live down-counter value (RO)
 *  - ``UNDFF``        bit  14    -- underflow flag (W0C)
 *  - ``REFEF``        bit  15    -- refresh-error flag (W0C)
 */
typedef enum : uint16_t {
  k_ra8_wdt_sr_cnt_mask = 0x3FFFU, /**< CNTVAL[13:0] mask. */
  k_ra8_wdt_sr_undff    = 0x4000U, /**< UNDFF bit 14.      */
  k_ra8_wdt_sr_refef    = 0x8000U, /**< REFEF bit 15.      */
} ra8_wdt_wdtsr_layout_t;

/**
 * @enum ra8_wdt_wdtrcr_layout_t
 * @brief Bit definitions of the WDTRCR (reset control) register.
 *
 * @details
 * HUM Ch 27.2.4 "WDTRCR : WDT Reset Control Register" p 1262 -- only
 * the high bit ``RSTIRQS`` is implemented; all other bits read 0.
 */
typedef enum : uint8_t {
  k_ra8_wdt_rcr_rstirqs = 0x80U, /**< RSTIRQS = bit 7 (1 = reset, 0 = NMI / IRQ). */
} ra8_wdt_wdtrcr_layout_t;

/**
 * @enum ra8_wdt_wdtcstpr_layout_t
 * @brief Bit definitions of the WDTCSTPR (count stop control) register.
 *
 * @details
 * HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register" p 1262
 * -- only ``SLCSTP`` (bit 7) is implemented.
 */
typedef enum : uint8_t {
  k_ra8_wdt_cstpr_slcstp = 0x80U, /**< SLCSTP = bit 7 (1 = halt counter in Sleep). */
} ra8_wdt_wdtcstpr_layout_t;

/**
 * @enum ra8_wdt_cycles_t
 * @brief TOPS-field decoded cycle counts.
 *
 * @details
 * HUM Ch 27.2.2 Table 27.2 p 1259 row by row.
 */
typedef enum : uint16_t {
  k_ra8_wdt_cycles_1024  = 1024U,  /**< TOPS = 0b00. */
  k_ra8_wdt_cycles_4096  = 4096U,  /**< TOPS = 0b01. */
  k_ra8_wdt_cycles_8192  = 8192U,  /**< TOPS = 0b10. */
  k_ra8_wdt_cycles_16384 = 16384U, /**< TOPS = 0b11. */
} ra8_wdt_cycles_t;

/**
 * @enum ra8_wdt_div_value_t
 * @brief CKS-field decoded numeric divisors.
 *
 * @details
 * HUM Ch 27.2.2 p 1258 -- only six CKS encodings are legal; the
 * others are marked "Setting prohibited".
 */
typedef enum : uint16_t {
  k_ra8_wdt_div_value_4    = 4U,    /**< RA8 wdt div value 4.    */
  k_ra8_wdt_div_value_64   = 64U,   /**< RA8 wdt div value 64.   */
  k_ra8_wdt_div_value_128  = 128U,  /**< RA8 wdt div value 128.  */
  k_ra8_wdt_div_value_512  = 512U,  /**< RA8 wdt div value 512.  */
  k_ra8_wdt_div_value_2048 = 2048U, /**< RA8 wdt div value 2048. */
  k_ra8_wdt_div_value_8192 = 8192U, /**< RA8 wdt div value 8192. */
} ra8_wdt_div_value_t;

/**
 * @enum ra8_wdt_nmier_layout_t
 * @brief WDT-relevant bit in the ICU's NMIER register.
 *
 * @details
 * HUM Ch 14.2.14 "NMIER" p 542 -- the WDT underflow / refresh-error
 * source is gated by ``WDTEN`` at bit 1 (mask 0x2). The companion
 * ``NMICLR`` register (Ch 14.2.15 p 544) uses the same bit position
 * for the W1C flag clear.
 */
typedef enum : uint32_t {
  k_ra8_wdt_nmier_wdten_mask = 0x00000002UL, /**< NMIER.WDTEN = bit 1. */
} ra8_wdt_nmier_layout_t;

/**
 * @enum ra8_wdt_ofs_layout_t
 * @brief Bit positions and field widths of the WDT view of OFSm.
 *
 * @details
 * HUM Ch 27.3.8 Table 27.5 p 1269 -- the OFS0 word (consumed by WDT0)
 * and OFS3 word (consumed by WDT1) carry seven WDT-related fields in
 * the same bit layout:
 *
 *  - ``WDTSTRT``  bit  17    -- start mode (0 = auto, 1 = register)
 *  - ``WDTTOPS``  bits 19:18 -- timeout period
 *  - ``WDTCKS``   bits 23:20 -- clock divider
 *  - ``WDTRPES``  bits 25:24 -- refresh window end
 *  - ``WDTRPSS``  bits 27:26 -- refresh window start
 *  - ``WDTRSTIRQS`` bit 28   -- reset (1) or NMI / IRQ (0)
 *  - ``WDTSTPCTL``  bit 30   -- 1 = halt counter in Sleep, 0 = keep
 *
 * Masks are *post-shift* (i.e. the field width); shifts are bit
 * positions inside the 32-bit OFSm word.
 *
 * ``k_ra8_wdt_ofs_field_mask`` is the union of all seven fields in
 * place. It matters because ``OFS3_SEL`` (HUM Ch 7.2.7 p 289) is a
 * *per-field* selector whose bits sit at exactly these positions:
 * for each field, 0 selects ``OFS3_SEC`` and 1 selects ``OFS3``. That
 * co-location is what lets ``ra8_wdt_ofs_get`` resolve WDT1 with a
 * single bitwise mux instead of seven field extractions.
 */
typedef enum : uint32_t {
  k_ra8_wdt_ofs_shift_strt    = 17U,  /**< OFSm.WDTSTRT shift.    */
  k_ra8_wdt_ofs_shift_tops    = 18U,  /**< OFSm.WDTTOPS shift.    */
  k_ra8_wdt_ofs_shift_cks     = 20U,  /**< OFSm.WDTCKS shift.     */
  k_ra8_wdt_ofs_shift_rpes    = 24U,  /**< OFSm.WDTRPES shift.    */
  k_ra8_wdt_ofs_shift_rpss    = 26U,  /**< OFSm.WDTRPSS shift.    */
  k_ra8_wdt_ofs_shift_rstirqs = 28U,  /**< OFSm.WDTRSTIRQS shift. */
  k_ra8_wdt_ofs_shift_stpctl  = 30U,  /**< OFSm.WDTSTPCTL shift.  */
  k_ra8_wdt_ofs_mask_strt     = 0x1U, /**< 1-bit field.           */
  k_ra8_wdt_ofs_mask_tops     = 0x3U, /**< 2-bit field.           */
  k_ra8_wdt_ofs_mask_cks      = 0xFU, /**< 4-bit field.           */
  k_ra8_wdt_ofs_mask_rpes     = 0x3U, /**< 2-bit field.           */
  k_ra8_wdt_ofs_mask_rpss     = 0x3U, /**< 2-bit field.           */
  k_ra8_wdt_ofs_mask_rstirqs  = 0x1U, /**< 1-bit field.           */
  k_ra8_wdt_ofs_mask_stpctl   = 0x1U, /**< 1-bit field.           */
  /* bit 17 | 19:18 | 23:20 | 25:24 | 27:26 | 28 | 30 */
  k_ra8_wdt_ofs_field_mask = 0x5FFE0000UL, /**< Union of all seven fields. */
} ra8_wdt_ofs_layout_t;

static_assert(k_ra8_wdt_ofs_field_mask ==
                (((uint32_t)k_ra8_wdt_ofs_mask_strt << k_ra8_wdt_ofs_shift_strt) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_tops << k_ra8_wdt_ofs_shift_tops) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_cks << k_ra8_wdt_ofs_shift_cks) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_rpes << k_ra8_wdt_ofs_shift_rpes) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_rpss << k_ra8_wdt_ofs_shift_rpss) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_rstirqs << k_ra8_wdt_ofs_shift_rstirqs) |
                 ((uint32_t)k_ra8_wdt_ofs_mask_stpctl << k_ra8_wdt_ofs_shift_stpctl)),
              "k_ra8_wdt_ofs_field_mask must equal the union of the seven WDT fields");

/**
 * @struct r_wdt_regs_t
 * @brief Memory-mapped layout of one WDT instance.
 *
 * @details
 * The RA8D2 WDT block is 12 bytes (0x0C) wide -- matches the FSP
 * ``R_WDT_Type`` in R7KA8D2KF_core0.h exactly. Reserved padding is
 * named ``_rN`` so the layout is unambiguous to a debugger.
 *
 * @see HUM Ch 27.2 "Register Descriptions" p 1256 (table) and the
 *      individual subsections for each register.
 */
typedef struct {
  /* HUM Ch 27.2 "Register Descriptions" p 1256 */ /* + FSP R_WDT_Type                 */
  volatile uint8_t  WDTRR;                         /**< +0x00 Refresh register.        */
  volatile uint8_t  _r0;                           /**< +0x01 Reserved.                */
  volatile uint16_t WDTCR;                         /**< +0x02 Control register.        */
  volatile uint16_t WDTSR;                         /**< +0x04 Status register.         */
  volatile uint8_t  WDTRCR;                        /**< +0x06 Reset control register.  */
  volatile uint8_t  _r1;                           /**< +0x07 Reserved.                */
  volatile uint8_t  WDTCSTPR;                      /**< +0x08 Count-stop control.      */
  volatile uint8_t  _r2;                           /**< +0x09 Reserved.                */
  volatile uint16_t _r3;                           /**< +0x0A Reserved (pads to 0x0C). */
} r_wdt_regs_t;

/**
 * @brief Get pointer to WDT0 (the default / M85-side instance).
 *
 * @return Pointer to the WDT0 register block.
 *
 * @pre WDT module-stop bit cleared (always-on for WDT, but listed
 *      for symmetry with other peripherals).
 * @post No hardware state is touched.
 *
 * @see HUM Ch 27.1 "Overview" p 1256.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_wdt_regs_t* ra8_wdt(void)
{
  /* HUM Ch 27.1 "Overview" p 1256 */
  return (volatile r_wdt_regs_t*)k_ra8_wdt_base_addr;
}

/**
 * @brief Get pointer to a specific WDT instance.
 *
 * @param[in] which Instance selector.
 * @return Pointer to the targeted WDT register block, or WDT0 if
 *         ``which`` is out of range (defensive fallback).
 *
 * @pre ``which`` < ``k_ra8_wdt_instance_count``.
 * @post No hardware state is touched.
 *
 * @see HUM Ch 27.1 "Overview" p 1256.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_wdt_regs_t* ra8_wdt_for(ra8_wdt_instance_t which)
{
  /* HUM Ch 27.1 "Overview" p 1256 */
  switch (which) {
    case k_ra8_wdt0:
      return (volatile r_wdt_regs_t*)k_ra8_wdt0_base_addr;
    case k_ra8_wdt1:
      return (volatile r_wdt_regs_t*)k_ra8_wdt1_base_addr;
    case k_ra8_wdt_instance_count:
    default:
      return (volatile r_wdt_regs_t*)k_ra8_wdt0_base_addr;
  }
}

/**
 * @brief Refresh the WDT0 instance using the two-byte unlock sequence.
 *
 * @details
 * Writes ``0x00`` then ``0xFF`` to WDTRR -- the only legal arming
 * sequence per HUM Ch 27.2.1 p 1257. Any deviation is silently
 * dropped by silicon.
 *
 * @pre WDT0 is in register-start mode and ``ra8_wdt_init`` has run,
 *      OR WDT0 is in auto-start mode (refresh works in either case).
 * @post WDTRR holds 0xFF and the down-counter has reloaded.
 *
 * @note Not thread-safe; serialise externally if multiple writers.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static inline void ra8_wdt_refresh(void)
{
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  volatile r_wdt_regs_t* w = ra8_wdt();
  w->WDTRR                 = k_ra8_wdt_refresh_a;
  w->WDTRR                 = k_ra8_wdt_refresh_b;
}

/**
 * @brief Refresh a specific WDT instance.
 *
 * @param[in] which Instance selector.
 *
 * @pre ``which`` < ``k_ra8_wdt_instance_count``.
 * @post Targeted instance's WDTRR holds 0xFF.
 *
 * @note Not thread-safe.
 *
 * @see HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static inline void ra8_wdt_refresh_instance(ra8_wdt_instance_t which)
{
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  volatile r_wdt_regs_t* w = ra8_wdt_for(which);
  w->WDTRR                 = k_ra8_wdt_refresh_a;
  w->WDTRR                 = k_ra8_wdt_refresh_b;
}

#ifdef __cplusplus
}
#endif
