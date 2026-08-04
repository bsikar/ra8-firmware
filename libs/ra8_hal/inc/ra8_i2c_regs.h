/**
 * @file ra8_i2c_regs.h
 * @brief I2C Bus Interface (IIC) register layout for the Renesas RA8D2
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Three RIIC channels (IIC0/IIC1/IIC2) at base
 * ``0x4025_E000 + 0x0100 * n`` -- this is the classic Renesas RIIC
 * register block (ICCR1, ICCR2, ICMR1..3, ICFER, ICSER, ICIER,
 * ICSR1/2, SARLy/SARUy, ICBRL/ICBRH, ICDRT, ICDRR). It is the
 * peripheral the RA8D2 HUM calls "I2C Bus Interface (IIC)" and that
 * FSP drives with ``r_iic_master`` -- distinct from the I3C unified IP
 * (``ra8_i3c`` / ``ra8_i3c_i2c``) at ``0x4035_F000``.
 *
 * This header is hand-derived from the RA8D2 Hardware User's Manual
 * Chapter 39 "I2C Bus Interface (IIC)" register descriptions
 * (HUM Ch 39.2, p 2369-2394). Every RIIC register is a single byte;
 * the registers are laid out contiguously from offset 0x00.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_i2c_addr_t
 * @brief Hardware base addresses for the IIC channels.
 *
 * @details
 * Per HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 the
 * channel base is ``IICn = 0x4025_E000 + 0x0100 * n`` (n = 0..2). The
 * HAL only ever programmes the S/non-aliased view; the NS alias lives
 * at ``0x5025_E000 + 0x0100 * n``.
 *
 * @invariant ``k_ra8_i2c_channel_stride`` separates consecutive channels.
 */
typedef enum : uintptr_t {
  k_ra8_i2c0_base_addr     = 0x4025E000UL, /**< HUM Ch 39.2.1, p 2369 */
  k_ra8_i2c_channel_stride = 0x00000100UL, /**< HUM Ch 39.2.1, p 2369 */
} ra8_i2c_addr_t;

/**
 * @enum ra8_i2c_limits_t
 * @brief Channel-count limits.
 */
typedef enum : uint16_t {
  k_ra8_i2c_channel_count    = 3U,  /**< HUM Ch 39.1, p 2367 (3 channels)  */
  k_ra8_i2c_regs_block_bytes = 22U, /**< sizeof(r_i2c_regs_t): 0x00..0x15. */
} ra8_i2c_limits_t;

/**
 * @struct r_i2c_regs_t
 * @brief Memory-mapped register block for one IIC channel.
 *
 * @details
 * Layout matches HUM Ch 39.2 "Register Descriptions" (p 2369-2394).
 * Every RIIC register is a single byte. The wakeup-unit registers
 * (ICWUR / ICWUR2 at +0x14 / +0x15, IIC0 only) are reserved holes
 * here -- the controller-mode polling driver does not touch them.
 *
 * @invariant ``sizeof(r_i2c_regs_t)`` is 0x16 bytes, well within the
 *            0x100 channel stride.
 */
