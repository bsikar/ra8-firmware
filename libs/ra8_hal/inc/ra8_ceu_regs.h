/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ceu_regs.h
 * @brief Capture Engine Unit (CEU) register layout for the RA8D2
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The CEU drives the parallel camera input path (VIO_D[15:0],
 * VIO_VD, VIO_HD, VIO_CLK, VIO_FLD) and DMAs captured frames into
 * SRAM/SDRAM. On the EK-RA8D2 board this is the path used by the
 * on-board OV5640 5MP CMOS sensor.
 *
 * Most CEU registers exist in three planes:
 *  - Plane A at base + 0x0000 (active programming view)
 *  - Plane B at base + 0x1000 (shadow plane, writable while A is in
 *    use; the CEU swaps planes on the next VD edge)
 *  - Mirror at base + 0x2000 (always targets the inactive plane)
 *
 * Per HUM Ch 60.2 Table 60.4 "Register configuration of CEU"
 * p 3629, the offset registers (CAMOR, CAPWR), filter registers
 * (CFLCR, CFSZR), destination width (CDWDR), every address
 * register (CDAYR/CDACR/CDBYR/CDBCR/CBDSR), the low-pass and data
 * output controls (CLFCR, CDOCR), and the bundle 2 address group
 * (CDAYR2/CDACR2/CDBYR2/CDBCR2) all expose the three-plane window.
 * Single-plane registers -- CAPSR, CAPCR, CAMCR, CMCYR, CAIFR,
 * CRCNTR, CRCMPR, CFWCR, CEIER, CETCR, CSTSR, CDSSR -- only have
 * a Plane A address and ignore the +0x1000 / +0x2000 offsets.
 *
 * ## Register table (verified against HUM Table 60.4 p 3629-3630)
 *
 * | Offset | Name      | R/W | Purpose                                     |
 * |-------:|-----------|-----|---------------------------------------------|
 * | 0x0000 | CAPSR     | R/W | Capture start (CE) / software reset (CPKIL) |
 * | 0x0004 | CAPCR     | R/W | Capture control (continuous, burst mode)    |
 * | 0x0008 | CAMCR     | R/W | Camera interface control                    |
 * | 0x000C | CMCYR     | R/W | Camera interface cycle (HCYL / VCYL)        |
 * | 0x0010 | CAMOR     | R/W | Camera offset (HOFST / VOFST) [3-plane]     |
 * | 0x0014 | CAPWR     | R/W | Camera capture width (HWDTH / VWDTH)        |
 * | 0x0018 | CAIFR     | R/W | Camera interface input format               |
 * | 0x0028 | CRCNTR    | R/W | Register control (plane-swap)               |
 * | 0x002C | CRCMPR    | R/W | Register forcible control                   |
 * | 0x0030 | CFLCR     | R/W | Capture filter control (scale-down factor)  |
 * | 0x0034 | CFSZR     | R/W | Capture filter size clip                    |
 * | 0x0038 | CDWDR     | R/W | Capture destination width                   |
 * | 0x003C | CDAYR     | R/W | Capture data Y address (top-field / frame)  |
 * | 0x0040 | CDACR     | R/W | Capture data C address                      |
 * | 0x0044 | CDBYR     | R/W | Capture data Y bottom-field                 |
 * | 0x0048 | CDBCR     | R/W | Capture data C bottom-field                 |
 * | 0x004C | CBDSR     | R/W | Capture bundle destination size             |
 * | 0x005C | CFWCR     | R/W | Firewall operation control                  |
 * | 0x0060 | CLFCR     | R/W | Low-pass filter control                     |
 * | 0x0064 | CDOCR     | R/W | Data output control (byte swap, format)     |
 * | 0x0070 | CEIER     | R/W | Event interrupt enable                      |
 * | 0x0074 | CETCR     | R/W | Event flag clear (write-0-to-clear)         |
 * | 0x007C | CSTSR     | R   | Status (CPTON: capture in progress)         |
 * | 0x0084 | CDSSR     | R/W | Data size status                            |
 * | 0x0090 | CDAYR2    | R/W | Bundle 2 Y address                          |
 * | 0x0094 | CDACR2    | R/W | Bundle 2 C address                          |
 * | 0x0098 | CDBYR2    | R/W | Bundle 2 Y bottom-field                     |
 * | 0x009C | CDBCR2    | R/W | Bundle 2 C bottom-field                     |
 *
 * The base address itself is already declared in
 * `ra8_glcdc_regs.h` (`k_ra8_ceu_base_addr` in the
 * `ra8_display_addr_t` enum), so this header pulls that header in
 * rather than redeclaring the constant.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_glcdc_regs.h" /* pulls in k_ra8_ceu_base_addr */

/**
 * @enum ra8_ceu_plane_off_t
 * @brief Plane stride between Plane A, Plane B, and the mirror view.
 *
 * @details
 * Per HUM Ch 60.2 Table 60.4 "Register configuration of CEU"
 * p 3629-3630, only a subset of CEU registers expose the
 * three-plane layout; for those that do, Plane B sits at base
 * + 0x1000 and the mirror at base + 0x2000.
 */
