/**
 * @file ra8d2_adc_b_regs.h
 * @brief ADC_B (12 / 14-bit SAR) register layout for the Renesas RA8D2
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 ADC_B is a Type-B SAR converter with two independent
 * units (ADC0/ADC1) sharing one 0x2224-byte register window at
 * 0x40338000. The layout was re-derived against FSP
 * ``R_ADC_B0_Type`` in
 * ``ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/R7KA8D2KF_core0.h``
 * lines 20460-24938. The pre-RA8 ``ADCSR / ADANSA / ADCER / ADDRn``
 * layout does not exist on this part; the previous version of this
 * header carried an invented bit map (CVEN, RESSEL, ADTRGMD,
 * ADSCANMD, ADBUSY) that does not match silicon. All such fields
 * have been removed.
 *
 * Register subset modelled by this header (the ones the driver
 * actually touches):
 *
 *   | Offset | Name        | Width | Purpose                              |
 *   |-------:|-------------|------:|--------------------------------------|
 *   | 0x0000 | ADCLKENR    |  32   | ADCLK enable (CLKEN[0])              |
 *   | 0x0004 | ADCLKSR     |  32   | ADCLK status (CLKSR[0], RO)          |
 *   | 0x0008 | ADCLKCR     |  32   | ADCLK source / divider               |
 *   | 0x000C | ADSYCR      |  32   | Synchronous-op control               |
 *   | 0x0020 | ADERINTCR   |  32   | Conversion-error IE                  |
 *   | 0x0040 | ADMDR       |  32   | Per-unit mode (ADMD0[3:0],ADMD1[11:8]) |
 *   | 0x0048 | ADSGER      |  32   | Scan-group enable (SGREn[8:0])       |
 *   | 0x005C | ADINTCR     |  32   | Per-group scan-end IE                |
 *   | 0x0400 | ADCMPENR    |  32   | Compare-match enable (CMPENn[7:0])   |
 *   | 0x0404 | ADCMPINTCR  |  32   | Compare-match IE (CMPIEn[3:0])       |
 *   | 0x0448 | ADCMPMDR0   |  32   | CMPMD0..3 mode select (2b each)      |
 *   | 0x044C | ADCMPMDR1   |  32   | CMPMD4..7 mode select (2b each)      |
 *   | 0x0458 | ADCMPTBR[8] |  32   | (low,high) compare table, stride 4   |
 *   | 0x0600 | ADCHCR[24]  |  32   | Per-channel config (stride 0x10)     |
 *   | 0x0608 | ADDOPCRB[24]|  32   | Per-channel AVEMD/CMPTBLEm (stride 0x10)|
 *   | 0x0C08 | ADTRGENR    |  32   | Trigger enable (STTRGENn[8:0])       |
 *   | 0x0C10 | ADSYSTR     |  32   | Synchronous SW start (ADSYSTn[8:0])  |
 *   | 0x0C20 | ADSTR[9]    |  32   | Per-group SW start (ADST[0])         |
 *   | 0x0C60 | ADSTOPR     |  32   | Force stop (ADSTOP0[0],ADSTOP1[8])   |
 *   | 0x0C80 | ADSR        |  32   | Status (ADACT0[0], ADACT1[1], RO)    |
 *   | 0x2000 | ADDR[23]    |  32   | Conversion result (DATA[15:0],ERR[31])|
 *
 * The full FSP struct exposes ~200 registers (FIFO, calibration,
 * compare, user gain/offset tables, sample-and-hold, etc.). They
 * land here as new consumers ask for them.
 *
 * On the host unit-test build the FW build maps these MMIO offsets
 * onto an anonymous ``ra_sim_mmap`` window so the same accessors
 * work in tests as on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_adc_b_addr_t
 * @brief Memory-mapped base address of the ADC_B register window.
 */
typedef enum : uintptr_t {
  k_ra_adc_b_base_addr = 0x40338000UL, /**< FSP R_ADC_B0_BASE. */
} ra_adc_b_addr_t;

/**
 * @enum ra_adc_b_limits_t
 * @brief Channel counts, register strides, and array sizes.
 *
 * @details
 * The widest value is 0x10 (ADCHCR stride) so a uint8_t backing
 * suffices. Loop variables driven by these limits should also be
 * uint8_t to satisfy clang-tidy's bugprone-too-small-loop-variable.
 */