typedef struct {
  volatile uint8_t ICCR1;  /**< +0x00 Bus Control Register 1 (HUM 39.2.1, p 2369).  */
  volatile uint8_t ICCR2;  /**< +0x01 Bus Control Register 2 (HUM 39.2.2, p 2371).  */
  volatile uint8_t ICMR1;  /**< +0x02 Bus Mode Register 1 (HUM 39.2.3, p 2374).     */
  volatile uint8_t ICMR2;  /**< +0x03 Bus Mode Register 2 (HUM 39.2.4, p 2375).     */
  volatile uint8_t ICMR3;  /**< +0x04 Bus Mode Register 3 (HUM 39.2.5, p 2376).     */
  volatile uint8_t ICFER;  /**< +0x05 Bus Function Enable (HUM 39.2.6, p 2378).     */
  volatile uint8_t ICSER;  /**< +0x06 Bus Status Enable (HUM 39.2.7, p 2380).       */
  volatile uint8_t ICIER;  /**< +0x07 Bus Interrupt Enable (HUM 39.2.8, p 2381).    */
  volatile uint8_t ICSR1;  /**< +0x08 Bus Status Register 1 (HUM 39.2.9, p 2382).   */
  volatile uint8_t ICSR2;  /**< +0x09 Bus Status Register 2 (HUM 39.2.10, p 2384).  */
  volatile uint8_t SARL0;  /**< +0x0A Peripheral Address L0 (HUM 39.2.13, p 2390).  */
  volatile uint8_t SARU0;  /**< +0x0B Peripheral Address U0 (HUM 39.2.14, p 2391).  */
  volatile uint8_t SARL1;  /**< +0x0C Peripheral Address L1 (HUM 39.2.13, p 2390).  */
  volatile uint8_t SARU1;  /**< +0x0D Peripheral Address U1 (HUM 39.2.14, p 2391).  */
  volatile uint8_t SARL2;  /**< +0x0E Peripheral Address L2 (HUM 39.2.13, p 2390).  */
  volatile uint8_t SARU2;  /**< +0x0F Peripheral Address U2 (HUM 39.2.14, p 2391).  */
  volatile uint8_t ICBRL;  /**< +0x10 Bit Rate Low-Level (HUM 39.2.15, p 2391).     */
  volatile uint8_t ICBRH;  /**< +0x11 Bit Rate High-Level (HUM 39.2.16, p 2392).    */
  volatile uint8_t ICDRT;  /**< +0x12 Transmit Data Register (HUM 39.2.17, p 2393). */
  volatile uint8_t ICDRR;  /**< +0x13 Receive Data Register (HUM 39.2.18, p 2393).  */
  volatile uint8_t ICWUR;  /**< +0x14 Wakeup Unit Register (HUM 39.2.11, p 2388).   */
  volatile uint8_t ICWUR2; /**< +0x15 Wakeup Unit Register 2 (HUM 39.2.12, p 2389). */
} r_i2c_regs_t;

static_assert(sizeof(r_i2c_regs_t) == (size_t)k_ra8_i2c_regs_block_bytes,
              "RIIC register block must be 22 (0x16) bytes");

/* =============================================================================
 * Bit positions for the registers actually programmed by the polling-mode
 * IIC controller driver. Names mirror the HUM bit names -- HUM Ch 39.2,
 * p 2369..2394.
 * =============================================================================
 */

/**
 * @enum ra8_i2c_iccr1_t
 * @brief ICCR1 (Bus Control 1) field positions (HUM Ch 39.2.1, p 2369).
 */
typedef enum : uint8_t {
  k_ra8_i2c_iccr1_sdai_pos   = 0U, /**< SDA line monitor (R).         */
  k_ra8_i2c_iccr1_scli_pos   = 1U, /**< SCL line monitor (R).         */
  k_ra8_i2c_iccr1_sdao_pos   = 2U, /**< SDA output control/monitor.   */
  k_ra8_i2c_iccr1_sclo_pos   = 3U, /**< SCL output control/monitor.   */
  k_ra8_i2c_iccr1_sowp_pos   = 4U, /**< SCLO/SDAO write protect.      */
  k_ra8_i2c_iccr1_clo_pos    = 5U, /**< Extra SCL clock cycle output. */
  k_ra8_i2c_iccr1_iicrst_pos = 6U, /**< I2C internal reset.           */
  k_ra8_i2c_iccr1_ice_pos    = 7U, /**< I2C interface enable.         */
} ra8_i2c_iccr1_t;

/**
 * @enum ra8_i2c_iccr2_t
 * @brief ICCR2 (Bus Control 2) field positions (HUM Ch 39.2.2, p 2371).
 */
typedef enum : uint8_t {
  k_ra8_i2c_iccr2_st_pos   = 1U, /**< Start condition issuance request.   */
  k_ra8_i2c_iccr2_rs_pos   = 2U, /**< Restart condition issuance request. */
  k_ra8_i2c_iccr2_sp_pos   = 3U, /**< Stop condition issuance request.    */
  k_ra8_i2c_iccr2_trs_pos  = 5U, /**< Transmit/receive mode.              */
  k_ra8_i2c_iccr2_mst_pos  = 6U, /**< Controller/peripheral mode.         */
  k_ra8_i2c_iccr2_bbsy_pos = 7U, /**< Bus busy detection flag (R).        */
} ra8_i2c_iccr2_t;

