/**
 * @file ra8_pdg_regs.h
 * @brief PWM Delay Generation Circuit (PDG) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The PDG block (HUM Ch 23 "PWM Delay Generation Circuit (PDG)",
 * p 1152-1163) is the per-edge fine-delay engine that sits between
 * GPT320..GPT323 and the GTIOC0..GTIOC3 output pads. It provides a
 * DLL-based delay line capable of shifting the rising and falling
 * edges of each PWM channel by an integer fraction (1/64 or 1/128)
 * of the GPT core clock (GTCLK) period.
 *
 * The block exposes the following registers (HUM Ch 23.2):
 *
 * | Offset | Reg       | Width | Purpose                                            |
 * |-------:|-----------|------:|----------------------------------------------------|
 * | 0x000  | GTDLYCR   | 16    | DLL enable, reset, frequency range select          |
 * | 0x002  | GTDLYCR2  | 16    | Per-channel bypass + per-channel power enable      |
 * | 0x018  | GTDLYR0A  | 16    | Channel 0 GTIOCnA rising-edge delay code (DLY[6:0]) |
 * | 0x01C  | GTDLYR1A  | 16    | Channel 1 GTIOCnA rising-edge delay code           |
 * | 0x020  | GTDLYR2A  | 16    | Channel 2 GTIOCnA rising-edge delay code           |
 * | 0x024  | GTDLYR3A  | 16    | Channel 3 GTIOCnA rising-edge delay code           |
 * | 0x01A  | GTDLYR0B  | 16    | Channel 0 GTIOCnB rising-edge delay code           |
 * | 0x01E  | GTDLYR1B  | 16    | Channel 1 GTIOCnB rising-edge delay code           |
 * | 0x022  | GTDLYR2B  | 16    | Channel 2 GTIOCnB rising-edge delay code           |
 * | 0x026  | GTDLYR3B  | 16    | Channel 3 GTIOCnB rising-edge delay code           |
 * | 0x028  | GTDLYF0A  | 16    | Channel 0 GTIOCnA falling-edge delay code          |
 * | 0x02C  | GTDLYF1A  | 16    | Channel 1 GTIOCnA falling-edge delay code          |
 * | 0x030  | GTDLYF2A  | 16    | Channel 2 GTIOCnA falling-edge delay code          |
 * | 0x034  | GTDLYF3A  | 16    | Channel 3 GTIOCnA falling-edge delay code          |
 * | 0x02A  | GTDLYF0B  | 16    | Channel 0 GTIOCnB falling-edge delay code          |
 * | 0x02E  | GTDLYF1B  | 16    | Channel 1 GTIOCnB falling-edge delay code          |
 * | 0x032  | GTDLYF2B  | 16    | Channel 2 GTIOCnB falling-edge delay code          |
 * | 0x036  | GTDLYF3B  | 16    | Channel 3 GTIOCnB falling-edge delay code          |
 *
 * The base address per HUM Ch 23.2.1 (p 1154) is PDG = 0x4032_4000
 * (Secure alias) / PDG_NS = 0x5032_4000 (Non-Secure alias). Only the
 * Secure alias is exposed here; the driver targets the Secure world.
 *
 * Register addresses for GTDLYRnA / GTDLYRnB / GTDLYFnA / GTDLYFnB
 * follow the formulas in HUM Ch 23.2.3..23.2.6 (p 1156-1158):
 *
 *   GTDLYRnA -> 0x018 + 0x4 * n
 *   GTDLYRnB -> 0x01A + 0x4 * n
 *   GTDLYFnA -> 0x028 + 0x4 * n
 *   GTDLYFnB -> 0x02A + 0x4 * n
 *
 * The struct laid out below uses an array of two 16-bit cells per
 * channel grouped by edge so the formula collapses to a clean
 * `dly_rise[n].A` / `dly_fall[n].B` index.
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
 * @enum ra8_pdg_addr_t
 * @brief Memory-mapped base addresses for the PDG block.
 *
 * @details
 * Cited from HUM Ch 23.2.1 "GTDLYCR : PWM Output Delay Control
 * Register" p 1154. Both PDG (Secure) and PDG_NS (Non-Secure)
 * aliases exist; this driver targets PDG. Use ``uintptr_t`` so the
 * value is portable between the 32-bit RA8D2 target and the 64-bit
 * x86_64 host test build.
 */