typedef enum : uint8_t {
  k_ra_adc_b_unit_count    = 2U,    /**< Two independent ADC units (ADC0/ADC1). */
  k_ra_adc_b_max_channels  = 24U,   /**< ADCHCR0..23 virtual-channel config slots. */
  k_ra_adc_b_result_regs   = 23U,   /**< ADDR[0..22] result slots. */
  k_ra_adc_b_scan_groups   = 9U,    /**< ADSGER / ADTRGENR / ADSYSTR width [8:0]. */
  k_ra_adc_b_cmp_tables    = 8U,    /**< ADCMPTBR0..7 / CMPMD0..7 / CMPENn[7:0]. */
  k_ra_adc_b_adchcr_stride = 0x10U, /**< ADCHCRn occupies 16 bytes per slot. */
  k_ra_adc_b_addr_stride   = 0x4U,  /**< ADDR[n] is 4 bytes per slot. */
  k_ra_adc_b_adstr_stride  = 0x4U,  /**< ADSTR[n] is 4 bytes per slot. */
  k_ra_adc_b_cmptbr_stride = 0x4U,  /**< ADCMPTBRn is 4 bytes per slot. */
} ra_adc_b_limits_t;

/**
 * @enum ra_adc_b_off_t
 * @brief Register offsets inside the ADC_B block (subset modelled).
 *
 * @details
 * Offsets follow FSP ``R_ADC_B0_Type``. The full struct is 0x2224
 * bytes; only the registers exercised by the driver are listed.
 */
typedef enum : uint16_t {
  k_ra_adc_b_off_adclkenr   = 0x0000U, /**< ADCLK Enable. */
  k_ra_adc_b_off_adclksr    = 0x0004U, /**< ADCLK Status (RO). */
  k_ra_adc_b_off_adclkcr    = 0x0008U, /**< ADCLK Control. */
  k_ra_adc_b_off_adsycr     = 0x000CU, /**< Synchronous Operation Control. */
  k_ra_adc_b_off_aderintcr  = 0x0020U, /**< Conversion Error IE. */
  k_ra_adc_b_off_admdr      = 0x0040U, /**< Per-unit Mode Selection. */
  k_ra_adc_b_off_adsger     = 0x0048U, /**< Scan-Group Enable. */
  k_ra_adc_b_off_adintcr    = 0x005CU, /**< Per-group Scan-End IE. */
  k_ra_adc_b_off_adcmpenr   = 0x0400U, /**< Compare-Match Enable (CMPENn[7:0]). */
  k_ra_adc_b_off_adcmpintcr = 0x0404U, /**< Compare-Match Interrupt Enable. */
  k_ra_adc_b_off_adcmpmdr0  = 0x0448U, /**< Compare-Match Mode CMPMD0..3. */
  k_ra_adc_b_off_adcmpmdr1  = 0x044CU, /**< Compare-Match Mode CMPMD4..7. */
  k_ra_adc_b_off_adcmptbr0  = 0x0458U, /**< ADCMPTBR[0] (low/high pair). */
  k_ra_adc_b_off_adchcr0    = 0x0600U, /**< ADCHCR[0] -- per-channel config. */
  k_ra_adc_b_off_addopcrb0  = 0x0608U, /**< ADDOPCRB[0] -- AVEMD/ADC/CMPTBLEm. */
  k_ra_adc_b_off_adtrgenr   = 0x0C08U, /**< Trigger Enable per group. */
  k_ra_adc_b_off_adsystr    = 0x0C10U, /**< Synchronous SW Start. */
  k_ra_adc_b_off_adstr0     = 0x0C20U, /**< ADSTR[0] per-group SW start. */
  k_ra_adc_b_off_adstopr    = 0x0C60U, /**< Force Stop. */
  k_ra_adc_b_off_adsr       = 0x0C80U, /**< Conversion Status (RO). */
  k_ra_adc_b_off_addr0      = 0x2000U, /**< ADDR[0] -- conversion results. */
} ra_adc_b_off_t;

/**
 * @enum ra_adclkenr_bit_t
 * @brief ADCLKENR bit positions.
 */
typedef enum : uint8_t {
  k_ra_adclkenr_bit_clken = 0U, /**< ADCLK enable. */
} ra_adclkenr_bit_t;

/**
 * @enum ra_adclkenr_mask_t
 * @brief ADCLKENR bit masks.
 */
typedef enum : uint32_t {
  k_ra_adclkenr_mask_clken = 0x00000001UL,
} ra_adclkenr_mask_t;