typedef enum : uint16_t {
  k_ra8_ceu_plane_a_off      = 0x0000U, /**< Plane A (active).        */
  k_ra8_ceu_plane_b_off      = 0x1000U, /**< Plane B (shadow).        */
  k_ra8_ceu_plane_mirror_off = 0x2000U, /**< Mirror (inactive plane). */
} ra8_ceu_plane_off_t;

/**
 * @enum ra8_ceu_off_t
 * @brief Plane A register offsets inside the CEU window.
 *
 * @details
 * Verified byte-for-byte against HUM Ch 60.2 Table 60.4 "Register
 * configuration of CEU" p 3629-3630. Mirror addresses for the
 * Plane A view (used by this driver) follow the FSP convention of
 * "+0x1000 -> Plane B, +0x2000 -> mirror" -- see
 * `ra8_ceu_plane_off_t` and `ra8_ceu_reg32_plane`.
 */
typedef enum : uint16_t {
  k_ra8_ceu_off_capsr  = 0x0000U, /**< Capture Start Register.                  */
  k_ra8_ceu_off_capcr  = 0x0004U, /**< Capture Control Register.                */
  k_ra8_ceu_off_camcr  = 0x0008U, /**< Camera Interface Control Register.       */
  k_ra8_ceu_off_cmcyr  = 0x000CU, /**< Camera Interface Cycle Register.         */
  k_ra8_ceu_off_camor  = 0x0010U, /**< Camera Interface Offset Register.        */
  k_ra8_ceu_off_capwr  = 0x0014U, /**< Camera Interface Width Register.         */
  k_ra8_ceu_off_caifr  = 0x0018U, /**< Camera Interface Input Format Register.  */
  k_ra8_ceu_off_crcntr = 0x0028U, /**< Register Control Register.               */
  k_ra8_ceu_off_crcmpr = 0x002CU, /**< Register Forcible Control Register.      */
  k_ra8_ceu_off_cflcr  = 0x0030U, /**< Capture Filter Control Register.         */
  k_ra8_ceu_off_cfszr  = 0x0034U, /**< Capture Filter Size Clip Register.       */
  k_ra8_ceu_off_cdwdr  = 0x0038U, /**< Capture Destination Width Register.      */
  k_ra8_ceu_off_cdayr  = 0x003CU, /**< Capture Data Y Address Register.         */
  k_ra8_ceu_off_cdacr  = 0x0040U, /**< Capture Data C Address Register.         */
  k_ra8_ceu_off_cdbyr  = 0x0044U, /**< Capture Data Y Bottom-Field Register.    */
  k_ra8_ceu_off_cdbcr  = 0x0048U, /**< Capture Data C Bottom-Field Register.    */
  k_ra8_ceu_off_cbdsr  = 0x004CU, /**< Capture Bundle Destination Size.         */
  k_ra8_ceu_off_cfwcr  = 0x005CU, /**< Firewall Operation Control Register.     */
  k_ra8_ceu_off_clfcr  = 0x0060U, /**< Capture Low-Pass Filter Control.         */
  k_ra8_ceu_off_cdocr  = 0x0064U, /**< Capture Data Output Control Register.    */
  k_ra8_ceu_off_ceier  = 0x0070U, /**< Capture Event Interrupt Enable Register. */
  k_ra8_ceu_off_cetcr  = 0x0074U, /**< Capture Event Flag Clear Register.       */
  k_ra8_ceu_off_cstsr  = 0x007CU, /**< Capture Status Register (R).             */
  k_ra8_ceu_off_cdssr  = 0x0084U, /**< Capture Data Size Status Register.       */
  k_ra8_ceu_off_cdayr2 = 0x0090U, /**< Capture Data Y Address Register 2.       */
  k_ra8_ceu_off_cdacr2 = 0x0094U, /**< Capture Data C Address Register 2.       */
  k_ra8_ceu_off_cdbyr2 = 0x0098U, /**< Capture Data Y2 Bottom-Field Register.   */
  k_ra8_ceu_off_cdbcr2 = 0x009CU, /**< Capture Data C2 Bottom-Field Register.   */
} ra8_ceu_off_t;

/**
 * @enum ra8_ceu_capsr_bit_t
 * @brief CAPSR (offset 0x0000) bit positions.
 *
 * @details
 * Per HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630-3631:
 *  - bit 0 (CE)    : 0 stops capture, 1 starts capture at next VD.
 *  - bit 16 (CPKIL): 1 issues a software reset of capturing.
 */
typedef enum : uint8_t {
  k_ra8_ceu_capsr_bit_ce    = 0U,  /**< CE @ bit 0.     */
  k_ra8_ceu_capsr_bit_cpkil = 16U, /**< CPKIL @ bit 16. */
} ra8_ceu_capsr_bit_t;

/**
 * @enum ra8_ceu_capsr_mask_t
 * @brief CAPSR bit masks (full 32-bit register).
 */
typedef enum : uint32_t {
  k_ra8_ceu_capsr_mask_ce    = (1UL << 0),  /**< CE: capture-enable bit.    */
  k_ra8_ceu_capsr_mask_cpkil = (1UL << 16), /**< CPKIL: software reset bit. */
} ra8_ceu_capsr_mask_t;

