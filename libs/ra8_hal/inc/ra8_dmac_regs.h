/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dmac_regs.h
 * @brief DMAC (Direct Memory Access Controller) register layout for the RA8D2
 * @ingroup grp_hal_memory
 *
 * @details
 * 8-channel DMAC0 at `0x4000A000` with `0x40` bytes per channel. The
 * shared DMA module-control bank lives at `R_DMA_BASE = 0x4000A800`
 * (DMAST, DMCTL, DMECHR, DELSR[0..7]). Each channel supports normal,
 * repeat, block, and repeat-block transfer modes with flexible
 * trigger sources routed through the ELC via DELSR.
 *
 * Layout matches FSP `R_DMAC0_Type` (`R7KA8D2KF_core0.h` lines
 * 5681-5871, size = 52 / 0x34 bytes per channel) and `R_DMA_Type`
 * (lines 5610-5671, size = 160 / 0xa0 bytes for the shared bank).
 *
 * Bit-field positions are derived from the HUM:
 *  - 17.2.10 DMTMD: pp 738-739
 *  - 17.2.11 DMINT: p 739
 *  - 17.2.12 DMAMD: p 741
 *  - 17.2.13 DMOFR: p 743
 *  - 17.2.14 DMCNT: p 743
 *  - 17.2.20 DMAST: p 749
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/* =============================================================================
 * Base addresses (DMAC0 + shared module-control bank)
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_dmac0_base_addr = 0x4000A000UL, /**< DMAC0 channel 0 base.   */
  k_ra8_dma_base_addr   = 0x4000A800UL, /**< Shared DMA module regs. */
} ra8_dmac_addr_t;

typedef enum : uint8_t {
  k_ra8_dmac_channel_count  = 8U,    /**< RA8 DMAC channel count.         */
  k_ra8_dmac_channel_stride = 0x40U, /**< RA8 DMAC channel stride.        */
  k_ra8_dmac_tail_padding   = 0x0CU, /**< Bytes after DMBWR up to stride. */
} ra8_dmac_limits_t;

/* =============================================================================
 * Per-channel register window
 * =============================================================================
 */

/**
 * @struct r_dmac_channel_regs_t
 * @brief Per-channel DMAC register window.
 *
 * @details
 * Layout verified against FSP `R_DMAC0_Type` in `R7KA8D2KF_core0.h`
 * lines 5681-5871. Size = 52 (0x34) bytes per channel, with 12
 * bytes of tail padding up to the 0x40 channel stride.
 */
typedef struct {
  volatile uint32_t DMSAR;                        /**< +0x00 Source address.                 */
  volatile uint32_t DMDAR;                        /**< +0x04 Destination address.            */
  volatile uint32_t DMCRA;                        /**< +0x08 Transfer count (DMCRAL/DMCRAH). */
  volatile uint32_t DMCRB;                        /**< +0x0C Block transfer count.           */
  volatile uint16_t DMTMD;                        /**< +0x10 Transfer Mode.                  */
  volatile uint8_t  _r0;                          /**< +0x12 reserved.                       */
  volatile uint8_t  DMINT;                        /**< +0x13 Interrupt Setting.              */
  volatile uint16_t DMAMD;                        /**< +0x14 Address Mode.                   */
  volatile uint16_t _r1;                          /**< +0x16 reserved.                       */
  volatile uint32_t DMOFR;                        /**< +0x18 Offset Register.                */
  volatile uint8_t  DMCNT;                        /**< +0x1C Transfer Enable (bit DTE).      */
  volatile uint8_t  DMREQ;                        /**< +0x1D Software Start (SWREQ/CLRS).    */
  volatile uint8_t  DMSTS;                        /**< +0x1E Status (ESIF/DTIF/ACT).         */
  volatile uint8_t  _r2;                          /**< +0x1F reserved.                       */
  volatile uint32_t DMSRR;                        /**< +0x20 Source Reload Address.          */
  volatile uint32_t DMDRR;                        /**< +0x24 Dest Reload Address.            */
  volatile uint32_t DMSBS;                        /**< +0x28 Source Buffer Size.             */
  volatile uint32_t DMDBS;                        /**< +0x2C Dest Buffer Size.               */
  volatile uint8_t  DMBWR;                        /**< +0x30 Bufferable Write Enable.        */
  volatile uint8_t  _r3;                          /**< +0x31 reserved.                       */
  volatile uint16_t _r4;                          /**< +0x32 reserved.                       */
  volatile uint8_t  _r5[k_ra8_dmac_tail_padding]; /**< Reserved.                             */
} r_dmac_channel_regs_t;