/**
 * @enum ra_adclksr_mask_t
 * @brief ADCLKSR bit masks (RO clock-running flag).
 */
typedef enum : uint32_t {
  k_ra_adclksr_mask_clksr = 0x00000001UL,
} ra_adclksr_mask_t;

/**
 * @enum ra_admdr_bit_t
 * @brief ADMDR bit positions (per-unit mode).
 *
 * @details
 * ADMDR holds two 4-bit per-unit "mode" fields: ADMD0[3:0] for
 * ADC0 and ADMD1[11:8] for ADC1. The encoding is silicon-defined
 * by the HUM and selects between single-shot, continuous, group
 * scan, and synchronous modes.
 */
typedef enum : uint8_t {
  k_ra_admdr_bit_admd0 = 0U, /**< ADC0 mode select [3:0]. */
  k_ra_admdr_bit_admd1 = 8U, /**< ADC1 mode select [11:8]. */
} ra_admdr_bit_t;

/**
 * @enum ra_admdr_mask_t
 * @brief ADMDR bit masks.
 */
typedef enum : uint32_t {
  k_ra_admdr_mask_admd0 = 0x0000000FUL, /**< ADMD0 [3:0]. */
  k_ra_admdr_mask_admd1 = 0x00000F00UL, /**< ADMD1 [11:8]. */
} ra_admdr_mask_t;

/**
 * @enum ra_admdr_mode_t
 * @brief ADMDR per-unit mode codes (subset of HUM Ch 53 ADMD0/1 table).
 *
 * @details
 * The HUM enumerates 16 codes; this driver uses single-shot
 * (0x0) for the default polling read-channel path and one-cycle
 * scan (0x1) when the caller requests scan mode. The remaining
 * codes (continuous, multi-trigger, group-scan, etc.) land here
 * as future consumers need them.
 */
typedef enum : uint8_t {
  k_ra_admdr_mode_disabled   = 0x0U, /**< Unit disabled / single-shot SW. */
  k_ra_admdr_mode_one_cycle  = 0x1U, /**< Single-shot scan, one cycle. */
  k_ra_admdr_mode_continuous = 0x2U, /**< Continuous-scan repeating. */
} ra_admdr_mode_t;

/**
 * @enum ra_adchcr_bit_t
 * @brief ADCHCRn bit positions (per virtual-channel config).
 *
 * @details
 * Each ADCHCRn (one per virtual channel 0..23) selects:
 *   - SGSEL[4:0]   scan-group membership
 *   - CNVCS[14:8]  physical analog channel number
 *   - AINMD[15]    differential vs single-ended
 *   - SSTSEL[19:16] sampling-state-table selection
 *
 * These four fields fully describe the channel; resolution is
 * controlled at the unit level via ADMDR, not per channel.
 */
typedef enum : uint8_t {
  k_ra_adchcr_bit_sgsel  = 0U,  /**< Scan group selection. */
  k_ra_adchcr_bit_cnvcs  = 8U,  /**< Physical channel selection. */
  k_ra_adchcr_bit_ainmd  = 15U, /**< Analog input mode (single/diff). */
  k_ra_adchcr_bit_sstsel = 16U, /**< Sampling-state table selection. */
} ra_adchcr_bit_t;

/**
 * @enum ra_adchcr_mask_t
 * @brief ADCHCRn bit masks.
 */
typedef enum : uint32_t {
  k_ra_adchcr_mask_sgsel  = 0x0000001FUL, /**< [4:0]   */
  k_ra_adchcr_mask_cnvcs  = 0x00007F00UL, /**< [14:8]  */
  k_ra_adchcr_mask_ainmd  = 0x00008000UL, /**< [15]    */
  k_ra_adchcr_mask_sstsel = 0x000F0000UL, /**< [19:16] */
} ra_adchcr_mask_t;

/**
 * @enum ra_adsger_mask_t
 * @brief ADSGER scan-group-enable mask (SGREn[8:0]).
 */
typedef enum : uint32_t {
  k_ra_adsger_mask_sgren = 0x000001FFUL,
} ra_adsger_mask_t;

/**
 * @enum ra_adsystr_mask_t
 * @brief ADSYSTR synchronous SW start mask (ADSYSTn[8:0]).
 */
typedef enum : uint32_t {
  k_ra_adsystr_mask_adsystn = 0x000001FFUL,
} ra_adsystr_mask_t;