/* HUM Ch 23.2.1 "GTDLYCR : PWM Output Delay Control Register" p 1154 */
typedef enum : uintptr_t {
  k_ra8_pdg_base_addr    = 0x40324000UL, /**< PDG Secure alias.     */
  k_ra8_pdg_ns_base_addr = 0x50324000UL, /**< PDG Non-Secure alias. */
} ra8_pdg_addr_t;

/**
 * @enum ra8_pdg_off_t
 * @brief Byte offsets of PDG registers (HUM Ch 23.2.1..23.2.6).
 *
 * @details
 * GTDLYRnA / GTDLYRnB / GTDLYFnA / GTDLYFnB obey ``base + 0x4 * n``
 * indexing inside the channel groups; the four "channel 0" offsets
 * are listed here so consumers that prefer raw offsets do not have
 * to recompute them.
 */
/* HUM Ch 23.2 "Register Descriptions" p 1154-1158 */
typedef enum : uintptr_t {
  k_ra8_pdg_off_gtdlycr  = 0x000U, /**< Delay control reg.   */
  k_ra8_pdg_off_gtdlycr2 = 0x002U, /**< Bypass / enable reg. */
  k_ra8_pdg_off_gtdlyr0a = 0x018U, /**< Ch0 GTIOCnA rise.    */
  k_ra8_pdg_off_gtdlyr0b = 0x01AU, /**< Ch0 GTIOCnB rise.    */
  k_ra8_pdg_off_gtdlyf0a = 0x028U, /**< Ch0 GTIOCnA fall.    */
  k_ra8_pdg_off_gtdlyf0b = 0x02AU, /**< Ch0 GTIOCnB fall.    */
} ra8_pdg_off_t;

/**
 * @enum ra8_pdg_count_t
 * @brief Channel count and per-channel structural sizes.
 *
 * @details
 * The PDG drives 4 GPT channels (HUM Ch 23.1 p 1152). Each channel
 * has 2 output pins (GTIOCnA / GTIOCnB) and 2 edges (rising /
 * falling), giving 4 * 2 * 2 = 16 individual delay codes. The
 * "edge slot count" field is used by the batch-update API to size
 * its per-call descriptor array.
 */
/* HUM Ch 23.1 "Overview" p 1152 */
typedef enum : uint8_t {
  k_ra8_pdg_channel_count = 4U,    /**< 4 GPT channels mapped to PDG (0..3).     */
  k_ra8_pdg_pin_count     = 2U,    /**< GTIOCnA + GTIOCnB.                       */
  k_ra8_pdg_edge_count    = 2U,    /**< Rising + falling delay slots.            */
  k_ra8_pdg_slot_count    = 16U,   /**< 4 ch * 2 pin * 2 edge = 16 delay slots.  */
  k_ra8_pdg_off_gtdlyr0   = 0x18U, /**< First GTDLYR offset (rises).             */
  k_ra8_pdg_off_after_cr2 = 0x4U,  /**< End of GTDLYCR2 (start of reserved gap). */
} ra8_pdg_count_t;

/**
 * @enum ra8_pdg_gtdlycr_bit_t
 * @brief Bit positions inside GTDLYCR (HUM Ch 23.2.1 p 1154).
 *
 * @details
 * - DLLEN @ bit 0 -- enable on-chip DLL.
 * - DLYRST @ bit 1 -- 1 = circuit held in reset, 0 = normal.
 * - FRANGE[1:0] @ bits [9:8] -- GPT core clock frequency band.
 *   00b = 80..160 MHz, 01b = 155..300 MHz; other codes are
 *   "Setting prohibited" (HUM 23.2.1 p 1154).
 */
/* HUM Ch 23.2.1 "GTDLYCR : PWM Output Delay Control Register" p 1154 */
typedef enum : uint8_t {
  k_ra8_pdg_gtdlycr_bit_dllen    = 0U, /**< DLL Operation Enable.        */
  k_ra8_pdg_gtdlycr_bit_dlyrst   = 1U, /**< PDG circuit reset.           */
  k_ra8_pdg_gtdlycr_shift_frange = 8U, /**< FRANGE[1:0] starts at bit 8. */
} ra8_pdg_gtdlycr_bit_t;

/**
 * @enum ra8_pdg_gtdlycr_mask_t
 * @brief Masks for GTDLYCR fields.
 */
/* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
typedef enum : uint16_t {
  k_ra8_pdg_gtdlycr_mask_dllen =
    (uint16_t)(1U << (uint16_t)k_ra8_pdg_gtdlycr_bit_dllen), /**< RA8 pdg gtdlycr mask dllen. */
  k_ra8_pdg_gtdlycr_mask_dlyrst =
    (uint16_t)(1U << (uint16_t)k_ra8_pdg_gtdlycr_bit_dlyrst), /**< RA8 pdg gtdlycr mask dlyrst. */
  k_ra8_pdg_gtdlycr_mask_frange =
    (uint16_t)(0x3U << (uint16_t)
                 k_ra8_pdg_gtdlycr_shift_frange), /**< RA8 pdg gtdlycr mask frange. */
} ra8_pdg_gtdlycr_mask_t;

/**
 * @enum ra8_pdg_frange_t
 * @brief GTDLYCR.FRANGE encoding (HUM Ch 23.2.1 p 1154).
 *
 * @details
 * The DLL needs to know the rough GTCLK band to pick its tap count.
 * The lower band gives 1/128 resolution; the upper band runs at
 * 1/64 resolution because the DLL halves its tap count.
 */
/* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
typedef enum : uint8_t {
  k_ra8_pdg_frange_80_160_mhz  = 0x0U, /**< 80..160 MHz, 1/128 step.               */
  k_ra8_pdg_frange_155_300_mhz = 0x1U, /**< 155..300 MHz, 1/64 step.               */
  k_ra8_pdg_frange_invalid     = 0x3U, /**< 0b11 -- "Setting prohibited" sentinel. */
} ra8_pdg_frange_t;

/**
 * @enum ra8_pdg_gtdlycr2_shift_t
 * @brief Bit positions for GTDLYCR2 fields (HUM Ch 23.2.2 p 1155).
 *
 * @details
 * - DLYBS0..DLYBS3 occupy bits [3:0] (1 = delay applied, 0 = bypass).
 * - DLYEN0..DLYEN3 occupy bits [11:8] (0 = power-on, 1 = power-off).
 *
 * Note the inverted polarity of DLYEN: the bit set means "disabled".
 * The driver hides this in its API (``exit_stop`` clears the bit,
 * ``enter_stop`` sets it).
 */
/* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
typedef enum : uint8_t {
  k_ra8_pdg_gtdlycr2_shift_dlybs = 0U, /**< DLYBS field starts at bit 0. */
  k_ra8_pdg_gtdlycr2_shift_dlyen = 8U, /**< DLYEN field starts at bit 8. */
} ra8_pdg_gtdlycr2_shift_t;

/**
 * @enum ra8_pdg_gtdlycr2_mask_t
 * @brief Masks for GTDLYCR2 fields.
 */
/* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
typedef enum : uint16_t {
  k_ra8_pdg_gtdlycr2_mask_dlybs =
    (uint16_t)(0xFU << (uint16_t)
                 k_ra8_pdg_gtdlycr2_shift_dlybs), /**< RA8 pdg gtdlycr2 mask dlybs. */
  k_ra8_pdg_gtdlycr2_mask_dlyen =
    (uint16_t)(0xFU << (uint16_t)
                 k_ra8_pdg_gtdlycr2_shift_dlyen), /**< RA8 pdg gtdlycr2 mask dlyen. */
} ra8_pdg_gtdlycr2_mask_t;

/**
 * @enum ra8_pdg_dly_field_t
 * @brief Layout of the GTDLYRnA / GTDLYRnB / GTDLYFnA / GTDLYFnB
 *        delay registers (HUM Ch 23.2.3..23.2.6 p 1156-1158).
 *
 * @details
 * Only DLY[6:0] is software-writable; bits [15:7] read as 0 and
 * must be written as 0. Allowed code range is 0x00..0x7F. Code 0x00
 * disables the per-edge delay; the meaning of the remaining codes
 * depends on FRANGE (1/128 step in the low band, 1/64 step in the
 * high band).
 */
/* HUM Ch 23.2.3 "GTDLYRnA" p 1156 */
typedef enum : uint16_t {
  k_ra8_pdg_dly_mask     = 0x007FU, /**< DLY[6:0] occupies the low 7 bits. */
  k_ra8_pdg_dly_max      = 0x007FU, /**< Maximum legal delay code.         */
  k_ra8_pdg_dly_div_low  = 128U,    /**< Divider in the 80..160 MHz band.  */
  k_ra8_pdg_dly_div_high = 64U,     /**< Divider in the 155..300 MHz band. */
} ra8_pdg_dly_field_t;

