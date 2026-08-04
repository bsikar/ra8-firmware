/**
 * @file ra8_i3c_i2c_regs.h
 * @brief IIC_B (I3C-based I2C) register layout for the Renesas RA8D2
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * One I3C channel at base ``0x4035_F000`` -- the unified I2C/I3C
 * peripheral. In I2C-only mode this is what the RA8D2 HUM calls the
 * "IIC_B" controller driver and what FSP exports as ``r_iic_b_master``.
 *
 * This header is hand-derived from the RA8D2 Hardware User's Manual
 * Chapter 40 "I3C Bus Interface (I3C)" register descriptions
 * (HUM Ch 40.2, p 2452-2701) and cross-checked against the FSP RA8D1
 * CMSIS device header ``R7FA8D1BH.h`` (the closest-cousin part with a
 * matching peripheral block).
 *
 * Only the registers we exercise from the IIC_B controller polling driver
 * are typed here -- the full I3C dynamic-address table, IBI queue,
 * and high-priority command queue are stubbed as ``volatile uint32_t``
 * holes so the struct still has the correct overall size.
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
 * @enum ra8_i3c_i2c_addr_t
 * @brief Hardware base addresses for the I3C / IIC_B block.
 *
 * @details
 * Single channel on the RA8D2; per HUM Ch 40.2 "I3C Bus Interface
 * Register Descriptions" the secure alias lives at the corresponding
 * NS+0x10000000 offset but the HAL only ever programmes the
 * S/non-aliased view.
 */
typedef enum : uintptr_t {
  k_ra8_i3c_i2c0_base_addr = 0x4035F000UL, /**< HUM Ch 40.2, p 2452 */
} ra8_i3c_i2c_addr_t;

/**
 * @enum ra8_i3c_i2c_limits_t
 * @brief Channel-count limits.
 */
typedef enum : uint16_t {
  k_ra8_i3c_i2c_channel_count = 1U, /**< HUM Ch 40.1.1, p 2445 */
} ra8_i3c_i2c_limits_t;

/**
 * @enum ra8_i3c_i2c_rsv_words_t
 * @brief Reserved-word array sizes (in 32-bit words) used inside the
 *        IIC_B / I3C register block.
 *
 * @details
 * Names each reserved gap so ``r_i3c_i2c_regs_t`` does not depend on
 * raw integer literals. Cross-checked against HUM Ch 40.2 offset
 * table.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_rsv_words_svtdlg_to_stctl    = 23U, /**< +0x00C4..0x011F gap. */
  k_ra8_i3c_i2c_rsv_words_ntdtbp0_to_nibiqp  = 8U,  /**< +0x015C..0x017B gap. */
  k_ra8_i3c_i2c_rsv_words_ntbthctl_to_nrqthc = 10U, /**< +0x0198..0x01BF gap. */
} ra8_i3c_i2c_rsv_words_t;

/**
 * @struct r_i3c_i2c_regs_t
 * @brief Memory-mapped register block for one IIC_B / I3C channel.
 *
 * @details
 * Layout matches HUM Ch 40.2 "I3C Bus Interface Register Descriptions"
 * (p 2452-2614). All registers are 32-bit; bit fields are accessed
 * via the ``k_ra8_i3c_i2c_*_pos`` and ``k_ra8_i3c_i2c_*_msk`` enums below.
 *
 * Fields prefixed ``RESERVED`` are holes per the HUM and must not
 * be touched.
 */
