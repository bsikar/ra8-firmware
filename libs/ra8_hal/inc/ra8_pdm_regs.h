/**
 * @file ra8_pdm_regs.h
 * @brief Pulse Density Modulation Interface (PDM-IF) register layout
 * @ingroup grp_hal_audio
 *
 * @details
 * Register map for the single RA8D2 PDM-IF block at 0x4025_6000. The
 * peripheral has one common register bank (channel start/stop/status
 * triggers, version) followed by three per-channel banks (``CH[0..2]``)
 * at offset 0x100, 0x200, 0x300. Each channel carries its own digital
 * filter chain (sinc + high-pass + compensation + half-band decimation)
 * and a 32-deep receive FIFO drained through the data-read register.
 *
 * The layout mirrors the RA8D2 Hardware User's Manual Ch 49 "Pulse
 * Density Modulation Interface (PDM-IF)" (p 3190-3256) field-for-field;
 * ``static_assert`` guards below pin the offsets so a stray padding
 * change cannot silently shift a register.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra8_pdm_addr_t
 * @brief Base address of the PDM-IF register block.
 */
typedef enum : uintptr_t {
  k_ra8_pdm_base_addr = 0x40256000UL, /**< PDM-IF Secure base (HUM Ch 49 p 3190). */
} ra8_pdm_addr_t;

/**
 * @enum ra8_pdm_fifo_t
 * @brief Receive-FIFO geometry constants (HUM Ch 49.3.4).
 */
typedef enum : uint8_t {
  k_ra8_pdm_fifo_depth = 32U, /**< PDDRR FIFO depth (samples). */
} ra8_pdm_fifo_t;

/**
 * @enum ra8_pdm_array_count_t
 * @brief Element counts for the variable-length register arrays.
 *
 * @details Length of each coefficient / reserved-word array in the PDM-IF
 *          layout. Named so the register-bank structs carry no bare array
 *          bound; every value is fixed by the HUM Ch 49 address map.
 */
typedef enum : uint8_t {
  k_ra8_pdm_cfch_count         = 11U, /**< PDCFCHR compensation-filter coeffs h(0..10). */
  k_ra8_pdm_lfch1_count        = 20U, /**< PDLFCH1R low-pass-filter coeffs h1(0..19).   */
  k_ra8_pdm_common_rsvd1_words = 21U, /**< Common bank +0x2C reserved-word run.         */
  k_ra8_pdm_common_rsvd2_words = 31U, /**< Common bank +0x84 reserved-word run.         */
} ra8_pdm_array_count_t;

/**
 * @enum ra8_pdm_ch_off_t
 * @brief Byte offsets of live registers within one channel bank (HUM Ch 49.2).
 *
 * @details Feeds the layout ``static_assert`` guards below so no bare hex
 *          offset appears in the checks. ``k_ra8_pdm_ch_bank_bytes`` is the
 *          per-channel bank stride (which equals its size).
 */
typedef enum : uint32_t {
  k_ra8_pdm_off_pdmdsr     = 0x20U,  /**< PDMDSR     Mode Setting.                  */
  k_ra8_pdm_off_pdsfcr     = 0x24U,  /**< PDSFCR     Sinc Filter Control.           */
  k_ra8_pdm_off_pdcfchr    = 0x38U,  /**< PDCFCHR    Compensation filter coeff h0.  */
  k_ra8_pdm_off_pdlfch010r = 0x64U,  /**< PDLFCH010R Low-pass filter coeff h0(10).  */
  k_ra8_pdm_off_pddrcr     = 0xE0U,  /**< PDDRCR     Data Read Control.             */
  k_ra8_pdm_off_pddrr      = 0xE8U,  /**< PDDRR      Data Read (20-bit PCM out).    */
  k_ra8_pdm_off_pddsr      = 0xECU,  /**< PDDSR      Data Status (FIFO fill count). */
  k_ra8_pdm_ch_bank_bytes  = 0x100U, /**< Per-channel bank stride and size (256 B). */
} ra8_pdm_ch_off_t;