/**
 * @enum ra8_ceu_capcr_shift_t
 * @brief CAPCR (offset 0x0004) field shifts.
 *
 * @details
 * Per HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634-3635
 * the layout is:
 *  - bit 16    CTNCP    : continuous capture enable.
 *  - bits 21:20 MTCM[1:0]: bus burst mode (32/64/128/256-byte unit).
 *  - bits 31:24 FDRP[7:0]: frame-drop interval count.
 */
typedef enum : uint8_t {
  k_ra8_ceu_capcr_shift_ctncp = 16U, /**< CTNCP @ bit 16.      */
  k_ra8_ceu_capcr_shift_mtcm  = 20U, /**< MTCM[1:0] @ [21:20]. */
  k_ra8_ceu_capcr_shift_fdrp  = 24U, /**< FDRP[7:0] @ [31:24]. */
} ra8_ceu_capcr_shift_t;

/**
 * @enum ra8_ceu_capcr_mask_t
 * @brief CAPCR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_capcr_mask_ctncp = (1UL << 16),    /**< CTNCP: continuous mode.   */
  k_ra8_ceu_capcr_mask_mtcm  = (3UL << 20),    /**< MTCM[1:0] burst-size sel. */
  k_ra8_ceu_capcr_mask_fdrp  = (0xFFUL << 24), /**< FDRP[7:0] frame-drop cnt. */
} ra8_ceu_capcr_mask_t;

/**
 * @enum ra8_ceu_camcr_shift_t
 * @brief CAMCR (offset 0x0008) field shifts.
 *
 * @details
 * Per HUM Ch 60.2.3 "CAMCR : Camera Interface Control Register"
 * p 3636-3641 the layout is:
 *  - bit 0     HDPOL   : HD polarity.
 *  - bit 1     VDPOL   : VD polarity.
 *  - bits 5:4  JPG[1:0]: capture format (image / data sync / data en).
 *  - bits 9:8  DTARY[1:0]: input order for image-capture mode.
 *  - bit 12    DTIF    : data bus width (8/16-bit).
 *  - bit 16    FLDPOL  : field signal polarity (interlace).
 *  - bit 24    DSEL    : data latch edge select.
 *  - bit 25    FLDSEL  : FLD latch edge select.
 *  - bit 26    HDSEL   : HD latch edge select.
 *  - bit 27    VDSEL   : VD latch edge select.
 */
typedef enum : uint8_t {
  k_ra8_ceu_camcr_shift_hdpol  = 0U,  /**< HDPOL @ bit 0.      */
  k_ra8_ceu_camcr_shift_vdpol  = 1U,  /**< VDPOL @ bit 1.      */
  k_ra8_ceu_camcr_shift_jpg    = 4U,  /**< JPG[1:0] @ [5:4].   */
  k_ra8_ceu_camcr_shift_dtary  = 8U,  /**< DTARY[1:0] @ [9:8]. */
  k_ra8_ceu_camcr_shift_dtif   = 12U, /**< DTIF @ bit 12.      */
  k_ra8_ceu_camcr_shift_fldpol = 16U, /**< FLDPOL @ bit 16.    */
  k_ra8_ceu_camcr_shift_dsel   = 24U, /**< DSEL @ bit 24.      */
  k_ra8_ceu_camcr_shift_fldsel = 25U, /**< FLDSEL @ bit 25.    */
  k_ra8_ceu_camcr_shift_hdsel  = 26U, /**< HDSEL @ bit 26.     */
  k_ra8_ceu_camcr_shift_vdsel  = 27U, /**< VDSEL @ bit 27.     */
} ra8_ceu_camcr_shift_t;

/**
 * @enum ra8_ceu_camcr_mask_t
 * @brief CAMCR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_camcr_mask_hdpol  = (1UL << 0),  /**< RA8 CEU camcr mask hdpol.  */
  k_ra8_ceu_camcr_mask_vdpol  = (1UL << 1),  /**< RA8 CEU camcr mask vdpol.  */
  k_ra8_ceu_camcr_mask_jpg    = (3UL << 4),  /**< RA8 CEU camcr mask jpg.    */
  k_ra8_ceu_camcr_mask_dtary  = (3UL << 8),  /**< RA8 CEU camcr mask dtary.  */
  k_ra8_ceu_camcr_mask_dtif   = (1UL << 12), /**< RA8 CEU camcr mask dtif.   */
  k_ra8_ceu_camcr_mask_fldpol = (1UL << 16), /**< RA8 CEU camcr mask fldpol. */
  k_ra8_ceu_camcr_mask_dsel   = (1UL << 24), /**< RA8 CEU camcr mask dsel.   */
  k_ra8_ceu_camcr_mask_fldsel = (1UL << 25), /**< RA8 CEU camcr mask fldsel. */
  k_ra8_ceu_camcr_mask_hdsel  = (1UL << 26), /**< RA8 CEU camcr mask hdsel.  */
  k_ra8_ceu_camcr_mask_vdsel  = (1UL << 27), /**< RA8 CEU camcr mask vdsel.  */
} ra8_ceu_camcr_mask_t;