/**
 * @enum ra_adstr_mask_t
 * @brief ADSTR[n] per-group SW start mask (ADST[0]).
 */
typedef enum : uint32_t {
  k_ra_adstr_mask_adst = 0x00000001UL,
} ra_adstr_mask_t;

/**
 * @enum ra_adsr_bit_t
 * @brief ADSR bit positions (busy / calibration flags, RO).
 */
typedef enum : uint8_t {
  k_ra_adsr_bit_adact0  = 0U,  /**< ADC0 conversion in progress. */
  k_ra_adsr_bit_adact1  = 1U,  /**< ADC1 conversion in progress. */
  k_ra_adsr_bit_calact0 = 16U, /**< ADC0 calibration in progress. */
  k_ra_adsr_bit_calact1 = 17U, /**< ADC1 calibration in progress. */
} ra_adsr_bit_t;

/**
 * @enum ra_adsr_mask_t
 * @brief ADSR bit masks.
 */
typedef enum : uint32_t {
  k_ra_adsr_mask_adact0  = 0x00000001UL,
  k_ra_adsr_mask_adact1  = 0x00000002UL,
  k_ra_adsr_mask_adact   = 0x00000003UL, /**< Either unit busy. */
  k_ra_adsr_mask_calact0 = 0x00010000UL,
  k_ra_adsr_mask_calact1 = 0x00020000UL,
} ra_adsr_mask_t;

/**
 * @enum ra_adstopr_mask_t
 * @brief ADSTOPR force-stop bit masks.
 */
typedef enum : uint32_t {
  k_ra_adstopr_mask_adstop0 = 0x00000001UL, /**< [0]   */
  k_ra_adstopr_mask_adstop1 = 0x00000100UL, /**< [8]   */
} ra_adstopr_mask_t;

/**
 * @enum ra_addr_mask_t
 * @brief ADDR[n] result-register field masks.
 */
typedef enum : uint32_t {
  k_ra_addr_mask_data = 0x0000FFFFUL, /**< [15:0]  conversion data. */
  k_ra_addr_mask_err  = 0x80000000UL, /**< [31]    conversion error flag. */
} ra_addr_mask_t;

/**
 * @enum ra_adtrgenr_mask_t
 * @brief ADTRGENR per-group hardware-trigger enable mask (STTRGENn[8:0]).
 */
typedef enum : uint32_t {
  k_ra_adtrgenr_mask_sttrgen = 0x000001FFUL,
} ra_adtrgenr_mask_t;

/**
 * @enum ra_adcmpenr_mask_t
 * @brief ADCMPENR compare-table enable mask (CMPENn[7:0]).
 */
typedef enum : uint32_t {
  k_ra_adcmpenr_mask_cmpen = 0x000000FFUL,
} ra_adcmpenr_mask_t;

/**
 * @enum ra_adcmptbr_bit_t
 * @brief ADCMPTBRn (low,high) packed-pair bit positions.
 */
typedef enum : uint8_t {
  k_ra_adcmptbr_bit_low  = 0U,  /**< Low-side window level [15:0]. */
  k_ra_adcmptbr_bit_high = 16U, /**< High-side window level [31:16]. */
} ra_adcmptbr_bit_t;

/**
 * @enum ra_adcmptbr_mask_t
 * @brief ADCMPTBRn field masks (low + high 16-bit thresholds).
 */
typedef enum : uint32_t {
  k_ra_adcmptbr_mask_low  = 0x0000FFFFUL,
  k_ra_adcmptbr_mask_high = 0xFFFF0000UL,
} ra_adcmptbr_mask_t;

/**
 * @enum ra_adcmpmd_t
 * @brief CMPMDn 2-bit window-mode codes (per HUM Ch 53).
 */
typedef enum : uint8_t {
  k_ra_adcmpmd_disabled   = 0x0U, /**< Compare match disabled. */
  k_ra_adcmpmd_inside     = 0x1U, /**< Match when low <= data <= high. */
  k_ra_adcmpmd_outside    = 0x2U, /**< Match when data < low OR data > high. */
  k_ra_adcmpmd_greater_eq = 0x3U, /**< Match when data >= high (single-sided). */
} ra_adcmpmd_t;

/**
 * @enum ra_adcmpmd_layout_t
 * @brief CMPMDn field layout inside ADCMPMDR0 / ADCMPMDR1.
 *
 * @details
 * Each register packs four 2-bit CMPMD fields at byte boundaries.
 * Bit position for table N is @c (N % 4) * 8.
 */
