/**
 * @file ra8_ssie_regs.h
 * @brief Serial Sound Interface Enhanced (SSIE) register layout for the RA8D2
 * @ingroup grp_hal_audio
 *
 * @details
 * The RA8D2 exposes two SSIE channels (SSIE0 and SSIE1). Each
 * channel has an independent register window at base
 * ``0x4025_D000`` with a ``0x100`` stride (HUM Ch 46.2.1, p 3056).
 * The same offset map is reused by both channels and is shown
 * by HUM Table 46.9 "Bits subject to software reset by the
 * SSIRST bit", p 3081.
 *
 * Per-channel registers:
 *
 * | Offset | Name    | Width | Purpose                              |
 * |-------:|---------|------:|--------------------------------------|
 * |  0x00  | SSICR   | 32    | Control (TEN/REN, MST, DWL, SWL ...) |
 * |  0x04  | SSISR   | 32    | Status (IIRQ + over/underflow flags) |
 * |  0x10  | SSIFCR  | 32    | FIFO Control (RIE, TIE, AUCKE ...)   |
 * |  0x14  | SSIFSR  | 32    | FIFO Status (RDF, TDE, RDC, TDC)     |
 * |  0x18  | SSIFTDR | 32    | Transmit FIFO Data Register          |
 * |  0x1C  | SSIFRDR | 32    | Receive FIFO Data Register           |
 * |  0x20  | SSIOFR  | 32    | Audio Format (OMOD, LRCONT, BCKASTP) |
 * |  0x24  | SSISCR  | 32    | Status Control (TDES, RDFS)          |
 *
 * Field bit positions are sourced from HUM Ch 46.2 register
 * description tables (p 3056-3094) and cross-checked against the
 * ``ra/fsp/src/r_ssi/r_ssi.c`` shift macros in the Renesas FSP
 * sources (FSP is reference only -- no code copied).
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
 * @enum ra8_ssie_addr_t
 * @brief Memory-mapped base addresses for SSIE channels.
 *
 * @details
 * HUM Ch 46.2.1 "SSICR : Control Register", p 3056 defines the
 * base as ``SSIEn = 0x4025_D000 + 0x0100 * n (n = 0, 1)``.
 */
typedef enum : uintptr_t {
  k_ra8_ssie0_base_addr = 0x4025D000UL, /**< SSIE0 base (full duplex). */
  k_ra8_ssie1_base_addr = 0x4025D100UL, /**< SSIE1 base (half duplex). */
} ra8_ssie_addr_t;

/**
 * @enum ra8_ssie_limits_t
 * @brief Channel-count, FIFO depth and stride constants.
 */
typedef enum : uint16_t {
  k_ra8_ssie_channel_count   = 2U,     /**< SSIE0 + SSIE1.            */
  k_ra8_ssie_channel_stride  = 0x100U, /**< Bytes between channels.   */
  k_ra8_ssie_fifo_depth      = 32U,    /**< 32-stage FIFO per dir.    */
  k_ra8_ssie_fifo_half_water = 15U,    /**< Default TX watermark.     */
  k_ra8_ssie_fifo_block_size = 2U,     /**< DMA block stages.         */
  k_ra8_ssie_reset_poll_max  = 32U,    /**< Bounded reset poll loops. */
} ra8_ssie_limits_t;

/**
 * @enum ra8_ssicr_bit_t
 * @brief SSICR control-bit positions.
 *
 * @details
 * Verified against HUM Ch 46.2.1 "SSICR : Control Register"
 * p 3056-3058 and FSP r_ssi.c SSI_PRV_SSICR_* macros.
 */