/**
 * @enum ra8_pdm_common_off_t
 * @brief Byte offsets of live common-bank registers plus the block size.
 *
 * @details Feeds the layout ``static_assert`` guards for ``r_pdm_regs_t``.
 *          ``k_ra8_pdm_off_ch`` is where the three per-channel banks begin and
 *          ``k_ra8_pdm_block_bytes`` is the whole PDM-IF block size.
 */
typedef enum : uint32_t {
  k_ra8_pdm_off_pdcsr   = 0x10U,  /**< PDCSR Channel Status.                 */
  k_ra8_pdm_off_pdvr    = 0x80U,  /**< PDVR  Version.                        */
  k_ra8_pdm_off_ch      = 0x100U, /**< CH[0] per-channel banks start offset. */
  k_ra8_pdm_block_bytes = 0x400U, /**< Full PDM-IF block size (1024 bytes).  */
} ra8_pdm_common_off_t;

/**
 * @struct r_pdm_ch_regs_t
 * @brief Per-channel PDM-IF register bank (256 bytes, HUM Ch 49.2).
 *
 * @details
 * One instance per channel; ``ra8_pdm()->CH[n]`` selects channel n. Every
 * member maps a register documented in HUM Ch 49; the ``RESERVED*`` gaps
 * reproduce the manual's address map so the offsets of live registers
 * stay correct.
 */
typedef struct {
  volatile uint32_t PDSTRTR;                         /**< +0x00 Software Start Trigger.          */
  volatile uint32_t PDSTPTR;                         /**< +0x04 Software Stop Trigger.           */
  volatile uint32_t PDCHGTR;                         /**< +0x08 Software Change Trigger.         */
  volatile uint32_t PDICR;                           /**< +0x0C Interrupt Control.               */
  volatile uint32_t PDSDCR;                          /**< +0x10 Status Detection Control.        */
  volatile uint32_t PDSR;                            /**< +0x14 Status.                          */
  volatile uint32_t PDSCR;                           /**< +0x18 Status Clear.                    */
  volatile uint32_t RESERVED0;                       /**< +0x1C reserved.                        */
  volatile uint32_t PDMDSR;                          /**< +0x20 Mode Setting.                    */
  volatile uint32_t PDSFCR;                          /**< +0x24 Sinc Filter Control.             */
  volatile uint32_t PDHFCS0R;                        /**< +0x28 High-pass filter coeff s(0).     */
  volatile uint32_t PDHFCK1R;                        /**< +0x2C High-pass filter coeff k(1).     */
  volatile uint32_t PDHFCHR[2];                      /**< +0x30 High-pass filter coeff h(0..1).  */
  volatile uint32_t PDCFCHR[k_ra8_pdm_cfch_count];   /**< +0x38 Comp filter coeff h(0..10).      */
  volatile uint32_t PDLFCH010R;                      /**< +0x64 Low-pass filter coeff h0(10).    */
  volatile uint32_t PDLFCH1R[k_ra8_pdm_lfch1_count]; /**< +0x68 Low-pass filter coeff h1(0..19). */
  volatile uint32_t PDSDLTR;                         /**< +0xB8 Sound Detection Lower Threshold. */
  volatile uint32_t PDSDUTR;                         /**< +0xBC Sound Detection Upper Threshold. */
  volatile uint32_t PDDBCR;                          /**< +0xC0 Data Buffer Control.             */
  volatile uint32_t PDSCTSR;                         /**< +0xC4 Short Circuit Threshold Setting. */
  volatile uint32_t PDOVLTR;                         /**< +0xC8 Overvoltage Lower Threshold.     */
  volatile uint32_t PDOVUTR;                         /**< +0xCC Overvoltage Upper Threshold.     */
  volatile uint32_t RESERVED1[4];                    /**< +0xD0 reserved.                        */
  volatile uint32_t PDDRCR;                          /**< +0xE0 Data Read Control.               */
  volatile uint32_t PDDCR;                           /**< +0xE4 Data Clear.                      */
  volatile uint32_t PDDRR;                           /**< +0xE8 Data Read (20-bit PCM out).      */
  volatile uint32_t PDDSR;                           /**< +0xEC Data Status (FIFO fill count).   */
  volatile uint32_t RESERVED2[4];                    /**< +0xF0 reserved.                        */
} r_pdm_ch_regs_t;