typedef enum : uint8_t {
  k_ra_adcmpmd_field_width = 2U, /**< Each CMPMDn occupies 2 bits. */
  k_ra_adcmpmd_field_step  = 8U, /**< Stride between CMPMDn fields. */
  k_ra_adcmpmd_per_reg     = 4U, /**< Four CMPMDn per ADCMPMDR. */
} ra_adcmpmd_layout_t;

/**
 * @enum ra_addopcrb_bit_t
 * @brief ADDOPCRB[ch] bit positions (per-channel oversampling + cmp enable).
 *
 * @details
 *   - AVEMD[1:0]    addition / averaging mode (off / add / avg / mix)
 *   - ADC[11:8]     averaging-times selection (1x / 2x / 4x / 8x / 16x...)
 *   - CMPTBLEm[23:16] per-channel compare-table enable mask
 */
typedef enum : uint8_t {
  k_ra_addopcrb_bit_avemd    = 0U,
  k_ra_addopcrb_bit_adc      = 8U,
  k_ra_addopcrb_bit_cmptblem = 16U,
} ra_addopcrb_bit_t;

/**
 * @enum ra_addopcrb_mask_t
 * @brief ADDOPCRB[ch] field masks.
 */
typedef enum : uint32_t {
  k_ra_addopcrb_mask_avemd    = 0x00000003UL, /**< [1:0]   */
  k_ra_addopcrb_mask_adc      = 0x00000F00UL, /**< [11:8]  */
  k_ra_addopcrb_mask_cmptblem = 0x00FF0000UL, /**< [23:16] */
} ra_addopcrb_mask_t;

/**
 * @enum ra_addopcrb_avemd_t
 * @brief ADDOPCRB.AVEMD encoding (per HUM Ch 53).
 */
typedef enum : uint8_t {
  k_ra_avemd_off     = 0x0U, /**< Disabled (single-sample). */
  k_ra_avemd_add     = 0x1U, /**< Sum N samples. */
  k_ra_avemd_average = 0x2U, /**< Sum then divide by N. */
} ra_addopcrb_avemd_t;

/**
 * @enum ra_addopcrb_adc_t
 * @brief ADDOPCRB.ADC averaging-times encoding.
 */
typedef enum : uint8_t {
  k_ra_adc_avg_1x  = 0x0U,
  k_ra_adc_avg_2x  = 0x1U,
  k_ra_adc_avg_4x  = 0x2U,
  k_ra_adc_avg_8x  = 0x3U,
  k_ra_adc_avg_16x = 0x4U,
  k_ra_adc_avg_64x = 0x6U,
} ra_addopcrb_adc_t;

/**
 * @brief Pointer to the 32-bit ADCLKENR register.
 * @return Volatile pointer to ADCLKENR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adclkenr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adclkenr);
}

/**
 * @brief Pointer to the 32-bit ADCLKSR register.
 * @return Volatile pointer to ADCLKSR (RO clock-running status).
 */
static inline volatile uint32_t* ra_adc_b_adclksr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adclksr);
}

/**
 * @brief Pointer to the 32-bit ADCLKCR register.
 * @return Volatile pointer to ADCLKCR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adclkcr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adclkcr);
}

/**
 * @brief Pointer to the 32-bit ADMDR register.
 * @return Volatile pointer to ADMDR (RW per-unit mode).
 */
static inline volatile uint32_t* ra_adc_b_admdr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_admdr);
}

/**
 * @brief Pointer to the 32-bit ADSGER register (scan-group enable).
 * @return Volatile pointer to ADSGER (RW).
 */
static inline volatile uint32_t* ra_adc_b_adsger(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adsger);
}

/**
 * @brief Pointer to the 32-bit ADINTCR register (per-group scan-end IE).
 * @return Volatile pointer to ADINTCR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adintcr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adintcr);
}

/**
 * @brief Pointer to the 32-bit ADSYSTR register (synchronous SW start).
 * @return Volatile pointer to ADSYSTR (WO).
 */
static inline volatile uint32_t* ra_adc_b_adsystr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adsystr);
}

/**
 * @brief Pointer to the 32-bit ADSTOPR register (force-stop).
 * @return Volatile pointer to ADSTOPR (WO).
 */
static inline volatile uint32_t* ra_adc_b_adstopr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adstopr);
}

/**
 * @brief Pointer to the 32-bit ADSR register (status).
 * @return Volatile pointer to ADSR (RO).
 */