/**
 * @enum ra8_ceu_cmcyr_shift_t
 * @brief CMCYR (offset 0x000C) field shifts.
 *
 * @details
 * HUM Ch 60.2.4 "CMCYR : Camera Interface Cycle Register"
 * p 3641 splits the register into:
 *  - bits 13:0  HCYL[13:0]: horizontal cycle count.
 *  - bits 29:16 VCYL[13:0]: vertical line count.
 */
typedef enum : uint8_t {
  k_ra8_ceu_cmcyr_shift_hcyl = 0U,  /**< HCYL[13:0] @ [13:0].  */
  k_ra8_ceu_cmcyr_shift_vcyl = 16U, /**< VCYL[13:0] @ [29:16]. */
} ra8_ceu_cmcyr_shift_t;

/**
 * @enum ra8_ceu_cmcyr_mask_t
 * @brief CMCYR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cmcyr_mask_hcyl = 0x00003FFFUL, /**< HCYL[13:0] @ [13:0].  */
  k_ra8_ceu_cmcyr_mask_vcyl = 0x3FFF0000UL, /**< VCYL[13:0] @ [29:16]. */
} ra8_ceu_cmcyr_mask_t;

/**
 * @enum ra8_ceu_camor_shift_t
 * @brief CAMOR (offset 0x0010) field shifts.
 *
 * @details
 * HUM Ch 60.2.5 "CAMOR : Camera Interface Offset Register" p 3642
 * splits the register into:
 *  - bits 12:0  HOFST[12:0]: horizontal capture-start offset.
 *  - bits 27:16 VOFST[11:0]: vertical capture-start offset.
 */
typedef enum : uint8_t {
  k_ra8_ceu_camor_shift_hofst = 0U,  /**< HOFST[12:0] @ [12:0].  */
  k_ra8_ceu_camor_shift_vofst = 16U, /**< VOFST[11:0] @ [27:16]. */
} ra8_ceu_camor_shift_t;

/**
 * @enum ra8_ceu_camor_mask_t
 * @brief CAMOR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_camor_mask_hofst = 0x00001FFFUL, /**< HOFST @ [12:0].  */
  k_ra8_ceu_camor_mask_vofst = 0x0FFF0000UL, /**< VOFST @ [27:16]. */
} ra8_ceu_camor_mask_t;

/**
 * @enum ra8_ceu_capwr_shift_t
 * @brief CAPWR (offset 0x0014) field shifts.
 *
 * @details
 * HUM Ch 60.2.6 "CAPWR : Camera Interface Width Register" p 3646
 * splits the register into:
 *  - bits 12:0  HWDTH[12:0]: horizontal capture width (cycles).
 *  - bits 27:16 VWDTH[11:0]: vertical capture lines.
 */
typedef enum : uint8_t {
  k_ra8_ceu_capwr_shift_hwdth = 0U,  /**< HWDTH[12:0] @ [12:0].  */
  k_ra8_ceu_capwr_shift_vwdth = 16U, /**< VWDTH[11:0] @ [27:16]. */
} ra8_ceu_capwr_shift_t;

/**
 * @enum ra8_ceu_capwr_mask_t
 * @brief CAPWR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_capwr_mask_hwdth = 0x00001FFFUL, /**< RA8 CEU capwr mask hwdth. */
  k_ra8_ceu_capwr_mask_vwdth = 0x0FFF0000UL, /**< RA8 CEU capwr mask vwdth. */
} ra8_ceu_capwr_mask_t;

/**
 * @enum ra8_ceu_caifr_shift_t
 * @brief CAIFR (offset 0x0018) field shifts.
 *
 * @details
 * HUM Ch 60.2.7 "CAIFR : Camera Interface Input Format Register"
 * p 3647 splits the register into:
 *  - bits 1:0  FCI[1:0]: first-captured-image select for interlace
 *                        (00=immediate, 01=top, 10=bottom).
 *  - bit 4     CIM     : capture image (0 = both-field, 1 = one-field).
 *  - bit 8     IFS     : input format select (0 = progressive, 1 = interlace).
 */
typedef enum : uint8_t {
  k_ra8_ceu_caifr_shift_fci = 0U, /**< FCI[1:0] @ [1:0]. */
  k_ra8_ceu_caifr_shift_cim = 4U, /**< CIM @ bit 4.      */
  k_ra8_ceu_caifr_shift_ifs = 8U, /**< IFS @ bit 8.      */
} ra8_ceu_caifr_shift_t;

/**
 * @enum ra8_ceu_caifr_mask_t
 * @brief CAIFR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_caifr_mask_fci = (3UL << 0), /**< FCI[1:0] mask. */
  k_ra8_ceu_caifr_mask_cim = (1UL << 4), /**< CIM mask.      */
  k_ra8_ceu_caifr_mask_ifs = (1UL << 8), /**< IFS mask.      */
} ra8_ceu_caifr_mask_t;

/**
 * @enum ra8_ceu_crcntr_mask_t
 * @brief CRCNTR (offset 0x0028) bit masks.
 *
 * @details
 * HUM Ch 60.2.8 "CRCNTR : CEU Register Control Register" p 3649
 * lays out the plane-swap control:
 *  - bit 0  RC : Plane A enable.
 *  - bit 1  RS : Plane B enable.
 *  - bit 4  RVS: register-set sync enable (latch on VD).
 */