/**
 * @enum ra8_i2c_icmr1_t
 * @brief ICMR1 (Bus Mode 1) field positions (HUM Ch 39.2.3, p 2374).
 */
typedef enum : uint8_t {
  k_ra8_i2c_icmr1_cks_pos  = 4U, /**< Internal reference clock select [6:4]. */
  k_ra8_i2c_icmr1_mtwp_pos = 7U, /**< MST/TRS write protect.                 */
} ra8_i2c_icmr1_t;

/**
 * @enum ra8_i2c_icmr3_t
 * @brief ICMR3 (Bus Mode 3) field positions (HUM Ch 39.2.5, p 2376).
 */
typedef enum : uint8_t {
  k_ra8_i2c_icmr3_ackbr_pos = 2U, /**< Receive acknowledge (R).        */
  k_ra8_i2c_icmr3_ackbt_pos = 3U, /**< Transmit acknowledge.           */
  k_ra8_i2c_icmr3_ackwp_pos = 4U, /**< ACKBT write protect.            */
  k_ra8_i2c_icmr3_rdrfs_pos = 5U, /**< RDRF flag set timing select.    */
  k_ra8_i2c_icmr3_wait_pos  = 6U, /**< Wait between 9th and 1st clock. */
  k_ra8_i2c_icmr3_smbs_pos  = 7U, /**< SMBus/I2C select.               */
} ra8_i2c_icmr3_t;

/**
 * @enum ra8_i2c_icfer_t
 * @brief ICFER (Bus Function Enable) field positions (HUM Ch 39.2.6, p 2378).
 */
typedef enum : uint8_t {
  k_ra8_i2c_icfer_tmoe_pos  = 0U, /**< Timeout function enable.                   */
  k_ra8_i2c_icfer_male_pos  = 1U, /**< Controller arbitration-lost detect enable. */
  k_ra8_i2c_icfer_nale_pos  = 2U, /**< NACK transmit arbitration-lost enable.     */
  k_ra8_i2c_icfer_sale_pos  = 3U, /**< Peripheral arbitration-lost enable.        */
  k_ra8_i2c_icfer_nacke_pos = 4U, /**< NACK reception transfer suspension enable. */
  k_ra8_i2c_icfer_nfe_pos   = 5U, /**< Digital noise filter enable.               */
  k_ra8_i2c_icfer_scle_pos  = 6U, /**< SCL synchronous circuit enable.            */
  k_ra8_i2c_icfer_fmpe_pos  = 7U, /**< Fast-mode plus enable.                     */
} ra8_i2c_icfer_t;

/**
 * @enum ra8_i2c_icier_t
 * @brief ICIER (Bus Interrupt Enable) field positions (HUM Ch 39.2.8, p 2381).
 */
typedef enum : uint8_t {
  k_ra8_i2c_icier_tmoie_pos = 0U, /**< Timeout interrupt enable.             */
  k_ra8_i2c_icier_alie_pos  = 1U, /**< Arbitration-lost interrupt enable.    */
  k_ra8_i2c_icier_stie_pos  = 2U, /**< Start condition interrupt enable.     */
  k_ra8_i2c_icier_spie_pos  = 3U, /**< Stop condition interrupt enable.      */
  k_ra8_i2c_icier_nakie_pos = 4U, /**< NACK reception interrupt enable.      */
  k_ra8_i2c_icier_rie_pos   = 5U, /**< Receive data full interrupt enable.   */
  k_ra8_i2c_icier_teie_pos  = 6U, /**< Transmit end interrupt enable.        */
  k_ra8_i2c_icier_tie_pos   = 7U, /**< Transmit data empty interrupt enable. */
} ra8_i2c_icier_t;

/**
 * @enum ra8_i2c_icsr2_t
 * @brief ICSR2 (Bus Status 2) field positions (HUM Ch 39.2.10, p 2384).
 */