static_assert(sizeof(r_pdm_ch_regs_t) == (size_t)k_ra8_pdm_ch_bank_bytes,
              "PDM channel bank must be 256 bytes");
static_assert(offsetof(r_pdm_ch_regs_t, PDMDSR) == (size_t)k_ra8_pdm_off_pdmdsr, "PDMDSR offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDSFCR) == (size_t)k_ra8_pdm_off_pdsfcr, "PDSFCR offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDCFCHR) == (size_t)k_ra8_pdm_off_pdcfchr,
              "PDCFCHR offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDLFCH010R) == (size_t)k_ra8_pdm_off_pdlfch010r,
              "PDLFCH010R offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDDRCR) == (size_t)k_ra8_pdm_off_pddrcr, "PDDRCR offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDDRR) == (size_t)k_ra8_pdm_off_pddrr, "PDDRR offset");
static_assert(offsetof(r_pdm_ch_regs_t, PDDSR) == (size_t)k_ra8_pdm_off_pddsr, "PDDSR offset");

/**
 * @struct r_pdm_regs_t
 * @brief Full PDM-IF register block: common bank + 3 channel banks.
 *
 * @details
 * Accessed through ``ra8_pdm()``. The common bank drives channel-wide
 * start/stop/status; ``CH[n]`` carries the per-channel configuration and
 * data path.
 */
typedef struct {
  volatile uint32_t PDCSTRTR;     /**< +0x00 Channel Software Start Trigger.  */
  volatile uint32_t PDCSTPTR;     /**< +0x04 Channel Software Stop Trigger.   */
  volatile uint32_t PDCCHGTR;     /**< +0x08 Channel Software Change Trigger. */
  volatile uint32_t PDCICR;       /**< +0x0C Channel Interrupt Control.       */
  volatile uint32_t PDCSR;        /**< +0x10 Channel Status.                  */
  volatile uint32_t PDCSCR;       /**< +0x14 Channel Status Clear.            */
  volatile uint32_t RESERVED0[2]; /**< +0x18 reserved.                        */
  volatile uint32_t PDCSDCR;      /**< +0x20 Channel Sound Detection Control. */
  volatile uint32_t PDCDRCR;      /**< +0x24 Channel Data Read Control.       */
  volatile uint32_t PDCDCR;       /**< +0x28 Channel Data Clear.              */
  /** +0x2C reserved. */
  volatile uint32_t RESERVED1[k_ra8_pdm_common_rsvd1_words];
  /** +0x80 Version. */
  volatile uint32_t PDVR;
  /** +0x84 reserved. */
  volatile uint32_t RESERVED2[k_ra8_pdm_common_rsvd2_words];
  r_pdm_ch_regs_t   CH[3]; /**< +0x100 Per-channel register banks. */
} r_pdm_regs_t;

static_assert(offsetof(r_pdm_regs_t, PDCSR) == (size_t)k_ra8_pdm_off_pdcsr, "PDCSR offset");
static_assert(offsetof(r_pdm_regs_t, PDVR) == (size_t)k_ra8_pdm_off_pdvr, "PDVR offset");
static_assert(offsetof(r_pdm_regs_t, CH) == (size_t)k_ra8_pdm_off_ch, "CH bank offset");
static_assert(sizeof(r_pdm_regs_t) == (size_t)k_ra8_pdm_block_bytes,
              "PDM block must be 1024 bytes");

/**
 * @enum ra8_pdm_pdmdsr_bits_t
 * @brief PDMDSRCHn (Mode Setting) field shifts (HUM Ch 49.2.19 p 3208).
 */