/**
 * @enum ra8_pdg_freq_band_t
 * @brief Per-band absolute frequency boundaries (Hz) for FRANGE.
 *
 * @details
 * Used by the auto-tune helper to pick a FRANGE value automatically.
 * Boundaries reproduced verbatim from HUM Ch 23.2.1 p 1154.
 */
/* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
typedef enum : uint32_t {
  k_ra8_pdg_freq_low_min_hz  = 80000000UL,  /**< 80 MHz.                  */
  k_ra8_pdg_freq_low_max_hz  = 160000000UL, /**< 160 MHz.                 */
  k_ra8_pdg_freq_high_min_hz = 155000000UL, /**< 155 MHz.                 */
  k_ra8_pdg_freq_high_max_hz = 300000000UL, /**< 300 MHz.                 */
  k_ra8_pdg_freq_overlap_top = 160000000UL, /**< Top of the overlap zone. */
  k_ra8_pdg_freq_overlap_bot = 155000000UL, /**< Bottom of overlap zone.  */
} ra8_pdg_freq_band_t;

/**
 * @struct r_pdg_dly_pair_t
 * @brief Two consecutive 16-bit delay cells that match the PDG layout.
 *
 * @details
 * The HUM lays out the delay registers as four interleaved sets:
 * GTDLYRnA / GTDLYRnB / GTDLYFnA / GTDLYFnB. Inside each set the
 * channel index ``n`` strides by 4 bytes, and the A / B pin variants
 * of the same channel are 2 bytes apart. Wrapping the pair into a
 * struct lets us index it as ``dly_rise[n].A`` / ``dly_rise[n].B``
 * which matches the FSP register-block convention.
 */
typedef struct {
  volatile uint16_t A; /**< +0 GTIOCnA delay code (DLY[6:0]). */
  volatile uint16_t B; /**< +2 GTIOCnB delay code (DLY[6:0]). */
} r_pdg_dly_pair_t;

/**
 * @struct r_pdg_regs_t
 * @brief PDG block register window mapped at ``k_ra8_pdg_base_addr``.
 *
 * @details
 * Field offsets verified against HUM Ch 23.2 (p 1154-1158):
 *
 *  - GTDLYCR   @ 0x000  (16-bit)
 *  - GTDLYCR2  @ 0x002  (16-bit)
 *  - reserved   0x004..0x017 (20 bytes)
 *  - GTDLYR[0..3] @ 0x018..0x027 (4 channels, A+B per channel)
 *  - GTDLYF[0..3] @ 0x028..0x037 (4 channels, A+B per channel)
 *
 * The reserved gap is modelled as a fixed-size byte filler so the
 * subsequent arrays land at exactly the HUM-specified offset.
 */
/* HUM Ch 23.2 "Register Descriptions" p 1154-1158 */
typedef struct {
  volatile uint16_t GTDLYCR;  /**< +0x000 HUM 23.2.1 p 1154 */
  volatile uint16_t GTDLYCR2; /**< +0x002 HUM 23.2.2 p 1155 */
  volatile uint8_t
    reserved_004[k_ra8_pdg_off_gtdlyr0 - k_ra8_pdg_off_after_cr2]; /**< +0x004..0x017 reserved. */
  r_pdg_dly_pair_t GTDLYR[k_ra8_pdg_channel_count];                /**< +0x018..0x027 rise.     */
  r_pdg_dly_pair_t GTDLYF[k_ra8_pdg_channel_count];                /**< +0x028..0x037 fall.     */
} r_pdg_regs_t;

/**
 * @brief Pointer to the PDG block.
 *
 * @return Volatile pointer at ``k_ra8_pdg_base_addr``.
 *
 * @pre Pointer cast must observe the host-vs-target uintptr_t width.
 * @pre Caller has cleared the PDG module-stop bit (MSTPD6).
 * @post Return value is non-null.
 * @post Subsequent dereferences see live PDG state.
 *
 * @note Re-entrancy: returns the same pointer on every call.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_pdg_regs_t* ra8_pdg(void)
{
  return (volatile r_pdg_regs_t*)k_ra8_pdg_base_addr;
}

#ifdef __cplusplus
}
#endif