typedef enum : uint8_t {
  k_ra8_i2c_icsr2_tmof_pos  = 0U, /**< Timeout detection flag.         */
  k_ra8_i2c_icsr2_al_pos    = 1U, /**< Arbitration-lost flag.          */
  k_ra8_i2c_icsr2_start_pos = 2U, /**< Start condition detection flag. */
  k_ra8_i2c_icsr2_stop_pos  = 3U, /**< Stop condition detection flag.  */
  k_ra8_i2c_icsr2_nackf_pos = 4U, /**< NACK detection flag.            */
  k_ra8_i2c_icsr2_rdrf_pos  = 5U, /**< Receive data full flag.         */
  k_ra8_i2c_icsr2_tend_pos  = 6U, /**< Transmit end flag.              */
  k_ra8_i2c_icsr2_tdre_pos  = 7U, /**< Transmit data empty flag.       */
} ra8_i2c_icsr2_t;

/**
 * @enum ra8_i2c_icser_t
 * @brief ICSER (Bus Status Enable) field positions (HUM Ch 39.2.7, p 2380).
 *
 * @details
 * Own-address-match enables used by the target (peripheral) role. Each
 * ``SARyE`` bit arms the matching ``SARLy`` / ``SARUy`` own-address slot, and
 * ``GCAE`` arms the I2C general-call address (``0000 000b + 0[W]``). Bits 4
 * and 6 are reserved. Each ``SARyE`` bit is this driver's inclusive
 * "own-address slot y" match-enable.
 */
typedef enum : uint8_t {
  k_ra8_i2c_icser_sar0e_pos = 0U, /**< Own-address slot 0 match enable. */
  k_ra8_i2c_icser_sar1e_pos = 1U, /**< Own-address slot 1 match enable. */
  k_ra8_i2c_icser_sar2e_pos = 2U, /**< Own-address slot 2 match enable. */
  k_ra8_i2c_icser_gcae_pos  = 3U, /**< General-call address enable.     */
  k_ra8_i2c_icser_dide_pos  = 5U, /**< Device-ID address detect enable. */
  k_ra8_i2c_icser_hoae_pos  = 7U, /**< Host-address enable.             */
} ra8_i2c_icser_t;

/**
 * @enum ra8_i2c_icsr1_t
 * @brief ICSR1 (Bus Status 1) field positions (HUM Ch 39.2.9, p 2382).
 *
 * @details
 * Own-address detection flags. After a controller issues a START and an
 * address that matches an enabled own-address slot, the hardware raises the
 * matching ``AASy`` flag (or ``GCA`` for the general-call address) on the
 * rising edge of the ninth SCL clock. These flags are cleared automatically
 * when the bus STOP condition is detected.
 */
typedef enum : uint8_t {
  k_ra8_i2c_icsr1_aas0_pos = 0U, /**< Own-address slot 0 detected.   */
  k_ra8_i2c_icsr1_aas1_pos = 1U, /**< Own-address slot 1 detected.   */
  k_ra8_i2c_icsr1_aas2_pos = 2U, /**< Own-address slot 2 detected.   */
  k_ra8_i2c_icsr1_gca_pos  = 3U, /**< General-call address detected. */
  k_ra8_i2c_icsr1_did_pos  = 5U, /**< Device-ID address detected.    */
  k_ra8_i2c_icsr1_hoa_pos  = 7U, /**< Host address detected.         */
} ra8_i2c_icsr1_t;

/**
 * @enum ra8_i2c_sarl_t
 * @brief SARLy (own-address lower) field positions (HUM Ch 39.2.13, p 2390).
 *
 * @details
 * In 7-bit format (``SARUy.FS`` = 0) the ``SVA[6:0]`` field at bit 1 holds
 * the whole 7-bit own address, so a 7-bit address is written left-shifted by
 * one. ``SVA0`` at bit 0 is the 10-bit LSB and is ignored in 7-bit format.
 */
typedef enum : uint8_t {
  k_ra8_i2c_sarl_sva0_pos = 0U, /**< 10-bit own-address LSB (SVA0).           */
  k_ra8_i2c_sarl_sva_pos  = 1U, /**< 7-bit own address / 10-bit low SVA[6:0]. */
} ra8_i2c_sarl_t;

/**
 * @enum ra8_i2c_saru_t
 * @brief SARUy (own-address upper) field positions (HUM Ch 39.2.14, p 2391).
 *
 * @details
 * ``FS`` selects 7-bit (0) or 10-bit (1) own-address format; ``SVA[1:0]`` at
 * bit 1 holds the two upper bits of a 10-bit own address. This polling-mode
 * target driver only programmes the 7-bit format, so SARUy is written as 0.
 */