/* =============================================================================
 * Shared DMA module-control bank (R_DMA at 0x4000A800)
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra8_dma_delsr_count = 8U,  /**< One DELSR per channel.                */
  k_ra8_dma_pad0_words  = 3U,  /**< +0x04..0x0F: 12 bytes / 4 = 3 words.  */
  k_ra8_dma_pad1_words  = 11U, /**< +0x14..0x3F: 44 bytes / 4 = 11 words. */
  k_ra8_dma_pad2_words  = 15U, /**< +0x44..0x7F: 60 bytes / 4 = 15 words. */
} ra8_dma_shared_limits_t;

/**
 * @struct r_dma_shared_regs_t
 * @brief Shared DMA module-control register window.
 *
 * @details
 * Verified against FSP `R_DMA_Type` (`R7KA8D2KF_core0.h` lines
 * 5610-5671, size = 160 / 0xa0 bytes). Holds the global activation
 * gate, controller-wide priority/error-clear control, error-channel
 * status and the per-channel ELC event-link select registers.
 */
typedef struct {
  volatile uint8_t  DMAST;                        /**< +0x00 DMA Module Activation (bit DMST). */
  volatile uint8_t  _r0;                          /**< +0x01 reserved.                         */
  volatile uint16_t _r1;                          /**< +0x02 reserved.                         */
  volatile uint32_t _r2[k_ra8_dma_pad0_words];    /**< +0x04..0x0F reserved.                   */
  volatile uint8_t  DMCTL;                        /**< +0x10 DMAC Control (PR / ERCH).         */
  volatile uint8_t  _r3;                          /**< +0x11 reserved.                         */
  volatile uint16_t _r4;                          /**< +0x12 reserved.                         */
  volatile uint32_t _r5[k_ra8_dma_pad1_words];    /**< +0x14..0x3F reserved.                   */
  volatile uint32_t DMECHR;                       /**< +0x40 Error-channel status.             */
  volatile uint32_t _r6[k_ra8_dma_pad2_words];    /**< +0x44..0x7F reserved.                   */
  volatile uint32_t DELSR[k_ra8_dma_delsr_count]; /**< +0x80 ELC link sel.                     */
} r_dma_shared_regs_t;

/* =============================================================================
 * DMTMD bit positions / masks (HUM 17.2.10, p 738)
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra8_dmtmd_dctg_pos = 0U,  /**< DCTG[1:0] bit 0..1 -- request source. */
  k_ra8_dmtmd_sz_pos   = 8U,  /**< SZ[1:0]   bit 8..9 -- transfer size.  */
  k_ra8_dmtmd_tkp_pos  = 10U, /**< TKP       bit 10   -- transfer keep.  */
  k_ra8_dmtmd_dts_pos  = 12U, /**< DTS[1:0]  bit 12..13 -- repeat area.  */
  k_ra8_dmtmd_md_pos   = 14U, /**< MD[1:0]   bit 14..15 -- mode.         */
} ra8_dmtmd_pos_t;

typedef enum : uint16_t {
  k_ra8_dmtmd_dctg_mask = (uint16_t)(0x3U << k_ra8_dmtmd_dctg_pos), /**< RA8 dmtmd dctg mask. */
  k_ra8_dmtmd_sz_mask   = (uint16_t)(0x3U << k_ra8_dmtmd_sz_pos),   /**< RA8 dmtmd sz mask.   */
  k_ra8_dmtmd_tkp_mask  = (uint16_t)(0x1U << k_ra8_dmtmd_tkp_pos),  /**< RA8 dmtmd tkp mask.  */
  k_ra8_dmtmd_dts_mask  = (uint16_t)(0x3U << k_ra8_dmtmd_dts_pos),  /**< RA8 dmtmd dts mask.  */
  k_ra8_dmtmd_md_mask   = (uint16_t)(0x3U << k_ra8_dmtmd_md_pos),   /**< RA8 dmtmd md mask.   */
} ra8_dmtmd_mask_t;

typedef enum : uint16_t {
  k_ra8_dmtmd_dctg_software = 0x0U, /**< 00b: software request. */
  k_ra8_dmtmd_dctg_hardware = 0x1U, /**< 01b: hardware request. */
} ra8_dmtmd_dctg_t;

typedef enum : uint16_t {
  k_ra8_dmtmd_sz_byte = 0x0U, /**< 00b: 8-bit.  */
  k_ra8_dmtmd_sz_half = 0x1U, /**< 01b: 16-bit. */
  k_ra8_dmtmd_sz_word = 0x2U, /**< 10b: 32-bit. */
  k_ra8_dmtmd_sz_long = 0x3U, /**< 11b: 64-bit. */
} ra8_dmtmd_sz_t;