typedef enum : uint32_t {
  k_ra8_ceu_crcntr_mask_rc  = (1UL << 0), /**< RC: enable Plane A.  */
  k_ra8_ceu_crcntr_mask_rs  = (1UL << 1), /**< RS: enable Plane B.  */
  k_ra8_ceu_crcntr_mask_rvs = (1UL << 4), /**< RVS: VD-sync update. */
} ra8_ceu_crcntr_mask_t;

/**
 * @enum ra8_ceu_crcmpr_mask_t
 * @brief CRCMPR (offset 0x002C) bit masks.
 *
 * @details
 * HUM Ch 60.2.9 "CRCMPR : CEU Register Forcible Control Register"
 * p 3649. Bit 0 (RA) forces an immediate plane swap regardless of
 * VD timing.
 */
typedef enum : uint32_t {
  k_ra8_ceu_crcmpr_mask_ra = (1UL << 0), /**< RA: force plane swap. */
} ra8_ceu_crcmpr_mask_t;

/**
 * @enum ra8_ceu_cflcr_shift_t
 * @brief CFLCR (offset 0x0030) field shifts.
 *
 * @details
 * HUM Ch 60.2.10 "CFLCR : Capture Filter Control Register" p 3650.
 * Scale-down factor encoded as fixed-point fraction.fmant:
 *  - bits 11:0  HFRAC[11:0]: horizontal scale fractional bits.
 *  - bits 15:12 HMANT[3:0]: horizontal scale mantissa bits.
 *  - bits 27:16 VFRAC[11:0]: vertical scale fractional bits.
 *  - bits 31:28 VMANT[3:0]: vertical scale mantissa bits.
 */
typedef enum : uint8_t {
  k_ra8_ceu_cflcr_shift_hfrac = 0U,  /**< RA8 CEU cflcr shift hfrac. */
  k_ra8_ceu_cflcr_shift_hmant = 12U, /**< RA8 CEU cflcr shift hmant. */
  k_ra8_ceu_cflcr_shift_vfrac = 16U, /**< RA8 CEU cflcr shift vfrac. */
  k_ra8_ceu_cflcr_shift_vmant = 28U, /**< RA8 CEU cflcr shift vmant. */
} ra8_ceu_cflcr_shift_t;

/**
 * @enum ra8_ceu_cflcr_mask_t
 * @brief CFLCR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cflcr_mask_hfrac = 0x00000FFFUL, /**< RA8 CEU cflcr mask hfrac. */
  k_ra8_ceu_cflcr_mask_hmant = 0x0000F000UL, /**< RA8 CEU cflcr mask hmant. */
  k_ra8_ceu_cflcr_mask_vfrac = 0x0FFF0000UL, /**< RA8 CEU cflcr mask vfrac. */
  k_ra8_ceu_cflcr_mask_vmant = 0xF0000000UL, /**< RA8 CEU cflcr mask vmant. */
} ra8_ceu_cflcr_mask_t;

/**
 * @enum ra8_ceu_cfszr_shift_t
 * @brief CFSZR (offset 0x0034) field shifts.
 *
 * @details
 * HUM Ch 60.2.11 "CFSZR : Capture Filter Size Clip Register" p 3651:
 *  - bits 11:0  HFCLP[11:0]: horizontal clip in 4-pixel units.
 *  - bits 27:16 VFCLP[11:0]: vertical clip in 4-pixel units.
 */
typedef enum : uint8_t {
  k_ra8_ceu_cfszr_shift_hfclp = 0U,  /**< RA8 CEU cfszr shift hfclp. */
  k_ra8_ceu_cfszr_shift_vfclp = 16U, /**< RA8 CEU cfszr shift vfclp. */
} ra8_ceu_cfszr_shift_t;

/**
 * @enum ra8_ceu_cfszr_mask_t
 * @brief CFSZR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cfszr_mask_hfclp = 0x00000FFFUL, /**< RA8 CEU cfszr mask hfclp. */
  k_ra8_ceu_cfszr_mask_vfclp = 0x0FFF0000UL, /**< RA8 CEU cfszr mask vfclp. */
} ra8_ceu_cfszr_mask_t;

/**
 * @enum ra8_ceu_cdwdr_mask_t
 * @brief CDWDR (offset 0x0038) bit masks.
 *
 * @details
 * HUM Ch 60.2.12 "CDWDR : Capture Destination Width Register"
 * p 3654. Holds the destination horizontal stride in 4-byte units.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cdwdr_mask_chdw = 0x00001FFFUL, /**< CHDW[12:0] stride. */
} ra8_ceu_cdwdr_mask_t;

/**
 * @enum ra8_ceu_cbdsr_mask_t
 * @brief CBDSR (offset 0x004C) bit masks.
 *
 * @details
 * HUM Ch 60.2.17 "CBDSR : Capture Bundle Destination Size
 * Register" p 3660. Number of lines (image / sync fetch) or bytes
 * (data-enable fetch) for each bundle write. Writes to bits [2:0]
 * are ignored (8-line / 32-byte alignment).
 */
typedef enum : uint32_t {
  k_ra8_ceu_cbdsr_mask_cbvs = 0x007FFFFFUL, /**< CBVS[22:0] bundle size. */
} ra8_ceu_cbdsr_mask_t;