typedef struct {
  volatile uint32_t PRTS;          /**< +0x000 Protocol Selection (HUM 40.2, p 2452).             */
  volatile uint32_t RESERVED0[3];  /**< Reserved.                                                 */
  volatile uint32_t CECTL;         /**< +0x010 Clock Enable Control (HUM 40.2, p 2453).           */
  volatile uint32_t BCTL;          /**< +0x014 Bus Control (HUM 40.2, p 2454).                    */
  volatile uint32_t MSDVAD;        /**< +0x018 Controller Device Address (HUM 40.2, p 2455).      */
  volatile uint32_t RESERVED1;     /**< Reserved.                                                 */
  volatile uint32_t RSTCTL;        /**< +0x020 Reset Control (HUM 40.2, p 2456).                  */
  volatile uint32_t PRSST;         /**< +0x024 Present State (HUM 40.2, p 2457).                  */
  volatile uint32_t RESERVED2[2];  /**< Reserved.                                                 */
  volatile uint32_t INST;          /**< +0x030 Internal Status (HUM 40.2, p 2458).                */
  volatile uint32_t INSTE;         /**< +0x034 Internal Status Enable (HUM 40.2, p 2459).         */
  volatile uint32_t INIE;          /**< +0x038 Internal Interrupt Enable (HUM 40.2, p 2460).      */
  volatile uint32_t INSTFC;        /**< +0x03C Internal Status Force (HUM 40.2, p 2460).          */
  volatile uint32_t RESERVED3;     /**< Reserved.                                                 */
  volatile uint32_t DVCT;          /**< +0x044 Device Characteristic Table (HUM 40.2, p 2460).    */
  volatile uint32_t RESERVED4[4];  /**< Reserved.                                                 */
  volatile uint32_t IBINCTL;       /**< +0x058 IBI Notify Control (HUM 40.2, p 2460).             */
  volatile uint32_t RESERVED5;     /**< Reserved.                                                 */
  volatile uint32_t BFCTL;         /**< +0x060 Bus Function Control (HUM 40.2, p 2461).           */
  volatile uint32_t SVCTL;         /**< +0x064 Peripheral Control (HUM 40.2, p 2461).             */
  volatile uint32_t RESERVED6[2];  /**< Reserved.                                                 */
  volatile uint32_t REFCKCTL;      /**< +0x070 Reference Clock Control (HUM 40.2, p 2462).        */
  volatile uint32_t STDBR;         /**< +0x074 Standard Bit Rate (HUM 40.2, p 2462).              */
  volatile uint32_t EXTBR;         /**< +0x078 Extended Bit Rate (HUM 40.2, p 2463).              */
  volatile uint32_t BFRECDT;       /**< +0x07C Bus Free Cond Detect Time (HUM 40.2, p 2463).      */
  volatile uint32_t BAVLCDT;       /**< +0x080 Bus Available Cond Detect Time (HUM 40.2, p 2464). */
  volatile uint32_t BIDLCDT;       /**< +0x084 Bus Idle Cond Detect Time (HUM 40.2, p 2464).      */
  volatile uint32_t OUTCTL;        /**< +0x088 Output Control (HUM 40.2, p 2465).                 */
  volatile uint32_t INCTL;         /**< +0x08C Input Control (HUM 40.2, p 2466).                  */
  volatile uint32_t TMOCTL;        /**< +0x090 Timeout Control (HUM 40.2, p 2466).                */
  volatile uint32_t RESERVED7;     /**< Reserved.                                                 */
  volatile uint32_t WUCTL;         /**< +0x098 Wake Up Unit Control (HUM 40.2, p 2467).           */
  volatile uint32_t RESERVED8;     /**< Reserved.                                                 */
  volatile uint32_t ACKCTL;        /**< +0x0A0 Acknowledge Control (HUM 40.2, p 2468).            */
  volatile uint32_t SCSTRCTL;      /**< +0x0A4 SCL Stretch Control (HUM 40.2, p 2469).            */
  volatile uint32_t RESERVED9[2];  /**< Reserved.                                                 */
  volatile uint32_t SCSTLCTL;      /**< +0x0B0 SCL Stalling Control (HUM 40.2, p 2470).           */
  volatile uint32_t RESERVED10[3]; /**< Reserved.                                                 */
  volatile uint32_t SVTDLG0;       /**< +0x0C0 Peripheral Xfer Data Length 0 (HUM 40.2, p 2470).  */
  /** Reserved. */
  volatile uint32_t RESERVED11[k_ra8_i3c_i2c_rsv_words_svtdlg_to_stctl];
  volatile uint32_t STCTL;         /**< +0x120 Synchronous Timing Control (HUM 40.2, p 2471).  */
  volatile uint32_t ATCTL;         /**< +0x124 Asynchronous Timing Control (HUM 40.2, p 2472). */
  volatile uint32_t ATTRG;         /**< +0x128 Asynchronous Timing Trigger (HUM 40.2, p 2472). */
  volatile uint32_t ATCCNTE;       /**< +0x12C Async Timing Counter Enable (HUM 40.2, p 2472). */
  volatile uint32_t RESERVED12[4]; /**< Reserved.                                              */
  volatile uint32_t CNDCTL;        /**< +0x140 Condition Control (HUM 40.2, p 2473).           */
  volatile uint32_t RESERVED13[3]; /**< Reserved.                                              */
  volatile uint32_t NCMDQP;        /**< +0x150 Normal Command Queue Port (HUM 40.2, p 2473).   */
  volatile uint32_t NRSPQP;        /**< +0x154 Normal Response Queue Port (HUM 40.2, p 2475).  */
  volatile uint32_t NTDTBP0;       /**< +0x158 Normal Tx/Rx Data Buffer 0 (HUM 40.2, p 2476).  */
  /** Reserved. */
  volatile uint32_t RESERVED14[k_ra8_i3c_i2c_rsv_words_ntdtbp0_to_nibiqp];
  volatile uint32_t NIBIQP;    /**< +0x17C Normal IBI Queue Port (HUM 40.2, p 2477).        */
  volatile uint32_t NRSQP;     /**< +0x180 Normal Receive Status Queue (HUM 40.2, p 2477).  */
  volatile uint32_t HCMDQP;    /**< +0x184 High Priority Cmd Queue Port (HUM 40.2, p 2478). */
  volatile uint32_t HRSPQP;    /**< +0x188 High Priority Resp Queue (HUM 40.2, p 2478).     */
  volatile uint32_t HTDTBP;    /**< +0x18C High Priority Tx Data Buffer (HUM 40.2, p 2478). */
  volatile uint32_t NQTHCTL;   /**< +0x190 Normal Queue Threshold Ctrl (HUM 40.2, p 2479).  */
  volatile uint32_t NTBTHCTL0; /**< +0x194 Normal Tx/Rx Threshold 0 (HUM 40.2, p 2480).     */
  /** Reserved. */
  volatile uint32_t RESERVED15[k_ra8_i3c_i2c_rsv_words_ntbthctl_to_nrqthc];
  volatile uint32_t NRQTHCTL;      /**< +0x1C0 Normal RxStatus Threshold (HUM 40.2, p 2481).    */
  volatile uint32_t HQTHCTL;       /**< +0x1C4 High Priority Q Threshold (HUM 40.2, p 2481).    */
  volatile uint32_t HTBTHCTL;      /**< +0x1C8 High Priority Tx Threshold (HUM 40.2, p 2482).   */
  volatile uint32_t RESERVED16;    /**< Reserved.                                               */
  volatile uint32_t BST;           /**< +0x1D0 Bus Status (HUM 40.2, p 2482).                   */
  volatile uint32_t BSTE;          /**< +0x1D4 Bus Status Enable (HUM 40.2, p 2483).            */
  volatile uint32_t BIE;           /**< +0x1D8 Bus Interrupt Enable (HUM 40.2, p 2484).         */
  volatile uint32_t BSTFC;         /**< +0x1DC Bus Status Force (HUM 40.2, p 2485).             */
  volatile uint32_t NTST;          /**< +0x1E0 Normal Transfer Status (HUM 40.2, p 2486).       */
  volatile uint32_t NTSTE;         /**< +0x1E4 Normal Xfer Status Enable (HUM 40.2, p 2487).    */
  volatile uint32_t NTIE;          /**< +0x1E8 Normal Xfer Interrupt Enable (HUM 40.2, p 2488). */
  volatile uint32_t NTSTFC;        /**< +0x1EC Normal Xfer Status Force (HUM 40.2, p 2489).     */
  volatile uint32_t RESERVED17[4]; /**< Reserved.                                               */
  volatile uint32_t HTST;          /**< +0x200 High Priority Xfer Status (HUM 40.2, p 2489).    */
  volatile uint32_t HTSTE;         /**< +0x204 High Priority Xfer St En (HUM 40.2, p 2490).     */
  volatile uint32_t HTIE;          /**< +0x208 High Priority Xfer Int En (HUM 40.2, p 2490).    */
  volatile uint32_t HTSTFC;        /**< +0x20C High Priority Xfer St Force (HUM 40.2, p 2491).  */
  volatile uint32_t BCST;          /**< +0x210 Bus Condition Status (HUM 40.2, p 2491).         */
} r_i3c_i2c_regs_t;