typedef enum : uint16_t {
  k_ra8_dmtmd_dts_dest_repeat = 0x0U, /**< 00b: dest is repeat/block area. */
  k_ra8_dmtmd_dts_src_repeat  = 0x1U, /**< 01b: src is repeat/block area.  */
  k_ra8_dmtmd_dts_none        = 0x2U, /**< 10b: no repeat/block area.      */
} ra8_dmtmd_dts_t;

typedef enum : uint16_t {
  k_ra8_dmtmd_md_normal       = 0x0U, /**< 00b: normal transfer.       */
  k_ra8_dmtmd_md_repeat       = 0x1U, /**< 01b: repeat transfer.       */
  k_ra8_dmtmd_md_block        = 0x2U, /**< 10b: block transfer.        */
  k_ra8_dmtmd_md_repeat_block = 0x3U, /**< 11b: repeat-block transfer. */
} ra8_dmtmd_md_t;

/* =============================================================================
 * DMINT bit positions (HUM 17.2.11, p 739)
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra8_dmint_darie_pos = 0U, /**< DARIE: dst extended-repeat overflow IRQ. */
  k_ra8_dmint_sarie_pos = 1U, /**< SARIE: src extended-repeat overflow IRQ. */
  k_ra8_dmint_rptie_pos = 2U, /**< RPTIE: repeat-size end IRQ.              */
  k_ra8_dmint_esie_pos  = 3U, /**< ESIE:  transfer-escape end IRQ.          */
  k_ra8_dmint_dtie_pos  = 4U, /**< DTIE:  transfer-end IRQ.                 */
} ra8_dmint_pos_t;

typedef enum : uint8_t {
  k_ra8_dmint_darie_mask = (uint8_t)(0x1U << k_ra8_dmint_darie_pos), /**< RA8 dmint darie mask. */
  k_ra8_dmint_sarie_mask = (uint8_t)(0x1U << k_ra8_dmint_sarie_pos), /**< RA8 dmint sarie mask. */
  k_ra8_dmint_rptie_mask = (uint8_t)(0x1U << k_ra8_dmint_rptie_pos), /**< RA8 dmint rptie mask. */
  k_ra8_dmint_esie_mask  = (uint8_t)(0x1U << k_ra8_dmint_esie_pos),  /**< RA8 dmint esie mask.  */
  k_ra8_dmint_dtie_mask  = (uint8_t)(0x1U << k_ra8_dmint_dtie_pos),  /**< RA8 dmint dtie mask.  */
} ra8_dmint_mask_t;

/* =============================================================================
 * DMAMD bit positions (HUM 17.2.12, p 741)
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra8_dmamd_dara_pos = 0U,  /**< DARA[4:0] dst extended-repeat area. */
  k_ra8_dmamd_dadr_pos = 5U,  /**< DADR      dst addr update select.   */
  k_ra8_dmamd_dm_pos   = 6U,  /**< DM[1:0]   dst addr update mode.     */
  k_ra8_dmamd_sara_pos = 8U,  /**< SARA[4:0] src extended-repeat area. */
  k_ra8_dmamd_sadr_pos = 13U, /**< SADR      src addr update select.   */
  k_ra8_dmamd_sm_pos   = 14U, /**< SM[1:0]   src addr update mode.     */
} ra8_dmamd_pos_t;

typedef enum : uint16_t {
  k_ra8_dmamd_dara_mask = (uint16_t)(0x1FU << k_ra8_dmamd_dara_pos), /**< RA8 dmamd dara mask. */
  k_ra8_dmamd_dadr_mask = (uint16_t)(0x1U << k_ra8_dmamd_dadr_pos),  /**< RA8 dmamd dadr mask. */
  k_ra8_dmamd_dm_mask   = (uint16_t)(0x3U << k_ra8_dmamd_dm_pos),    /**< RA8 dmamd dm mask.   */
  k_ra8_dmamd_sara_mask = (uint16_t)(0x1FU << k_ra8_dmamd_sara_pos), /**< RA8 dmamd sara mask. */
  k_ra8_dmamd_sadr_mask = (uint16_t)(0x1U << k_ra8_dmamd_sadr_pos),  /**< RA8 dmamd sadr mask. */
  k_ra8_dmamd_sm_mask   = (uint16_t)(0x3U << k_ra8_dmamd_sm_pos),    /**< RA8 dmamd sm mask.   */
} ra8_dmamd_mask_t;

typedef enum : uint16_t {
  k_ra8_dmamd_addr_fixed     = 0x0U, /**< 00b: address fixed.   */
  k_ra8_dmamd_addr_offset    = 0x1U, /**< 01b: offset addition. */
  k_ra8_dmamd_addr_increment = 0x2U, /**< 10b: increment.       */
  k_ra8_dmamd_addr_decrement = 0x3U, /**< 11b: decrement.       */
} ra8_dmamd_addr_mode_t;