/**
 * @enum ra8_ceu_cfwcr_shift_t
 * @brief CFWCR (offset 0x005C) field shifts.
 *
 * @details
 * HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register"
 * p 3661. Programs a TrustZone-style window of legal write
 * addresses for data-enable fetch:
 *  - bit 0   FWE       : firewall enable.
 *  - bits 31:5 FWV[26:0]: upper firewall address bits (lower 5 are
 *                         implicitly 0x1F).
 */
typedef enum : uint8_t {
  k_ra8_ceu_cfwcr_shift_fwe = 0U, /**< RA8 CEU cfwcr shift fwe. */
  k_ra8_ceu_cfwcr_shift_fwv = 5U, /**< RA8 CEU cfwcr shift fwv. */
} ra8_ceu_cfwcr_shift_t;

/**
 * @enum ra8_ceu_cfwcr_mask_t
 * @brief CFWCR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cfwcr_mask_fwe = (1UL << 0),   /**< FWE: enable.        */
  k_ra8_ceu_cfwcr_mask_fwv = 0xFFFFFFE0UL, /**< FWV[26:0] @ [31:5]. */
} ra8_ceu_cfwcr_mask_t;

/**
 * @enum ra8_ceu_clfcr_mask_t
 * @brief CLFCR (offset 0x0060) bit masks.
 *
 * @details
 * HUM Ch 60.2.19 "CLFCR : Capture Low-Pass Filter Control
 * Register" p 3662. Single-bit enable for the input low-pass
 * filter ahead of the scale-down stage.
 */
typedef enum : uint32_t {
  k_ra8_ceu_clfcr_mask_lpf = (1UL << 0), /**< LPF: enable LPF. */
} ra8_ceu_clfcr_mask_t;

/**
 * @enum ra8_ceu_cdocr_shift_t
 * @brief CDOCR (offset 0x0064) field shifts.
 *
 * @details
 * HUM Ch 60.2.20 "CDOCR : Capture Data Output Control Register"
 * p 3662-3663:
 *  - bit 0  COBS: 8-bit unit byte swap.
 *  - bit 1  COWS: 16-bit unit byte swap.
 *  - bit 2  COLS: 32-bit unit byte swap.
 *  - bit 4  CDS : output format (0 = YCbCr420, 1 = YCbCr422).
 *  - bit 16 CBE : bundle write enable.
 */
typedef enum : uint8_t {
  k_ra8_ceu_cdocr_shift_cobs = 0U,  /**< RA8 CEU cdocr shift cobs. */
  k_ra8_ceu_cdocr_shift_cows = 1U,  /**< RA8 CEU cdocr shift cows. */
  k_ra8_ceu_cdocr_shift_cols = 2U,  /**< RA8 CEU cdocr shift cols. */
  k_ra8_ceu_cdocr_shift_cds  = 4U,  /**< RA8 CEU cdocr shift cds.  */
  k_ra8_ceu_cdocr_shift_cbe  = 16U, /**< RA8 CEU cdocr shift cbe.  */
} ra8_ceu_cdocr_shift_t;

/**
 * @enum ra8_ceu_cdocr_mask_t
 * @brief CDOCR field masks.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cdocr_mask_cobs = (1UL << 0),  /**< RA8 CEU cdocr mask cobs. */
  k_ra8_ceu_cdocr_mask_cows = (1UL << 1),  /**< RA8 CEU cdocr mask cows. */
  k_ra8_ceu_cdocr_mask_cols = (1UL << 2),  /**< RA8 CEU cdocr mask cols. */
  k_ra8_ceu_cdocr_mask_cds  = (1UL << 4),  /**< RA8 CEU cdocr mask cds.  */
  k_ra8_ceu_cdocr_mask_cbe  = (1UL << 16), /**< RA8 CEU cdocr mask cbe.  */
} ra8_ceu_cdocr_mask_t;

/**
 * @enum ra8_ceu_cstsr_mask_t
 * @brief CSTSR (offset 0x007C) status bit masks.
 *
 * @details
 * HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672. CSTSR
 * is read-only:
 *  - bit 0  CPTON: 1 -> capture currently running.
 *  - bit 16 CPFLD: current field (0 = top, 1 = bottom) when in
 *                  interlace mode.
 *  - bit 24 CRST : 1 -> CEU is in software-reset (CPKIL) sequence.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cstsr_mask_cpton = (1UL << 0),  /**< CPTON: capture running. */
  k_ra8_ceu_cstsr_mask_cpfld = (1UL << 16), /**< CPFLD: current field.   */
  k_ra8_ceu_cstsr_mask_crst  = (1UL << 24), /**< CRST: reset in flight.  */
} ra8_ceu_cstsr_mask_t;