typedef enum : uint8_t {
  k_ra8_i2c_saru_fs_pos  = 0U, /**< 7-bit (0) / 10-bit (1) format select. */
  k_ra8_i2c_saru_sva_pos = 1U, /**< 10-bit own-address upper SVA[1:0].    */
} ra8_i2c_saru_t;

/* =============================================================================
 * Composite masks. Keeping masks as a typed enum (uint8_t) lets callers OR
 * them together without per-call shift expressions.
 * =============================================================================
 */

/**
 * @enum ra8_i2c_mask_t
 * @brief Composite bit masks used by the polling driver (HUM Ch 39.2).
 */
typedef enum : uint8_t {
  k_ra8_i2c_msk_iccr1_sowp   = (uint8_t)(1U << 4U), /**< HUM 39.2.1 ICCR1.SOWP, p 2369   */
  k_ra8_i2c_msk_iccr1_iicrst = (uint8_t)(1U << 6U), /**< HUM 39.2.1 ICCR1.IICRST, p 2369 */
  k_ra8_i2c_msk_iccr1_ice    = (uint8_t)(1U << 7U), /**< HUM 39.2.1 ICCR1.ICE, p 2369    */
  k_ra8_i2c_msk_iccr2_st     = (uint8_t)(1U << 1U), /**< HUM 39.2.2 ICCR2.ST, p 2371     */
  k_ra8_i2c_msk_iccr2_rs     = (uint8_t)(1U << 2U), /**< HUM 39.2.2 ICCR2.RS, p 2371     */
  k_ra8_i2c_msk_iccr2_sp     = (uint8_t)(1U << 3U), /**< HUM 39.2.2 ICCR2.SP, p 2371     */
  k_ra8_i2c_msk_iccr2_trs    = (uint8_t)(1U << 5U), /**< HUM 39.2.2 ICCR2.TRS, p 2371    */
  k_ra8_i2c_msk_iccr2_mst    = (uint8_t)(1U << 6U), /**< HUM 39.2.2 ICCR2.MST, p 2371    */
  k_ra8_i2c_msk_iccr2_bbsy   = (uint8_t)(1U << 7U), /**< HUM 39.2.2 ICCR2.BBSY, p 2371   */
  k_ra8_i2c_msk_icmr3_ackbr  = (uint8_t)(1U << 2U), /**< HUM 39.2.5 ICMR3.ACKBR, p 2376  */
  k_ra8_i2c_msk_icmr3_ackbt  = (uint8_t)(1U << 3U), /**< HUM 39.2.5 ICMR3.ACKBT, p 2376  */
  k_ra8_i2c_msk_icmr3_ackwp  = (uint8_t)(1U << 4U), /**< HUM 39.2.5 ICMR3.ACKWP, p 2376  */
  k_ra8_i2c_msk_icmr3_wait   = (uint8_t)(1U << 6U), /**< HUM 39.2.5 ICMR3.WAIT, p 2376   */
  k_ra8_i2c_msk_icfer_male   = (uint8_t)(1U << 1U), /**< HUM 39.2.6 ICFER.MALE, p 2378   */
  k_ra8_i2c_msk_icfer_nacke  = (uint8_t)(1U << 4U), /**< HUM 39.2.6 ICFER.NACKE, p 2378  */
  k_ra8_i2c_msk_icfer_nfe    = (uint8_t)(1U << 5U), /**< HUM 39.2.6 ICFER.NFE, p 2378    */
  k_ra8_i2c_msk_icfer_scle   = (uint8_t)(1U << 6U), /**< HUM 39.2.6 ICFER.SCLE, p 2378   */
  k_ra8_i2c_msk_icfer_fmpe   = (uint8_t)(1U << 7U), /**< HUM 39.2.6 ICFER.FMPE, p 2378   */
  k_ra8_i2c_msk_icsr2_al     = (uint8_t)(1U << 1U), /**< HUM 39.2.10 ICSR2.AL, p 2384    */
  k_ra8_i2c_msk_icsr2_stop   = (uint8_t)(1U << 3U), /**< HUM 39.2.10 ICSR2.STOP, p 2384  */
  k_ra8_i2c_msk_icsr2_nackf  = (uint8_t)(1U << 4U), /**< HUM 39.2.10 ICSR2.NACKF, p 2384 */
  k_ra8_i2c_msk_icsr2_rdrf   = (uint8_t)(1U << 5U), /**< HUM 39.2.10 ICSR2.RDRF, p 2384  */
  k_ra8_i2c_msk_icsr2_tend   = (uint8_t)(1U << 6U), /**< HUM 39.2.10 ICSR2.TEND, p 2384  */
  k_ra8_i2c_msk_icsr2_tdre   = (uint8_t)(1U << 7U), /**< HUM 39.2.10 ICSR2.TDRE, p 2384  */
  k_ra8_i2c_msk_icmr1_mtwp   = (uint8_t)(1U << 7U), /**< HUM 39.2.3 ICMR1.MTWP, p 2374   */
} ra8_i2c_mask_t;