/* =============================================================================
 * DMCNT, DMREQ, DMSTS, DMAST bit positions
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra8_dmcnt_dte_pos  = 0U,   /**< DMCNT.DTE bit 0 (HUM 17.2.14, p 743). */
  k_ra8_dmcnt_dte_mask = 0x1U, /**< RA8 dmcnt dte mask.                   */
} ra8_dmcnt_bits_t;

typedef enum : uint8_t {
  k_ra8_dmreq_swreq_pos  = 0U,                                       /**< DMREQ.SWREQ bit 0.    */
  k_ra8_dmreq_clrs_pos   = 4U,                                       /**< DMREQ.CLRS  bit 4.    */
  k_ra8_dmreq_swreq_mask = (uint8_t)(0x1U << k_ra8_dmreq_swreq_pos), /**< RA8 dmreq swreq mask. */
  k_ra8_dmreq_clrs_mask  = (uint8_t)(0x1U << k_ra8_dmreq_clrs_pos),  /**< RA8 dmreq clrs mask.  */
} ra8_dmreq_bits_t;

typedef enum : uint8_t {
  k_ra8_dmsts_esif_pos  = 0U,                                      /**< DMSTS.ESIF bit 0.    */
  k_ra8_dmsts_dtif_pos  = 4U,                                      /**< DMSTS.DTIF bit 4.    */
  k_ra8_dmsts_act_pos   = 7U,                                      /**< DMSTS.ACT  bit 7.    */
  k_ra8_dmsts_esif_mask = (uint8_t)(0x1U << k_ra8_dmsts_esif_pos), /**< RA8 dmsts esif mask. */
  k_ra8_dmsts_dtif_mask = (uint8_t)(0x1U << k_ra8_dmsts_dtif_pos), /**< RA8 dmsts dtif mask. */
  k_ra8_dmsts_act_mask  = (uint8_t)(0x1U << k_ra8_dmsts_act_pos),  /**< RA8 dmsts act mask.  */
} ra8_dmsts_bits_t;

typedef enum : uint8_t {
  k_ra8_dmast_dmst_pos  = 0U,   /**< DMAST.DMST bit 0 (HUM 17.2.20, p 749). */
  k_ra8_dmast_dmst_mask = 0x1U, /**< RA8 dmast dmst mask.                   */
} ra8_dmast_bits_t;

/* =============================================================================
 * DMCRA halves (HUM 17.2.8) and DMSBS/DMDBS halves (HUM 17.2.17/18)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra8_dmcra_low_pos   = 0U,                             /**< RA8 dmcra low pos.   */
  k_ra8_dmcra_low_mask  = 0x3FFU << k_ra8_dmcra_low_pos,  /**< RA8 dmcra low mask.  */
  k_ra8_dmcra_high_pos  = 16U,                            /**< RA8 dmcra high pos.  */
  k_ra8_dmcra_high_mask = 0x3FFU << k_ra8_dmcra_high_pos, /**< RA8 dmcra high mask. */
} ra8_dmcra_bits_t;

typedef enum : uint32_t {
  k_ra8_dmbs_low_pos   = 0U,                                       /**< RA8 dmbs low pos.   */
  k_ra8_dmbs_low_mask  = 0xFFFFU << k_ra8_dmbs_low_pos,            /**< RA8 dmbs low mask.  */
  k_ra8_dmbs_high_pos  = 16U,                                      /**< RA8 dmbs high pos.  */
  k_ra8_dmbs_high_mask = (uint32_t)0xFFFFU << k_ra8_dmbs_high_pos, /**< RA8 dmbs high mask. */
} ra8_dmbs_bits_t;

/* =============================================================================
 * Accessors
 * =============================================================================
 */

/** @brief Pointer to DMAC channel `channel` (0..7), or NULL on error. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_dmac_channel_regs_t* ra8_dmac(uint8_t channel)
{
  if (channel >= k_ra8_dmac_channel_count) {
    return nullptr;
  }
  return (
    volatile r_dmac_channel_regs_t*)(k_ra8_dmac0_base_addr +
                                     ((uintptr_t)channel * (uintptr_t)k_ra8_dmac_channel_stride));
}

/** @brief Pointer to the shared DMA module-control bank. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_dma_shared_regs_t* ra8_dma_shared(void)
{
  return (volatile r_dma_shared_regs_t*)k_ra8_dma_base_addr;
}

#ifdef __cplusplus
}
#endif