typedef enum : uint32_t {
  k_ra8_pdm_pdmdsr_inpsel   = 0x1U, /**< [0]     Input data edge select.   */
  k_ra8_pdm_pdmdsr_sfmd_pos = 4U,   /**< [6:4]   Sinc filter mode shift.   */
  k_ra8_pdm_pdmdsr_hfis_pos = 8U,   /**< [9:8]   High-pass input shift.    */
  k_ra8_pdm_pdmdsr_cfis_pos = 12U,  /**< [13:12] Compensation input shift. */
  k_ra8_pdm_pdmdsr_lfis_pos = 16U,  /**< [17:16] Low-pass input shift.     */
  k_ra8_pdm_pdmdsr_sdma_pos = 24U,  /**< [25:24] Sound-detect avg mode.    */
  k_ra8_pdm_pdmdsr_dbis_pos = 28U,  /**< [31:28] Data buffer input shift.  */
} ra8_pdm_pdmdsr_bits_t;

/**
 * @enum ra8_pdm_pdsfcr_bits_t
 * @brief PDSFCRCHn (Sinc Filter Control) field shifts (HUM Ch 49.2.20 p 3209).
 */
typedef enum : uint32_t {
  k_ra8_pdm_pdsfcr_ckdiv_pos   = 0U,  /**< [3:0]   PDM_CLKn dividend ratio. */
  k_ra8_pdm_pdsfcr_sincdec_pos = 16U, /**< [23:16] Sinc decimation ratio.   */
  k_ra8_pdm_pdsfcr_sincrng_pos = 24U, /**< [28:24] Sinc output valid range. */
} ra8_pdm_pdsfcr_bits_t;

/**
 * @enum ra8_pdm_status_bits_t
 * @brief Trigger / status / data-read bit masks (HUM Ch 49.2).
 */
typedef enum : uint32_t {
  k_ra8_pdm_pdsr_state    = 0x1U,        /**< PDSR[0]  channel running.      */
  k_ra8_pdm_pdsr_drf      = 0x4U,        /**< PDSR[2]  data reception flag.  */
  k_ra8_pdm_pdscr_clr_all = 0x08070002U, /**< PDSCR clear-all write pattern. */
  k_ra8_pdm_pdstrtr_strt  = 0x1U,        /**< PDSTRTR[0] start trigger.      */
  k_ra8_pdm_pdstptr_stp   = 0x1U,        /**< PDSTPTR[0] stop trigger.       */
  k_ra8_pdm_pdicr_idre    = 0x4U,        /**< PDICR[2] data IRQ enable.      */
  k_ra8_pdm_pddrcr_datre  = 0x1U,        /**< PDDRCR[0] data read enable.    */
} ra8_pdm_status_bits_t;

/**
 * @enum ra8_pdm_data_bits_t
 * @brief PDDRRCHn / PDDSRCHn field masks (HUM Ch 49.2.36, 49.2.37).
 */
typedef enum : uint32_t {
  k_ra8_pdm_pddrr_dat_mask = 0x000FFFFFU, /**< PDDRR[19:0] 20-bit PCM.      */
  k_ra8_pdm_pddrr_sign_bit = 0x00080000U, /**< PDDRR[19]   sign bit.        */
  k_ra8_pdm_pddrr_sign_ext = 0xFFF00000U, /**< High bits set on negatives.  */
  k_ra8_pdm_pddsr_num_mask = 0x000000FFU, /**< PDDSR[7:0]  FIFO fill count. */
} ra8_pdm_data_bits_t;

/**
 * @brief Get a pointer to the PDM-IF register block.
 * @return Volatile pointer to the common PDM-IF register bank.
 */
static inline volatile r_pdm_regs_t* ra8_pdm(void)
{
  return (volatile r_pdm_regs_t*)k_ra8_pdm_base_addr;
}

/**
 * @brief Get a pointer to a PDM-IF channel register bank.
 * @param[in] ch Channel index (0..2). Caller validates the range.
 * @return Volatile pointer to ``CH[ch]``.
 */
static inline volatile r_pdm_ch_regs_t* ra8_pdm_ch(uint8_t ch)
{
  return &ra8_pdm()->CH[ch];
}

#ifdef __cplusplus
}
#endif