/**
 * @enum ra8_i2c_peripheral_mask_t
 * @brief Composite masks for the target (peripheral) role (HUM Ch 39.2).
 *
 * @details
 * Own-address enable bits (ICSER), own-address detection flags (ICSR1) and
 * the union of all match flags used to decide that the controller addressed
 * this device. Kept separate from ``ra8_i2c_mask_t`` so the controller-only
 * transfer plane is unaffected.
 */
typedef enum : uint8_t {
  k_ra8_i2c_msk_icser_sar0e = (uint8_t)(1U << 0U), /**< HUM 39.2.7 ICSER.SAR0E, p 2380 */
  k_ra8_i2c_msk_icser_sar1e = (uint8_t)(1U << 1U), /**< HUM 39.2.7 ICSER.SAR1E, p 2380 */
  k_ra8_i2c_msk_icser_sar2e = (uint8_t)(1U << 2U), /**< HUM 39.2.7 ICSER.SAR2E, p 2380 */
  k_ra8_i2c_msk_icser_gcae  = (uint8_t)(1U << 3U), /**< HUM 39.2.7 ICSER.GCAE, p 2380  */
  k_ra8_i2c_msk_icsr1_aas0  = (uint8_t)(1U << 0U), /**< HUM 39.2.9 ICSR1.AAS0, p 2382  */
  k_ra8_i2c_msk_icsr1_aas1  = (uint8_t)(1U << 1U), /**< HUM 39.2.9 ICSR1.AAS1, p 2382  */
  k_ra8_i2c_msk_icsr1_aas2  = (uint8_t)(1U << 2U), /**< HUM 39.2.9 ICSR1.AAS2, p 2382  */
  k_ra8_i2c_msk_icsr1_gca   = (uint8_t)(1U << 3U), /**< HUM 39.2.9 ICSR1.GCA, p 2382   */
  /** Union AAS0|AAS1|AAS2|GCA: any own-address/general-call match. */
  k_ra8_i2c_msk_icsr1_match = (uint8_t)((1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U)),
} ra8_i2c_peripheral_mask_t;

/**
 * @brief Get a pointer to the IIC channel ``channel`` register block.
 *
 * @param[in] channel Channel index. ``0``, ``1`` or ``2`` on the RA8D2.
 *
 * @return ``volatile r_i2c_regs_t*`` Pointer to channel registers, or
 *         ``nullptr`` when ``channel`` is out of range.
 *
 * @pre ``ra8_mstp_enable(k_ra8_mstp_iicN)`` has been called for the channel.
 * @post Returned pointer is ``nullptr`` or aliases
 *       ``k_ra8_i2c0_base_addr + channel * k_ra8_i2c_channel_stride``.
 *
 * @note Inline -- expands at the call site so the compiler can fold the
 *       channel-bounds check when called with a literal channel index.
 *
 * @see HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1", p 2369.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_i2c_regs_t* ra8_i2c_regs(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i2c_channel_count) {
    return nullptr;
  }
  const uintptr_t base =
    (uintptr_t)k_ra8_i2c0_base_addr + ((uintptr_t)channel * (uintptr_t)k_ra8_i2c_channel_stride);
  return (volatile r_i2c_regs_t*)base;
}

#ifdef __cplusplus
}
#endif