static inline volatile uint32_t* ra_adc_b_adsr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adsr);
}

/**
 * @brief Pointer to the per-virtual-channel ADCHCR register.
 *
 * @param[in] ch Virtual channel index (0..23).
 * @return Volatile pointer, or ``nullptr`` if ``ch`` is out of range.
 */
static inline volatile uint32_t* ra_adc_b_adchcr(uint8_t ch)
{
  if (ch >= k_ra_adc_b_max_channels) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adchcr0 +
                              ((uintptr_t)ch * (uintptr_t)k_ra_adc_b_adchcr_stride));
}

/**
 * @brief Pointer to the per-scan-group ADSTR[n] register.
 *
 * @param[in] group Scan-group index (0..8).
 * @return Volatile pointer to ADSTR[group], or ``nullptr`` if ``group``
 *         is out of range.
 */
static inline volatile uint32_t* ra_adc_b_adstr(uint8_t group)
{
  if (group >= k_ra_adc_b_scan_groups) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adstr0 +
                              ((uintptr_t)group * (uintptr_t)k_ra_adc_b_adstr_stride));
}

/**
 * @brief Pointer to the 32-bit ADTRGENR register (per-group HW trigger enable).
 * @return Volatile pointer to ADTRGENR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adtrgenr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adtrgenr);
}

/**
 * @brief Pointer to the 32-bit ADCMPENR register (compare-table enable).
 * @return Volatile pointer to ADCMPENR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adcmpenr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcmpenr);
}

/**
 * @brief Pointer to the 32-bit ADCMPINTCR register (compare-match IE).
 * @return Volatile pointer to ADCMPINTCR (RW).
 */
static inline volatile uint32_t* ra_adc_b_adcmpintcr(void)
{
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcmpintcr);
}

/**
 * @brief Pointer to ADCMPMDR0 / ADCMPMDR1 (compare-table mode select).
 *
 * @param[in] which 0 -> ADCMPMDR0 (CMPMD0..3), 1 -> ADCMPMDR1 (CMPMD4..7).
 * @return Volatile pointer, or ``nullptr`` if @p which is out of range.
 */
static inline volatile uint32_t* ra_adc_b_adcmpmdr(uint8_t which)
{
  if (which == 0U) {
    return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcmpmdr0);
  }
  if (which == 1U) {
    return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcmpmdr1);
  }
  return nullptr;
}

/**
 * @brief Pointer to a per-table ADCMPTBR[n] register (low/high pair).
 *
 * @param[in] table Table index (0..7).
 * @return Volatile pointer or ``nullptr`` if @p table is out of range.
 */
static inline volatile uint32_t* ra_adc_b_adcmptbr(uint8_t table)
{
  if (table >= k_ra_adc_b_cmp_tables) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_adcmptbr0 +
                              ((uintptr_t)table * (uintptr_t)k_ra_adc_b_cmptbr_stride));
}

/**
 * @brief Pointer to the per-virtual-channel ADDOPCRB[n] register.
 *
 * @details
 * ADDOPCRB shares the 16-byte ADCHCR slot stride; channel @p ch maps to
 * @c addopcrb0 + ch*16 (the per-channel data-operation control B register
 * holds AVEMD averaging mode, ADC averaging-times, and CMPTBLEm).
 *
 * @param[in] ch Virtual channel index (0..23).
 * @return Volatile pointer or ``nullptr`` if @p ch is out of range.
 */
static inline volatile uint32_t* ra_adc_b_addopcrb(uint8_t ch)
{
  if (ch >= k_ra_adc_b_max_channels) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_addopcrb0 +
                              ((uintptr_t)ch * (uintptr_t)k_ra_adc_b_adchcr_stride));
}

/**
 * @brief Pointer to the per-virtual-channel ADDR[n] result register.
 *
 * @param[in] ch Virtual channel index (0..22).
 * @return Volatile pointer to the 32-bit result slot, or ``nullptr``
 *         if ``ch`` is out of range. ADDR[23] does not exist on this
 *         silicon -- the FSP struct only declares 23 result slots.
 */
static inline volatile uint32_t* ra_adc_b_addr(uint8_t ch)
{
  if (ch >= k_ra_adc_b_result_regs) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra_adc_b_base_addr + k_ra_adc_b_off_addr0 +
                              ((uintptr_t)ch * (uintptr_t)k_ra_adc_b_addr_stride));
}

#ifdef __cplusplus
}
#endif