/* =============================================================================
 * Bit positions and masks for the registers actually programmed by the
 * polling-mode IIC_B controller driver. Names mirror the FSP CMSIS field
 * macros (R_I3C0_xxx_yyy_Pos / _Msk) -- HUM Ch 40.2, p 2452..2491.
 * =============================================================================
 */

/**
 * @enum ra8_i3c_i2c_prts_t
 * @brief PRTS field positions (HUM Ch 40.2.1 "PRTS : Protocol Selection"
 *        p 2449).
 *
 * @details
 * PRTMD = 0 selects I3C FIFO-buffer transfer (the post-reset default).
 * PRTMD = 1 selects I2C single-buffer transfer. The polling driver in
 * ``ra8_i3c_i2c.c`` only knows how to drive the I2C single-buffer mode
 * (it writes NTDTBP0 directly without composing an I3C command queue
 * entry), so it must set PRTMD = 1 during init.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_prts_prtmd_pos = 0U, /**< HUM 40.2.1 PRTS.PRTMD, p 2449 */
} ra8_i3c_i2c_prts_t;

/**
 * @enum ra8_i3c_i2c_cectl_t
 * @brief CECTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_cectl_clke_pos = 0U, /**< HUM 40.2 CECTL.CLKE, p 2453 */
} ra8_i3c_i2c_cectl_t;