/**
 * @enum ra8_ceu_cetcr_mask_t
 * @brief CETCR (offset 0x0074) event flag bit positions.
 *
 * @details
 * HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register"
 * p 3664-3667. CEIER uses identical bit positions to gate which
 * events raise the CEU IRQ. Writing 0 to any CETCR bit clears it
 * (write-0-to-clear); writing 1 leaves it asserted.
 *
 * Every documented event in HUM Table 60.10 / Figure 60.45 is
 * enumerated here so the dispatcher can fan out the full mask:
 *
 *  | Bit | Mnemonic | Description                                  |
 *  |----:|----------|----------------------------------------------|
 *  |  0  | CPE      | One-frame capture end                        |
 *  |  1  | CFE      | One-field capture end                        |
 *  |  4  | IGRW     | Illegal register write during capture        |
 *  |  8  | HD       | Horizontal sync detect                       |
 *  |  9  | VD       | Vertical sync detect                         |
 *  | 12  | CPBE1    | Bundle write end on CDAYR / CDACR            |
 *  | 13  | CPBE2    | Bundle write end on CDAYR2 / CDACR2          |
 *  | 14  | CPBE3    | Bundle write end on CDBYR / CDBCR            |
 *  | 15  | CPBE4    | Bundle write end on CDBYR2 / CDBCR2          |
 *  | 16  | CDTOF    | CRAM overflow                                |
 *  | 17  | IGHS     | HD-line-count mismatch vs CMCYR              |
 *  | 18  | IGVS     | VD-line-count mismatch vs CMCYR              |
 *  | 20  | VBP      | Invalid VD condition                         |
 *  | 23  | FWF      | Firewall address fault                       |
 *  | 24  | NHD      | HD expected but absent                       |
 *  | 25  | NVD      | VD expected but absent                       |
 */
typedef enum : uint32_t {
  k_ra8_ceu_evt_cpe           = (1UL << 0),  /**< CPE:   one-frame capture end.  */
  k_ra8_ceu_evt_cfe           = (1UL << 1),  /**< CFE:   one-field capture end.  */
  k_ra8_ceu_evt_igrw          = (1UL << 4),  /**< IGRW:  illegal register write. */
  k_ra8_ceu_evt_hd            = (1UL << 8),  /**< HD:    horizontal sync.        */
  k_ra8_ceu_evt_vd            = (1UL << 9),  /**< VD:    vertical sync.          */
  k_ra8_ceu_evt_cpbe1         = (1UL << 12), /**< CPBE1: bundle 1 write end.     */
  k_ra8_ceu_evt_cpbe2         = (1UL << 13), /**< CPBE2: bundle 2 write end.     */
  k_ra8_ceu_evt_cpbe3         = (1UL << 14), /**< CPBE3: bundle 3 write end.     */
  k_ra8_ceu_evt_cpbe4         = (1UL << 15), /**< CPBE4: bundle 4 write end.     */
  k_ra8_ceu_evt_cram_overflow = (1UL << 16), /**< CDTOF: CRAM overflow.          */
  k_ra8_ceu_evt_hd_mismatch   = (1UL << 17), /**< IGHS: HD line-count mismatch.  */
  k_ra8_ceu_evt_vd_mismatch   = (1UL << 18), /**< IGVS: VD line-count mismatch.  */
  k_ra8_ceu_evt_vd_error      = (1UL << 20), /**< VBP:  invalid VD condition.    */
  k_ra8_ceu_evt_firewall      = (1UL << 23), /**< FWF:  firewall-address fault.  */
  k_ra8_ceu_evt_hd_missing    = (1UL << 24), /**< NHD:  HD expected but absent.  */
  k_ra8_ceu_evt_vd_missing    = (1UL << 25), /**< NVD:  VD expected but absent.  */
} ra8_ceu_cetcr_mask_t;

/**
 * @enum ra8_ceu_evt_mask_group_t
 * @brief Convenience groupings of CETCR events for callbacks.
 *
 * @details
 * Hand-curated unions of `ra8_ceu_cetcr_mask_t` values that callers
 * tend to enable as a set. `k_ra8_ceu_evt_mask_all_errors` covers
 * every fault condition the HUM lists; `k_ra8_ceu_evt_mask_all`
 * is the universal mask used by `ra8_ceu_clear_status` callers
 * that want to ack every pending flag.
 */
typedef enum : uint32_t {
  k_ra8_ceu_evt_mask_all_errors =
    (k_ra8_ceu_evt_igrw | k_ra8_ceu_evt_cram_overflow | k_ra8_ceu_evt_hd_mismatch |
     k_ra8_ceu_evt_vd_mismatch | k_ra8_ceu_evt_vd_error | k_ra8_ceu_evt_firewall |
     k_ra8_ceu_evt_hd_missing | k_ra8_ceu_evt_vd_missing), /**< RA8 CEU evt mask all errors. */
  k_ra8_ceu_evt_mask_all_bundle = (k_ra8_ceu_evt_cpbe1 | k_ra8_ceu_evt_cpbe2 | k_ra8_ceu_evt_cpbe3 |
                                   k_ra8_ceu_evt_cpbe4), /**< RA8 CEU evt mask all bundle. */
  k_ra8_ceu_evt_mask_all_sync =
    (k_ra8_ceu_evt_hd | k_ra8_ceu_evt_vd), /**< RA8 CEU evt mask all sync. */
  k_ra8_ceu_evt_mask_all_end =
    (k_ra8_ceu_evt_cpe | k_ra8_ceu_evt_cfe), /**< RA8 CEU evt mask all end. */
  k_ra8_ceu_evt_mask_all = 0x03F7F713UL,     /**< RA8 CEU evt mask all.     */
} ra8_ceu_evt_mask_group_t;