typedef enum : uint8_t {
  k_ra8_ssie_bit_ren   = 0U,  /**< REN: reception enable.              */
  k_ra8_ssie_bit_ten   = 1U,  /**< TEN: transmission enable.           */
  k_ra8_ssie_bit_muen  = 3U,  /**< MUEN: mute enable.                  */
  k_ra8_ssie_bit_ckdv0 = 4U,  /**< CKDV[3:0] @ [7:4] bit clock div.    */
  k_ra8_ssie_bit_del   = 8U,  /**< DEL: serial data delay select.      */
  k_ra8_ssie_bit_pdta  = 9U,  /**< PDTA: placement data alignment.     */
  k_ra8_ssie_bit_sdta  = 10U, /**< SDTA: serial data alignment.        */
  k_ra8_ssie_bit_spdp  = 11U, /**< SPDP: padding polarity.             */
  k_ra8_ssie_bit_lrckp = 12U, /**< LRCKP: LR/FS init level + polarity  */
  k_ra8_ssie_bit_bckp  = 13U, /**< BCKP: bit clock polarity.           */
  k_ra8_ssie_bit_mst   = 14U, /**< MST: controller-mode enable.        */
  k_ra8_ssie_bit_swl0  = 16U, /**< SWL[2:0] @ [18:16] system word.     */
  k_ra8_ssie_bit_dwl0  = 19U, /**< DWL[2:0] @ [21:19] data word.       */
  k_ra8_ssie_bit_frm0  = 22U, /**< FRM[1:0] @ [23:22] frame words.     */
  k_ra8_ssie_bit_iien  = 25U, /**< IIEN: idle mode IRQ enable.         */
  k_ra8_ssie_bit_roien = 26U, /**< ROIEN: receive overflow IRQ en.     */
  k_ra8_ssie_bit_ruien = 27U, /**< RUIEN: receive underflow IRQ en.    */
  k_ra8_ssie_bit_toien = 28U, /**< TOIEN: transmit overflow IRQ en.    */
  k_ra8_ssie_bit_tuien = 29U, /**< TUIEN: transmit underflow IRQ en.   */
  k_ra8_ssie_bit_cks   = 30U, /**< CKS: controller audio-clock select. */
} ra8_ssicr_bit_t;

/**
 * @enum ra8_ssicr_mask_t
 * @brief SSICR field masks (32-bit register).
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_ren     = 0x00000001UL, /**< REN.                         */
  k_ra8_ssie_mask_ten     = 0x00000002UL, /**< TEN.                         */
  k_ra8_ssie_mask_ren_ten = 0x00000003UL, /**< REN | TEN.                   */
  k_ra8_ssie_mask_muen    = 0x00000008UL, /**< MUEN.                        */
  k_ra8_ssie_mask_ckdv    = 0x000000F0UL, /**< CKDV[3:0] @ [7:4].           */
  k_ra8_ssie_mask_del     = 0x00000100UL, /**< DEL.                         */
  k_ra8_ssie_mask_pdta    = 0x00000200UL, /**< PDTA.                        */
  k_ra8_ssie_mask_sdta    = 0x00000400UL, /**< SDTA.                        */
  k_ra8_ssie_mask_spdp    = 0x00000800UL, /**< SPDP.                        */
  k_ra8_ssie_mask_lrckp   = 0x00001000UL, /**< LRCKP.                       */
  k_ra8_ssie_mask_bckp    = 0x00002000UL, /**< BCKP.                        */
  k_ra8_ssie_mask_mst     = 0x00004000UL, /**< MST.                         */
  k_ra8_ssie_mask_swl     = 0x00070000UL, /**< SWL[2:0] @ [18:16].          */
  k_ra8_ssie_mask_dwl     = 0x00380000UL, /**< DWL[2:0] @ [21:19].          */
  k_ra8_ssie_mask_frm     = 0x00C00000UL, /**< FRM[1:0] @ [23:22].          */
  k_ra8_ssie_mask_iien    = 0x02000000UL, /**< IIEN.                        */
  k_ra8_ssie_mask_roien   = 0x04000000UL, /**< ROIEN.                       */
  k_ra8_ssie_mask_ruien   = 0x08000000UL, /**< RUIEN.                       */
  k_ra8_ssie_mask_toien   = 0x10000000UL, /**< TOIEN.                       */
  k_ra8_ssie_mask_tuien   = 0x20000000UL, /**< TUIEN.                       */
  k_ra8_ssie_mask_cks     = 0x40000000UL, /**< CKS audio clock select.      */
  k_ra8_ssie_mask_irq_all = 0x3E000000UL, /**< IIEN..TUIEN aggregate IRQ.   */
  k_ra8_ssie_mask_err_ien = 0x3C000000UL, /**< ROIEN..TUIEN tx/rx err IRQs. */
} ra8_ssicr_mask_t;

/**
 * @enum ra8_ssisr_mask_t
 * @brief SSISR status flags (HUM Ch 46.2.2, p 3066-3067).
 *
 * @details
 * IDST is the live idle-status flag at bit 0 (FSP
 * ``R_SSI0_SSISR_b.IDST``). It is read-only and reflects the SSIE
 * core state machine, not an interrupt latch; the latched IIRQ at
 * bit 25 is what fires the idle interrupt.
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_idst    = 0x00000001UL, /**< IDST live idle flag (RO). */
  k_ra8_ssie_mask_iirq    = 0x02000000UL, /**< IIRQ idle-mode flag (RO). */
  k_ra8_ssie_mask_roirq   = 0x04000000UL, /**< ROIRQ receive overflow.   */
  k_ra8_ssie_mask_ruirq   = 0x08000000UL, /**< RUIRQ receive underflow.  */
  k_ra8_ssie_mask_toirq   = 0x10000000UL, /**< TOIRQ transmit overflow.  */
  k_ra8_ssie_mask_tuirq   = 0x20000000UL, /**< TUIRQ transmit underflow. */
  k_ra8_ssie_mask_err_all = 0x3C000000UL, /**< ROIRQ|RUIRQ|TOIRQ|TUIRQ.  */
  k_ra8_ssie_mask_evt_all = 0x3E000000UL, /**< IIRQ + four error flags.  */
} ra8_ssisr_mask_t;