/**
 * @enum ra8_i3c_i2c_bctl_t
 * @brief BCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_bctl_buse_pos = 31U, /**< HUM 40.2 BCTL.BUSE, p 2454 */
} ra8_i3c_i2c_bctl_t;

/**
 * @enum ra8_i3c_i2c_rstctl_t
 * @brief RSTCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_rstctl_ri3crst_pos = 0U, /**< HUM 40.2 RSTCTL.RI3CRST, p 2456 */
} ra8_i3c_i2c_rstctl_t;

/**
 * @enum ra8_i3c_i2c_prsst_t
 * @brief PRSST field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_prsst_crms_pos = 2U, /**< HUM 40.2 PRSST.CRMS, p 2457 */
  k_ra8_i3c_i2c_prsst_trmd_pos = 4U, /**< HUM 40.2 PRSST.TRMD, p 2457 */
} ra8_i3c_i2c_prsst_t;

/**
 * @enum ra8_i3c_i2c_bfctl_t
 * @brief BFCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_bfctl_male_pos   = 0U,  /**< HUM 40.2 BFCTL.MALE, p 2461   */
  k_ra8_i3c_i2c_bfctl_nale_pos   = 1U,  /**< HUM 40.2 BFCTL.NALE, p 2461   */
  k_ra8_i3c_i2c_bfctl_scsyne_pos = 8U,  /**< HUM 40.2 BFCTL.SCSYNE, p 2461 */
  k_ra8_i3c_i2c_bfctl_smbs_pos   = 12U, /**< HUM 40.2 BFCTL.SMBS, p 2461   */
  k_ra8_i3c_i2c_bfctl_fmpe_pos   = 14U, /**< HUM 40.2 BFCTL.FMPE, p 2461   */
} ra8_i3c_i2c_bfctl_t;