/**
 * @enum ra8_ceu_cdssr_mask_t
 * @brief CDSSR (offset 0x0084) bit mask.
 *
 * @details
 * HUM Ch 60.2.24 "CDSSR : Capture Data Size Register" p 3674. The
 * full 32 bits hold the byte count written by the most recent
 * data-enable-fetch capture. In bundle-write mode the value is the
 * tail count after the last bundle boundary; if the boundary
 * coincides with the capture-end the register reads 0.
 */
typedef enum : uint32_t {
  k_ra8_ceu_cdssr_mask_cdss = 0xFFFFFFFFUL, /**< CDSS[31:0] data byte count. */
} ra8_ceu_cdssr_mask_t;

/**
 * @enum ra8_ceu_align_t
 * @brief Capture buffer alignment requirement.
 *
 * @details
 * The CDAYR / CDACR / CDBYR / CDBCR address registers require
 * their lower 3 bits to be zero (8-byte alignment) per HUM Ch
 * 60.2.13 "CDAYR" / 60.2.14 "CDACR" / 60.2.15 "CDBYR" /
 * 60.2.16 "CDBCR" p 3656-3658.
 */
typedef enum : uint32_t {
  k_ra8_ceu_buffer_align_bytes = 8U, /**< RA8 CEU buffer align bytes. */
} ra8_ceu_align_t;

/**
 * @enum ra8_ceu_burst_align_t
 * @brief CBDSR write-granularity alignment.
 *
 * @details
 * HUM Ch 60.2.17 p 3660 says CBDSR writes ignore the lower 3 bits.
 */
typedef enum : uint32_t {
  k_ra8_ceu_bundle_align_mask = 0x7U, /**< RA8 CEU bundle align mask. */
} ra8_ceu_burst_align_t;

/**
 * @enum ra8_ceu_firewall_align_t
 * @brief CFWCR.FWV alignment.
 *
 * @details
 * Per HUM Ch 60.2.18 p 3661 the firewall value is the upper 27
 * bits of an address; the lower 5 bits are forced to 0x1F. This
 * mask is used by `ra8_ceu_firewall_set` to compose the value.
 */
typedef enum : uint32_t {
  k_ra8_ceu_firewall_align_mask = 0x1FU, /**< RA8 CEU firewall align mask. */
} ra8_ceu_firewall_align_t;

/**
 * @brief Pointer to a 32-bit register inside the CEU window.
 *
 * @details
 * All CEU registers are 32 bits wide and accessed by 32-bit reads
 * and writes (HUM Ch 60.2 Table 60.4 "Access size" column = 32).
 *
 * @param[in] offset One of the `k_ra8_ceu_off_*` enum values.
 * @return Volatile pointer to the register at
 *         `k_ra8_ceu_base_addr + offset`.
 *
 * @pre `offset` is a member of `ra8_ceu_off_t` (compile-time
 *      enforced by the parameter type).
 * @pre The CEU module-stop bit (MSTPC16) is cleared before any
 *      access via this pointer.
 * @post Returned pointer is non-null and properly aligned for a
 *       32-bit access.
 *
 * @note Routes through `ra8_fake_mmap` on host test builds so that
 *       writes target real RAM rather than segfaulting.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_ceu_reg32(ra8_ceu_off_t offset)
{
  return (volatile uint32_t*)(k_ra8_ceu_base_addr + (uint16_t)offset);
}

/**
 * @brief Pointer to a 32-bit register on a specific CEU plane.
 *
 * @details
 * Most CEU registers expose a 3-plane layout per HUM Ch 60.2 Table
 * 60.4 p 3629-3630. This helper composes
 * `k_ra8_ceu_base_addr + plane + offset` so callers can read/write
 * the Plane B view (`+0x1000`) or the mirror (`+0x2000`) without
 * doing the arithmetic themselves.
 *
 * @param[in] offset Plane-A offset of the target register.
 * @param[in] plane  Which plane to address.
 * @return Volatile pointer to the register on the requested plane.
 *
 * @pre `offset` is a member of `ra8_ceu_off_t`.
 * @pre `plane` is a member of `ra8_ceu_plane_off_t`.
 * @pre The target register actually has a plane B / mirror view
 *      (caller checked HUM Table 60.4 -- the helper does NOT
 *      validate this).
 * @post Returned pointer is non-null and 32-bit aligned.
 *
 * @note Single-plane registers (CAPSR, CAPCR, CAMCR, CMCYR, CAIFR,
 *       CRCNTR, CRCMPR, CFWCR, CEIER, CETCR, CSTSR, CDSSR) ignore
 *       the +0x1000 / +0x2000 offsets at hardware level; addresses
 *       outside the 3-plane subset behave as ordinary RAM on host
 *       tests via `ra8_fake_mmap`.
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_ceu_reg32_plane(ra8_ceu_off_t       offset,
                                                     ra8_ceu_plane_off_t plane)
{
  return (volatile uint32_t*)(k_ra8_ceu_base_addr + (uint16_t)plane + (uint16_t)offset);
}

#ifdef __cplusplus
}
#endif