/**
 * @enum ra8_ssifcr_mask_t
 * @brief SSIFCR FIFO control bits (HUM Ch 46.2.3, p 3077).
 *
 * @details
 * RTRG[5:4] and TTRG[7:6] are the FIFO request trigger thresholds
 * used to drive RXI/TXI to the DMAC/DTC; see HUM Table 46.5 and
 * FSP ``R_SSI0_SSIFCR_b.RTRG`` / ``TTRG`` field definitions.
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_rfrst       = 0x00000001UL, /**< RFRST receive FIFO reset.  */
  k_ra8_ssie_mask_tfrst       = 0x00000002UL, /**< TFRST transmit FIFO reset. */
  k_ra8_ssie_mask_rfrst_tfrst = 0x00000003UL, /**< Both FIFO reset bits.      */
  k_ra8_ssie_mask_rie         = 0x00000004UL, /**< RIE receive full IRQ.      */
  k_ra8_ssie_mask_tie         = 0x00000008UL, /**< TIE transmit empty IRQ.    */
  k_ra8_ssie_mask_rie_tie     = 0x0000000CUL, /**< RIE | TIE.                 */
  k_ra8_ssie_mask_rtrg        = 0x00000030UL, /**< RTRG[1:0] @ [5:4].         */
  k_ra8_ssie_mask_ttrg        = 0x000000C0UL, /**< TTRG[1:0] @ [7:6].         */
  k_ra8_ssie_mask_bsw         = 0x00000800UL, /**< BSW byte-swap enable.      */
  k_ra8_ssie_mask_ssirst      = 0x00010000UL, /**< SSIRST software reset.     */
  k_ra8_ssie_mask_aucke       = 0x80000000UL, /**< AUCKE AUDIO_MCK enable.    */
} ra8_ssifcr_mask_t;

/**
 * @enum ra8_ssifcr_shift_t
 * @brief SSIFCR field bit positions for RTRG / TTRG.
 *
 * @details See HUM Ch 46.2.3 "SSIFCR : FIFO Control Register" p 3077
 * and FSP ``R_SSI0_SSIFCR_b.RTRG`` / ``TTRG`` (BSP header).
 */
typedef enum : uint8_t {
  k_ra8_ssie_shift_rtrg = 4U, /**< RTRG[1:0] starts at bit 4. */
  k_ra8_ssie_shift_ttrg = 6U, /**< TTRG[1:0] starts at bit 6. */
} ra8_ssifcr_shift_t;

/**
 * @enum ra8_ssifsr_mask_t
 * @brief SSIFSR FIFO status bits (HUM Ch 46.2.4, p 3083-3084).
 *
 * @details
 * RDF and TDE are write-1-to-1-keep / write-0-to-clear; FSP
 * code at p 3084 maps the only-keep encodings to the
 * ``rdf_clear`` / ``tde_clear`` constants below.
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_rdf       = 0x00000001UL, /**< RDF receive data full.      */
  k_ra8_ssie_mask_rdc       = 0x00003F00UL, /**< RDC[5:0] @ [13:8].          */
  k_ra8_ssie_mask_tde       = 0x00010000UL, /**< TDE transmit data empty.    */
  k_ra8_ssie_mask_tdc       = 0x3F000000UL, /**< TDC[5:0] @ [29:24].         */
  k_ra8_ssie_mask_rdf_clear = 0x00010000UL, /**< Keep TDE while clearing RDF */
  k_ra8_ssie_mask_tde_clear = 0x00000001UL, /**< Keep RDF while clearing TDE */
} ra8_ssifsr_mask_t;

/**
 * @enum ra8_ssifsr_shift_t
 * @brief SSIFSR field bit positions for extraction.
 */
typedef enum : uint8_t {
  k_ra8_ssie_shift_rdc = 8U,  /**< RDC[5:0] starts at bit 8.  */
  k_ra8_ssie_shift_tdc = 24U, /**< TDC[5:0] starts at bit 24. */
} ra8_ssifsr_shift_t;