/**
 * @enum ra8_i3c_i2c_stdbr_t
 * @brief STDBR field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_stdbr_sbrlo_pos = 0U, /**< HUM 40.2 STDBR.SBRLO, p 2462 */
  k_ra8_i3c_i2c_stdbr_sbrho_pos = 8U, /**< HUM 40.2 STDBR.SBRHO, p 2462 */
} ra8_i3c_i2c_stdbr_t;

/**
 * @enum ra8_i3c_i2c_ackctl_t
 * @brief ACKCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_ackctl_ackt_pos   = 1U, /**< HUM 40.2 ACKCTL.ACKT, p 2468   */
  k_ra8_i3c_i2c_ackctl_acktwp_pos = 2U, /**< HUM 40.2 ACKCTL.ACKTWP, p 2468 */
} ra8_i3c_i2c_ackctl_t;

/**
 * @enum ra8_i3c_i2c_scstrctl_t
 * @brief SCSTRCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_scstrctl_rwe_pos = 1U, /**< HUM 40.2 SCSTRCTL.RWE, p 2469 */
} ra8_i3c_i2c_scstrctl_t;

/**
 * @enum ra8_i3c_i2c_cndctl_t
 * @brief CNDCTL field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_cndctl_stcnd_pos = 0U, /**< HUM 40.2 CNDCTL.STCND, p 2473 */
  k_ra8_i3c_i2c_cndctl_srcnd_pos = 1U, /**< HUM 40.2 CNDCTL.SRCND, p 2473 */
  k_ra8_i3c_i2c_cndctl_spcnd_pos = 2U, /**< HUM 40.2 CNDCTL.SPCND, p 2473 */
} ra8_i3c_i2c_cndctl_t;

/**
 * @enum ra8_i3c_i2c_bst_t
 * @brief BST (Bus Status) field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_bst_stcnddf_pos = 0U,  /**< HUM 40.2 BST.STCNDDF, p 2482 */
  k_ra8_i3c_i2c_bst_spcnddf_pos = 1U,  /**< HUM 40.2 BST.SPCNDDF, p 2482 */
  k_ra8_i3c_i2c_bst_nackdf_pos  = 4U,  /**< HUM 40.2 BST.NACKDF,  p 2482 */
  k_ra8_i3c_i2c_bst_tendf_pos   = 8U,  /**< HUM 40.2 BST.TENDF,   p 2482 */
  k_ra8_i3c_i2c_bst_alf_pos     = 16U, /**< HUM 40.2 BST.ALF,     p 2482 */
  k_ra8_i3c_i2c_bst_todf_pos    = 20U, /**< HUM 40.2 BST.TODF,    p 2482 */
} ra8_i3c_i2c_bst_t;

/**
 * @enum ra8_i3c_i2c_ntst_t
 * @brief NTST (Normal Transfer Status) field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_ntst_tdbef0_pos = 0U, /**< HUM 40.2 NTST.TDBEF0, p 2486 */
  k_ra8_i3c_i2c_ntst_rdbff0_pos = 1U, /**< HUM 40.2 NTST.RDBFF0, p 2486 */
} ra8_i3c_i2c_ntst_t;

/**
 * @enum ra8_i3c_i2c_ntie_t
 * @brief NTIE (Normal Transfer Interrupt Enable) field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_ntie_tdbeie0_pos = 0U, /**< HUM 40.2 NTIE.TDBEIE0, p 2488 */
  k_ra8_i3c_i2c_ntie_rdbfie0_pos = 1U, /**< HUM 40.2 NTIE.RDBFIE0, p 2488 */
} ra8_i3c_i2c_ntie_t;

/**
 * @enum ra8_i3c_i2c_bie_t
 * @brief BIE (Bus Interrupt Enable) field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_bie_nackdie_pos = 4U,  /**< HUM 40.2 BIE.NACKDIE, p 2484 */
  k_ra8_i3c_i2c_bie_tendie_pos  = 8U,  /**< HUM 40.2 BIE.TENDIE,  p 2484 */
  k_ra8_i3c_i2c_bie_alie_pos    = 16U, /**< HUM 40.2 BIE.ALIE,    p 2484 */
  k_ra8_i3c_i2c_bie_todie_pos   = 20U, /**< HUM 40.2 BIE.TODIE,   p 2484 */
} ra8_i3c_i2c_bie_t;

/**
 * @enum ra8_i3c_i2c_bcst_t
 * @brief BCST (Bus Condition Status) field positions.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_bcst_bfref_pos = 0U, /**< HUM 40.2 BCST.BFREF, p 2491 */
} ra8_i3c_i2c_bcst_t;

/* =============================================================================
 * Composite masks built from the bit-position enums above. Keeping the
 * masks as a typed enum (uint32_t) lets callers OR them together
 * without per-call shift expressions.
 * =============================================================================
 */

/**
 * @enum ra8_i3c_i2c_mask_t
 * @brief Composite bit masks used by the polling driver.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_msk_prts_prtmd     = (uint32_t)(1U << 0U),  /**< HUM 40.2.1 PRTS.PRTMD, p 2449   */
  k_ra8_i3c_i2c_msk_cectl_clke     = (uint32_t)(1U << 0U),  /**< HUM 40.2 CECTL.CLKE, p 2453     */
  k_ra8_i3c_i2c_msk_bctl_buse      = (uint32_t)(1U << 31U), /**< HUM 40.2 BCTL.BUSE, p 2454      */
  k_ra8_i3c_i2c_msk_rstctl_ri3crst = (uint32_t)(1U << 0U),  /**< HUM 40.2 RSTCTL.RI3CRST, p 2456 */
  k_ra8_i3c_i2c_msk_bfctl_male     = (uint32_t)(1U << 0U),  /**< HUM 40.2 BFCTL.MALE,   p 2461   */
  k_ra8_i3c_i2c_msk_bfctl_nale     = (uint32_t)(1U << 1U),  /**< HUM 40.2 BFCTL.NALE,   p 2461   */
  k_ra8_i3c_i2c_msk_bfctl_scsyne   = (uint32_t)(1U << 8U),  /**< HUM 40.2 BFCTL.SCSYNE, p 2461   */
  k_ra8_i3c_i2c_msk_bfctl_fmpe     = (uint32_t)(1U << 14U), /**< HUM 40.2 BFCTL.FMPE,   p 2461   */
  k_ra8_i3c_i2c_msk_ackctl_ackt    = (uint32_t)(1U << 1U),  /**< HUM 40.2 ACKCTL.ACKT,  p 2468   */
  k_ra8_i3c_i2c_msk_ackctl_acktwp  = (uint32_t)(1U << 2U),  /**< HUM 40.2 ACKCTL.ACKTWP,p 2468   */
  k_ra8_i3c_i2c_msk_scstrctl_rwe   = (uint32_t)(1U << 1U),  /**< HUM 40.2 SCSTRCTL.RWE, p 2469   */
  k_ra8_i3c_i2c_msk_cndctl_stcnd   = (uint32_t)(1U << 0U),  /**< HUM 40.2 CNDCTL.STCND, p 2473   */
  k_ra8_i3c_i2c_msk_cndctl_srcnd   = (uint32_t)(1U << 1U),  /**< HUM 40.2 CNDCTL.SRCND, p 2473   */
  k_ra8_i3c_i2c_msk_cndctl_spcnd   = (uint32_t)(1U << 2U),  /**< HUM 40.2 CNDCTL.SPCND, p 2473   */
  k_ra8_i3c_i2c_msk_bst_stcnddf    = (uint32_t)(1U << 0U),  /**< HUM 40.2 BST.STCNDDF,  p 2482   */
  k_ra8_i3c_i2c_msk_bst_spcnddf    = (uint32_t)(1U << 1U),  /**< HUM 40.2 BST.SPCNDDF,  p 2482   */
  k_ra8_i3c_i2c_msk_bst_nackdf     = (uint32_t)(1U << 4U),  /**< HUM 40.2 BST.NACKDF,   p 2482   */
  k_ra8_i3c_i2c_msk_bst_tendf      = (uint32_t)(1U << 8U),  /**< HUM 40.2 BST.TENDF,    p 2482   */
  k_ra8_i3c_i2c_msk_bst_alf        = (uint32_t)(1U << 16U), /**< HUM 40.2 BST.ALF,      p 2482   */
  k_ra8_i3c_i2c_msk_bst_todf       = (uint32_t)(1U << 20U), /**< HUM 40.2 BST.TODF,     p 2482   */
  k_ra8_i3c_i2c_msk_ntst_tdbef0    = (uint32_t)(1U << 0U),  /**< HUM 40.2 NTST.TDBEF0,  p 2486   */
  k_ra8_i3c_i2c_msk_ntst_rdbff0    = (uint32_t)(1U << 1U),  /**< HUM 40.2 NTST.RDBFF0,  p 2486   */
  k_ra8_i3c_i2c_msk_ntie_tdbeie0   = (uint32_t)(1U << 0U),  /**< HUM 40.2 NTIE.TDBEIE0, p 2488   */
  k_ra8_i3c_i2c_msk_ntie_rdbfie0   = (uint32_t)(1U << 1U),  /**< HUM 40.2 NTIE.RDBFIE0, p 2488   */
  k_ra8_i3c_i2c_msk_bie_nackdie    = (uint32_t)(1U << 4U),  /**< HUM 40.2 BIE.NACKDIE,  p 2484   */
  k_ra8_i3c_i2c_msk_bie_tendie     = (uint32_t)(1U << 8U),  /**< HUM 40.2 BIE.TENDIE,   p 2484   */
  k_ra8_i3c_i2c_msk_bie_alie       = (uint32_t)(1U << 16U), /**< HUM 40.2 BIE.ALIE,     p 2484   */
  k_ra8_i3c_i2c_msk_bie_todie      = (uint32_t)(1U << 20U), /**< HUM 40.2 BIE.TODIE,    p 2484   */
  k_ra8_i3c_i2c_msk_bcst_bfref     = (uint32_t)(1U << 0U),  /**< HUM 40.2 BCST.BFREF,   p 2491   */
} ra8_i3c_i2c_mask_t;

/**
 * @brief Get a pointer to the IIC_B / I3C channel ``channel``.
 *
 * @param[in] channel Channel index. Only ``0`` is valid on the RA8D2.
 *
 * @return ``volatile r_i3c_i2c_regs_t*`` Pointer to channel registers,
 *         or ``nullptr`` when ``channel`` is out of range.
 *
 * @pre ``ra8_mstp_enable(k_ra8_mstp_i3c)`` has been called.
 * @post Returned pointer either is ``nullptr`` or aliases
 *       ``k_ra8_i3c_i2c0_base_addr``.
 *
 * @note Inline -- expands at the call site so the compiler can fold
 *       the channel-bounds check when called with a literal ``0``.
 *
 * @see HUM Ch 40.2 "I3C Bus Interface Register Descriptions", p 2452.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_i3c_i2c_regs_t* i3c_i2c_regs(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_i3c_i2c_channel_count) {
    return nullptr;
  }
  return (volatile r_i3c_i2c_regs_t*)k_ra8_i3c_i2c0_base_addr;
}

#ifdef __cplusplus
}
#endif