/**
 * @enum ra8_ssiofr_mask_t
 * @brief SSIOFR audio format bits (HUM Ch 46.2.7, p 3091).
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_omod    = 0x00000003UL, /**< OMOD[1:0] @ [1:0].     */
  k_ra8_ssie_mask_lrcont  = 0x00000100UL, /**< LRCONT continuation.   */
  k_ra8_ssie_mask_bckastp = 0x00000200UL, /**< BCKASTP idle-stop BCK. */
} ra8_ssiofr_mask_t;

/**
 * @enum ra8_ssiscr_mask_t
 * @brief SSISCR FIFO trigger thresholds (HUM Ch 46.2.8, p 3094).
 */
typedef enum : uint32_t {
  k_ra8_ssie_mask_rdfs = 0x0000001FUL, /**< RDFS[4:0] @ [4:0].  */
  k_ra8_ssie_mask_tdes = 0x00001F00UL, /**< TDES[4:0] @ [12:8]. */
} ra8_ssiscr_mask_t;

/**
 * @enum ra8_ssiscr_shift_t
 * @brief SSISCR field bit positions.
 */
typedef enum : uint8_t {
  k_ra8_ssie_shift_rdfs = 0U, /**< RDFS[4:0] starts at bit 0. */
  k_ra8_ssie_shift_tdes = 8U, /**< TDES[4:0] starts at bit 8. */
} ra8_ssiscr_shift_t;

/**
 * @enum ra8_ssie_omod_t
 * @brief OMOD[1:0] encoding from HUM Ch 46.2.7 table p 3091.
 *
 * @details
 *  - 00b -- I2S format
 *  - 01b -- TDM format
 *  - 10b -- Monaural format
 *  - 11b -- Setting prohibited
 */
typedef enum : uint8_t {
  k_ra8_ssie_omod_i2s      = 0U, /**< RA8 ssie omod i2s.      */
  k_ra8_ssie_omod_tdm      = 1U, /**< RA8 ssie omod tdm.      */
  k_ra8_ssie_omod_monaural = 2U, /**< RA8 ssie omod monaural. */
} ra8_ssie_omod_t;

/**
 * @enum ra8_ssie_frm_t
 * @brief FRM[1:0] encoding from HUM Ch 46.2.1 table p 3057.
 *
 * @details
 * The number of words per frame depends on OMOD; see the
 * cross-table for valid combinations.
 */
typedef enum : uint8_t {
  k_ra8_ssie_frm_default = 0U, /**< 2 (I2S) / 1 (Mon) / -- (TDM) */
  k_ra8_ssie_frm_alt1    = 1U, /**< -- / -- / 4 (TDM-4)          */
  k_ra8_ssie_frm_alt2    = 2U, /**< -- / -- / 6 (TDM-6)          */
  k_ra8_ssie_frm_alt3    = 3U, /**< -- / -- / 8 (TDM-8)          */
} ra8_ssie_frm_t;

/**
 * @struct r_ssie_regs_t
 * @brief Per-channel SSIE register window.
 *
 * @details
 * Layout sourced from HUM Table 46.9 "Bits subject to software
 * reset by the SSIRST bit" p 3081 and the per-register sections
 * 46.2.1 - 46.2.8. Reserved gaps are explicit byte arrays so the
 * struct lays out at the correct offsets without compiler-specific
 * alignment hacks.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra8_ssie()`` in
 * ``libs/ra8_hal/src/ra8_ssie.c`` or by the unit test in
 * ``tests/test_ra8_ssie.c``.
 */
typedef struct {
  volatile uint32_t SSICR;   /**< +0x00 Control register.        */
  volatile uint32_t SSISR;   /**< +0x04 Status register.         */
  volatile uint8_t  _r0[8];  /**< Reserved 0x08..0x0F.           */
  volatile uint32_t SSIFCR;  /**< +0x10 FIFO control register.   */
  volatile uint32_t SSIFSR;  /**< +0x14 FIFO status register.    */
  volatile uint32_t SSIFTDR; /**< +0x18 Transmit FIFO data.      */
  volatile uint32_t SSIFRDR; /**< +0x1C Receive FIFO data.       */
  volatile uint32_t SSIOFR;  /**< +0x20 Audio format register.   */
  volatile uint32_t SSISCR;  /**< +0x24 Status control register. */
} r_ssie_regs_t;

/**
 * @brief Get pointer to an SSIE channel block.
 *
 * @param[in] channel Channel index (0 = SSIE0, 1 = SSIE1).
 * @return Volatile pointer to the channel register block, or
 *         ``nullptr`` if ``channel`` is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_ssie_regs_t* ra8_ssie(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_ssie_channel_count) {
    return nullptr;
  }
  return (volatile r_ssie_regs_t*)(k_ra8_ssie0_base_addr +
                                   ((uintptr_t)channel * (uintptr_t)k_ra8_ssie_channel_stride));
}

#ifdef __cplusplus
}
#endif
