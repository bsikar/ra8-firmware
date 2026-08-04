/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_canfd_regs.h
 * @brief CANFD controller register layout for the Renesas RA8D2
 * @ingroup grp_hal_comms
 *
 * @details
 * Two CANFD instances live at `0x40380000` (CANFD0) and `0x40382000`
 * (CANFD1). Each instance is a full RA8 CANFD block (NOT the smaller
 * "CANFD Lite" / "CANFD_B" found on RA4/RA6 parts). The actual
 * register layout is the one captured by the FSP CMSIS device file
 * `R7KA8D2KF_core0.h` `R_CANFD_Type` (size 0x1920) and matches the
 * field semantics in `r_canfd.c` (FSP).
 *
 * Layout summary (offsets relative to the channel base):
 *
 *  - 0x000 .. 0x00F : per-channel CFDC[1] = { NCFG, CTR, STS, ERFL }
 *  - 0x010          : reserved
 *  - 0x014 .. 0x0DC : global block (CFDGCFG, CFDGCTR, CFDGSTS,
 *                     CFDGERFL, CFDGTSC, CFDGAFLECTR, CFDGAFLCFG0,
 *                     CFDRMNB, CFDRMND0, CFDRMIEC, CFDRFCC[2],
 *                     CFDRFSTS[2], CFDRFPCTR[2], CFDCFCC[1],
 *                     CFDCFSTS[1], CFDCFPCTR[1], CFDFESTS, CFDFFSTS,
 *                     CFDFMSTS, CFDRFISTS, CFDTMC[4], CFDTMSTS[4],
 *                     CFDTMTRSTS[1], CFDTMTARSTS[1], CFDTMTCSTS[1],
 *                     CFDTMTASTS[1], CFDTMIEC[1], CFDTXQCC0[1],
 *                     CFDTXQSTS0[1], CFDTXQPCTR0[1], CFDTHLCC[1],
 *                     CFDTHLSTS[1], CFDTHLPCTR[1], CFDGTINTSTS0,
 *                     CFDGTSTCFG, CFDGTSTCTR, CFDGFDCFG, CFDGLOCKK,
 *                     CFDGAFLIGNENT, CFDGAFLIGNCTR, CFDCDTCT,
 *                     CFDCDTSTS, CFDGRSTC)
 *  - 0x100 .. 0x11F : per-channel CFDC2[1] = { DCFG, FDCFG, FDCTR,
 *                     FDSTS, FDCRC }
 *  - 0x120 .. 0x21F : CFDGAFL[16] acceptance-filter list page window
 *  - 0x280 .. 0x37F : CFDRPGACC[64] RAM-test page-access window
 *  - 0x520 .. 0x5B7 : CFDRF[2] RX FIFO access (ID/PTR/FDSTS/DF[64])
 *  - 0x5B8 .. 0x603 : CFDCF[1] common FIFO access
 *  - 0x604 .. 0x73F : CFDTM[4] TX message buffer access
 *  - 0x740 .. 0x747 : CFDTHL[1] TX history list
 *  - 0x920 .. 0x191F: CFDRM[4] RX message-buffer cluster
 *
 * Field shifts / masks below come from the FSP `R7KA8D2KF_core0.h`
 * bit-field unions and from HUM Ch 41 "CAN with Flexible Data-rate
 * (CANFD)" pages 2702..2867 (chapter map row 41).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_canfd0_base_addr = 0x40380000UL, /**< HUM Ch 41 p 2702 channel 0 base. */
  k_ra8_canfd1_base_addr = 0x40382000UL, /**< HUM Ch 41 p 2702 channel 1 base. */
} ra8_canfd_addr_t;

typedef enum : uint8_t {
  k_ra8_canfd_instance_count = 2U,  /**< Number of CANFD channels.         */
  k_ra8_canfd_rx_fifo_count  = 2U,  /**< CFDRFCC / CFDRFSTS array length.  */
  k_ra8_canfd_cf_count       = 1U,  /**< Common-FIFO array length.         */
  k_ra8_canfd_tx_mb_count    = 4U,  /**< CFDTM / CFDTMC / CFDTMSTS length. */
  k_ra8_canfd_afl_page_size  = 16U, /**< CFDGAFL[] page-window size.       */
  k_ra8_canfd_rm_per_cluster = 8U,  /**< RX MB entries per cluster.        */
  k_ra8_canfd_rm_clusters    = 4U,  /**< CFDRM cluster array length.       */
} ra8_canfd_limits_t;

/**
 * @enum ra8_canfd_frame_limits_t
 * @brief Message-frame limits common to the public API and register layer.
 */
typedef enum : uint8_t {
  k_ra8_canfd_data_bytes_max = 64U, /**< CAN-FD payload cap in bytes.    */
  k_ra8_canfd_dlc_max        = 15U, /**< 4-bit DLC max value (64 bytes). */
} ra8_canfd_frame_limits_t;

/**
 * @enum ra8_canfd_pad_t
 * @brief Reserved-region sizes between FSP-named registers.
 *
 * @details
 * Each value covers a documented gap in the RA8D2 R_CANFD_Type
 * memory map (see HUM Ch 41 register-list table and FSP
 * `R7KA8D2KF_core0.h::R_CANFD_Type`). Naming them keeps the layout
 * literal-free and lets clang-tidy `readability-magic-numbers`
 * stay happy.
 */
typedef enum : uint16_t {
  k_ra8_canfd_pad_cfdc2_tail_bytes  = 12U,  /**< CFDC2 reserved tail (3 words). */
  k_ra8_canfd_pad_rm_cluster_words  = 104U, /**< CFDRM cluster pad (0x1A0/4).   */
  k_ra8_canfd_pad_before_grstc      = 2U,   /**< 0x0D0..0x0D7 -> 2 words.       */
  k_ra8_canfd_pad_after_grstc_words = 9U,   /**< 0x0DC..0x0FF -> 9 words.       */
  k_ra8_canfd_pad_before_rpgacc     = 24U,  /**< 0x220..0x27F -> 24 words.      */
  k_ra8_canfd_rpgacc_word_count     = 64U,  /**< CFDRPGACC[64] RAM-test page.   */
  k_ra8_canfd_pad_before_rf_words   = 104U, /**< 0x380..0x51F -> 104 words.     */
  k_ra8_canfd_pad_before_thl_words  = 3U,   /**< 0x734..0x73F -> 3 words.       */
  k_ra8_canfd_pad_before_rm_words   = 118U, /**< 0x748..0x91F -> 118 words.     */
} ra8_canfd_pad_t;

/* =============================================================================
 * Per-channel control block (CFDC[0])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDC_Type` (size 0x10):
 *   +0x00 NCFG  Nominal-bitrate config
 *   +0x04 CTR   Channel control
 *   +0x08 STS   Channel status (TEC[31:24], REC[23:16] live here)
 *   +0x0C ERFL  Channel error flags / CRC value
 */
typedef struct {
  volatile uint32_t NCFG; /**< +0x000 Nominal Bitrate Config. */
  volatile uint32_t CTR;  /**< +0x004 Channel Control.        */
  volatile uint32_t STS;  /**< +0x008 Channel Status.         */
  volatile uint32_t ERFL; /**< +0x00C Channel Error Flag.     */
} r_canfd_cfdc_t;

/* =============================================================================
 * Per-channel CAN-FD config block (CFDC2[0])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDC2_Type` (size 0x20):
 *   +0x00 DCFG    Data-bitrate config
 *   +0x04 FDCFG   CAN-FD config (TDC, FDOE, REFE, CLOE, ESIC)
 *   +0x08 FDCTR   CAN-FD control (EOCCLR, SOCCLR)
 *   +0x0C FDSTS   CAN-FD status (TDC result, EOC/SOC counters)
 *   +0x10 FDCRC   CAN-FD CRC + stuff-bit count
 *   +0x14 .. 0x1F reserved (3 words)
 */
typedef struct {
  volatile uint32_t DCFG;                                        /**< +0x000 Data Bitrate Config. */
  volatile uint32_t FDCFG;                                       /**< +0x004 CAN-FD Config.       */
  volatile uint32_t FDCTR;                                       /**< +0x008 CAN-FD Control.      */
  volatile uint32_t FDSTS;                                       /**< +0x00C CAN-FD Status.       */
  volatile uint32_t FDCRC;                                       /**< +0x010 CAN-FD CRC.          */
  volatile uint8_t  _reserved[k_ra8_canfd_pad_cfdc2_tail_bytes]; /**< Reserved.                   */
} r_canfd_cfdc2_t;

/* =============================================================================
 * Acceptance-filter-list entry (CFDGAFL[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDGAFL_Type` (size 0x10) -- the AFL is paged through a
 * 16-entry page window. Set CFDGAFLECTR.AFLPN to select the page.
 */
typedef struct {
  volatile uint32_t ID; /**< +0x000 GAFLID + GAFLLB + GAFLRTR + GAFLIDE.  */
  volatile uint32_t M;  /**< +0x004 GAFLIDM + GAFLIFL1 + GAFLRTRM + IDEM. */
  volatile uint32_t P0; /**< +0x008 GAFLDLC + GAFLIFL0 + GAFLRMDP + ...   */
  volatile uint32_t P1; /**< +0x00C GAFLFDP.                              */
} r_canfd_cfdgafl_t;

/* =============================================================================
 * RX FIFO access registers (CFDRF[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDRF_Type` (size 0x4C).
 */
typedef struct {
  volatile uint32_t ID;                             /**< +0x000 RFID + RFRTR + RFIDE.       */
  volatile uint32_t PTR;                            /**< +0x004 RFTS + RFDLC.               */
  volatile uint32_t FDSTS;                          /**< +0x008 RFESI + RFBRS + RFFDF + ... */
  volatile uint8_t  DF[k_ra8_canfd_data_bytes_max]; /**< +0x00C 64-byte data field.         */
} r_canfd_cfdrf_t;

/* =============================================================================
 * Common FIFO access registers (CFDCF[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDCF_Type` (size 0x4C).
 */
typedef struct {
  volatile uint32_t ID;                             /**< +0x000 CFID + THLEN + CFRTR + IDE. */
  volatile uint32_t PTR;                            /**< +0x004 CFTS + CFDLC.               */
  volatile uint32_t FDSTS;                          /**< +0x008 CFESI + CFBRS + CFFDF + ... */
  volatile uint8_t  DF[k_ra8_canfd_data_bytes_max]; /**< +0x00C 64-byte data field.         */
} r_canfd_cfdcf_t;

/* =============================================================================
 * TX message-buffer access registers (CFDTM[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDTM_Type` (size 0x4C).
 */
typedef struct {
  volatile uint32_t ID;                             /**< +0x000 TMID + THLEN + TMRTR + IDE. */
  volatile uint32_t PTR;                            /**< +0x004 TMTS + TMDLC.               */
  volatile uint32_t FDCTR;                          /**< +0x008 TMESI + TMBRS + TMFDF + ... */
  volatile uint8_t  DF[k_ra8_canfd_data_bytes_max]; /**< +0x00C 64-byte data field.         */
} r_canfd_cfdtm_t;

/* =============================================================================
 * TX history list (CFDTHL[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDTHL_Type` (size 0x8).
 */
typedef struct {
  volatile uint32_t ACC0; /**< +0x000 BT + BN + TMTS. */
  volatile uint32_t ACC1; /**< +0x004 TID + TIFL.     */
} r_canfd_cfdthl_t;

/* =============================================================================
 * RX message-buffer cluster (CFDRM[i])
 * =============================================================================
 *
 * FSP `R_CANFD_CFDRM_Type` (size 0x400) -- 8 RM entries (each 0x4C)
 * followed by 0x1A0 of reserved padding.
 */
typedef struct {
  volatile uint32_t ID;                             /**< +0x000 RMID + RMRTR + RMIDE.  */
  volatile uint32_t PTR;                            /**< +0x004 RMTS + RMDLC.          */
  volatile uint32_t FDSTS;                          /**< +0x008 RMESI + RMBRS + RMFDF. */
  volatile uint8_t  DF[k_ra8_canfd_data_bytes_max]; /**< +0x00C 64-byte data field.    */
} r_canfd_cfdrm_entry_t;

typedef struct {
  r_canfd_cfdrm_entry_t RM[k_ra8_canfd_rm_per_cluster]; /**< +0x000 8 RX MBs. */
  volatile uint32_t     _reserved[k_ra8_canfd_pad_rm_cluster_words];
  /**< 0x4C * 8 = 0x260 -> pad to 0x400 (104 words). */
} r_canfd_cfdrm_t;

/* =============================================================================
 * Aggregate channel block (R_CANFD0 / R_CANFD1)
 * =============================================================================
 */
typedef struct {
  /* 0x000 -- per-channel control */
  r_canfd_cfdc_t    CFDC[1];       /**< CFDC register.           */
  volatile uint32_t _r_after_cfdc; /**< Reserved (offset 0x010). */

  /* 0x014 -- global control (per-instance copy) */
  volatile uint32_t CFDGCFG;     /**< CFDGCFG register (offset 0x014).     */
  volatile uint32_t CFDGCTR;     /**< CFDGCTR register (offset 0x018).     */
  volatile uint32_t CFDGSTS;     /**< CFDGSTS register (offset 0x01C).     */
  volatile uint32_t CFDGERFL;    /**< CFDGERFL register (offset 0x020).    */
  volatile uint32_t CFDGTSC;     /**< CFDGTSC register (offset 0x024).     */
  volatile uint32_t CFDGAFLECTR; /**< CFDGAFLECTR register (offset 0x028). */
  volatile uint32_t CFDGAFLCFG0; /**< CFDGAFLCFG0 register (offset 0x02C). */
  volatile uint32_t CFDRMNB;     /**< CFDRMNB register (offset 0x030).     */
  volatile uint32_t CFDRMND0;    /**< CFDRMND0 register (offset 0x034).    */
  volatile uint32_t CFDRMIEC;    /**< CFDRMIEC register (offset 0x038).    */
  volatile uint32_t
    CFDRFCC[k_ra8_canfd_rx_fifo_count]; /**< CFDRFCC register (offset 0x03C..0x043). */
  volatile uint32_t
    CFDRFSTS[k_ra8_canfd_rx_fifo_count]; /**< CFDRFSTS register (offset 0x044..0x04B). */
  volatile uint32_t
    CFDRFPCTR[k_ra8_canfd_rx_fifo_count];              /**< CFDRFPCTR reg (offset 0x04C..0x053).  */
  volatile uint32_t CFDCFCC[k_ra8_canfd_cf_count];     /**< CFDCFCC register (offset 0x054).      */
  volatile uint32_t CFDCFSTS[k_ra8_canfd_cf_count];    /**< CFDCFSTS register (offset 0x058).     */
  volatile uint32_t CFDCFPCTR[k_ra8_canfd_cf_count];   /**< CFDCFPCTR register (offset 0x05C).    */
  volatile uint32_t CFDFESTS;                          /**< CFDFESTS register (offset 0x060).     */
  volatile uint32_t CFDFFSTS;                          /**< CFDFFSTS register (offset 0x064).     */
  volatile uint32_t CFDFMSTS;                          /**< CFDFMSTS register (offset 0x068).     */
  volatile uint32_t CFDRFISTS;                         /**< CFDRFISTS register (offset 0x06C).    */
  volatile uint8_t  CFDTMC[k_ra8_canfd_tx_mb_count];   /**< CFDTMC register (offset 0x070).       */
  volatile uint8_t  CFDTMSTS[k_ra8_canfd_tx_mb_count]; /**< CFDTMSTS register (offset 0x074).     */
  volatile uint32_t CFDTMTRSTS[1];                     /**< CFDTMTRSTS register (offset 0x078).   */
  volatile uint32_t CFDTMTARSTS[1];                    /**< CFDTMTARSTS register (offset 0x07C).  */
  volatile uint32_t CFDTMTCSTS[1];                     /**< CFDTMTCSTS register (offset 0x080).   */
  volatile uint32_t CFDTMTASTS[1];                     /**< CFDTMTASTS register (offset 0x084).   */
  volatile uint32_t CFDTMIEC[1];                       /**< CFDTMIEC register (offset 0x088).     */
  volatile uint32_t CFDTXQCC0[1];                      /**< CFDTXQCC0 register (offset 0x08C).    */
  volatile uint32_t CFDTXQSTS0[1];                     /**< CFDTXQSTS0 register (offset 0x090).   */
  volatile uint32_t CFDTXQPCTR0[1];                    /**< CFDTXQPCTR0 register (offset 0x094).  */
  volatile uint32_t CFDTHLCC[1];                       /**< CFDTHLCC register (offset 0x098).     */
  volatile uint32_t CFDTHLSTS[1];                      /**< CFDTHLSTS register (offset 0x09C).    */
  volatile uint32_t CFDTHLPCTR[1];                     /**< CFDTHLPCTR register (offset 0x0A0).   */
  volatile uint32_t CFDGTINTSTS0;                      /**< CFDGTINTSTS0 register (offset 0x0A4). */
  volatile uint32_t CFDGTSTCFG;                        /**< CFDGTSTCFG register (offset 0x0A8).   */
  volatile uint32_t CFDGTSTCTR;                        /**< CFDGTSTCTR register (offset 0x0AC).   */
  volatile uint32_t CFDGFDCFG;                         /**< CFDGFDCFG register (offset 0x0B0).    */
  volatile uint32_t _r_after_gfdcfg;                   /**< Reserved (offset 0x0B4).              */
  volatile uint32_t CFDGLOCKK;                         /**< CFDGLOCKK register (offset 0x0B8).    */
  volatile uint32_t _r_after_glockk;                   /**< Reserved (offset 0x0BC).              */
  volatile uint32_t CFDGAFLIGNENT;                     /**< CFDGAFLIGNENT reg (offset 0x0C0).     */
  volatile uint32_t CFDGAFLIGNCTR;                     /**< CFDGAFLIGNCTR reg (offset 0x0C4).     */
  volatile uint32_t CFDCDTCT;                          /**< CFDCDTCT register (offset 0x0C8).     */
  volatile uint32_t CFDCDTSTS;                         /**< CFDCDTSTS register (offset 0x0CC).    */
  volatile uint32_t
    _r_before_grstc[k_ra8_canfd_pad_before_grstc]; /**< Reserved (offset 0x0D0..0x0D7).   */
  volatile uint32_t CFDGRSTC;                      /**< CFDGRSTC register (offset 0x0D8). */
  volatile uint32_t
    _r_after_grstc[k_ra8_canfd_pad_after_grstc_words]; /**< Reserved (offset 0x0DC..0x0FF). */

  /* 0x100 -- per-channel CAN-FD config */
  r_canfd_cfdc2_t CFDC2[1]; /**< CFDC2 register (offset 0x100 (0x20)). */

  /* 0x120 -- AFL page window */
  r_canfd_cfdgafl_t
    CFDGAFL[k_ra8_canfd_afl_page_size]; /**< CFDGAFL register (offset 0x120 (0x100)). */

  /* 0x220 -- reserved (0x60) */
  volatile uint32_t
    _r_before_rpgacc[k_ra8_canfd_pad_before_rpgacc]; /**< Reserved (offset 0x220..0x27F). */

  /* 0x280 -- RAM test page */
  volatile uint32_t
    CFDRPGACC[k_ra8_canfd_rpgacc_word_count]; /**< CFDRPGACC register (offset 0x280 (0x100)). */

  /* 0x380 -- reserved (0x1A0) */
  volatile uint32_t
    _r_before_rf[k_ra8_canfd_pad_before_rf_words]; /**< Reserved (offset 0x380..0x51F). */

  /* 0x520 -- RX FIFO access (2 fifos x 0x4C each = 0x98) */
  r_canfd_cfdrf_t CFDRF[k_ra8_canfd_rx_fifo_count]; /**< CFDRF register (offset 0x520..0x5B7). */

  /* 0x5B8 -- common FIFO access (1 fifo x 0x4C) */
  r_canfd_cfdcf_t CFDCF[k_ra8_canfd_cf_count]; /**< CFDCF register (offset 0x5B8..0x603). */

  /* 0x604 -- TX MB access (4 buffers x 0x4C = 0x130) */
  r_canfd_cfdtm_t CFDTM[k_ra8_canfd_tx_mb_count]; /**< CFDTM register (offset 0x604..0x733). */

  /* 0x734 -- reserved before THL (3 words) */
  volatile uint32_t
    _r_before_thl[k_ra8_canfd_pad_before_thl_words]; /**< Reserved (offset 0x734..0x73F). */

  /* 0x740 -- TX history list */
  r_canfd_cfdthl_t CFDTHL[1]; /**< CFDTHL register (offset 0x740..0x747). */

  /* 0x748 -- reserved before RM cluster (118 words = 0x1D8) */
  volatile uint32_t
    _r_before_rm[k_ra8_canfd_pad_before_rm_words]; /**< Reserved (offset 0x748..0x91F). */

  /* 0x920 -- RX MB cluster (4 clusters x 0x400) */
  r_canfd_cfdrm_t CFDRM[k_ra8_canfd_rm_clusters]; /**< CFDRM register (offset 0x920..0x191F). */
} r_canfd_t;

/** @brief Get pointer to CANFD instance N (0..1). */
RA8_HW_REGISTER_ACCESS
static inline volatile r_canfd_t* ra8_canfd(uint8_t channel)
{
  if (channel >= k_ra8_canfd_instance_count) {
    return nullptr;
  }
  const uintptr_t base = (channel == 0U) ? k_ra8_canfd0_base_addr : k_ra8_canfd1_base_addr;
  return (volatile r_canfd_t*)base;
}

/* =============================================================================
 * Channel Control (CFDC[0].CTR) bit positions
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC_Type::CTR_b`. CHMDC is 2 bits at [1:0],
 * CSLPR is bit 2, BOM at [22:21]. The previous "Lite" header had
 * CHMDC at 3 bits and CSLPR at bit 4 -- both wrong.
 */

typedef enum : uint8_t {
  k_ra8_cnctr_bit_chmdc = 0U,  /**< Channel Mode Control [1:0].       */
  k_ra8_cnctr_bit_cslpr = 2U,  /**< Channel Sleep Request bit 2.      */
  k_ra8_cnctr_bit_bom   = 21U, /**< Bus-Off Mode [22:21].             */
  k_ra8_cnctr_bit_ctme  = 24U, /**< Channel Test Mode Enable bit 24.  */
  k_ra8_cnctr_bit_ctms  = 25U, /**< Channel Test Mode Select [26:25]. */
} ra8_cnctr_bit_t;

typedef enum : uint32_t {
  k_ra8_cnctr_mask_chmdc = 0x3UL,        /**< 2-bit CHMDC mask.          */
  k_ra8_cnctr_mask_cslpr = 0x4UL,        /**< CSLPR (bit 2).             */
  k_ra8_cnctr_mask_ctme  = 1UL << 24U,   /**< CTME bit 24.               */
  k_ra8_cnctr_mask_ctms  = 0x3UL << 25U, /**< CTMS[1:0] at bits [26:25]. */
} ra8_cnctr_mask_t;

/**
 * @enum ra8_ctms_mode_t
 * @brief Channel Test Mode Select values (CFDC[0].CTR.CTMS[1:0]).
 *
 * @details
 * HUM Ch 41 "CFDCnCTR" p 2710 -- CTMS is bits [26:25] of CTR and is
 * only writable while the channel is in CH_HALT mode.  Selecting any
 * test mode requires CTME (bit 24) to be set in the same write.
 */
typedef enum : uint8_t {
  k_ra8_ctms_basic       = 0U, /**< 00b: Basic test mode (CRC observation). */
  k_ra8_ctms_listen_only = 1U, /**< 01b: Listen-only mode.                  */
  k_ra8_ctms_self_test_0 = 2U, /**< 10b: Self-test 0 (external loopback).   */
  k_ra8_ctms_self_test_1 = 3U, /**< 11b: Self-test 1 (internal loopback).   */
} ra8_ctms_mode_t;

/**
 * @enum ra8_chmdc_mode_t
 * @brief Channel Mode Control (`CFDC[0].CTR.CHMDC[1:0]`) values.
 */
typedef enum : uint8_t {
  k_ra8_chmdc_operation = 0U, /**< Bus operation mode. */
  k_ra8_chmdc_reset     = 1U, /**< Reset mode.         */
  k_ra8_chmdc_halt      = 2U, /**< Halt mode.          */
  k_ra8_chmdc_keep      = 3U, /**< Keep current mode.  */
} ra8_chmdc_mode_t;

/* =============================================================================
 * Channel Status (CFDC[0].STS) bit positions
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC_Type::STS_b`. TEC[31:24] and REC[23:16] live
 * here -- not in ERFL like the previous header had it.
 */

typedef enum : uint8_t {
  k_ra8_cnsts_bit_crstst = 0U,  /**< Channel Reset Status. */
  k_ra8_cnsts_bit_chltst = 1U,  /**< Channel Halt Status.  */
  k_ra8_cnsts_bit_cslpst = 2U,  /**< Channel Sleep Status. */
  k_ra8_cnsts_bit_epst   = 3U,  /**< Error Passive Status. */
  k_ra8_cnsts_bit_bosts  = 4U,  /**< Bus-Off Status.       */
  k_ra8_cnsts_bit_rec    = 16U, /**< REC[23:16] in STS.    */
  k_ra8_cnsts_bit_tec    = 24U, /**< TEC[31:24] in STS.    */
} ra8_cnsts_bit_t;

typedef enum : uint32_t {
  k_ra8_cnsts_mask_rec = 0xFFUL, /**< REC field mask (post-shift). */
  k_ra8_cnsts_mask_tec = 0xFFUL, /**< TEC field mask (post-shift). */
} ra8_cnsts_mask_t;

/* =============================================================================
 * Nominal Bit-Rate Config (CFDC[0].NCFG)
 * =============================================================================
 *
 * Verified against FSP `R_CANFD_CFDC_NCFG_b` (R7KA8D2KF_core0.h
 * lines ~1018-1027): NBRP[9:0], NSJW[16:10], NTSEG1[24:17],
 * NTSEG2[31:25].
 */

typedef enum : uint8_t {
  k_ra8_cncfg_shift_nbrp   = 0U,  /**< RA8 cncfg shift nbrp.   */
  k_ra8_cncfg_shift_nsjw   = 10U, /**< RA8 cncfg shift nsjw.   */
  k_ra8_cncfg_shift_ntseg1 = 17U, /**< RA8 cncfg shift ntseg1. */
  k_ra8_cncfg_shift_ntseg2 = 25U, /**< RA8 cncfg shift ntseg2. */
} ra8_cncfg_shift_t;

typedef enum : uint32_t {
  k_ra8_cncfg_mask_nbrp   = 0x3FFUL, /**< [9:0]   10-bit prescaler field. */
  k_ra8_cncfg_mask_nsjw   = 0x7FUL,  /**< [16:10] 7-bit SJW field.        */
  k_ra8_cncfg_mask_ntseg1 = 0xFFUL,  /**< [24:17] 8-bit TSEG1 field.      */
  k_ra8_cncfg_mask_ntseg2 = 0x7FUL,  /**< [31:25] 7-bit TSEG2 field.      */
} ra8_cncfg_mask_t;

/* =============================================================================
 * Data Bit-Rate Config (CFDC2[0].DCFG) -- DIFFERENT field positions
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC2_DCFG_b`: DBRP[7:0], DTSEG1[12:8],
 * DTSEG2[19:16], DSJW[27:24]. Narrower fields than nominal.
 */

typedef enum : uint8_t {
  k_ra8_dcfg_shift_dbrp   = 0U,  /**< RA8 dcfg shift dbrp.   */
  k_ra8_dcfg_shift_dtseg1 = 8U,  /**< RA8 dcfg shift dtseg1. */
  k_ra8_dcfg_shift_dtseg2 = 16U, /**< RA8 dcfg shift dtseg2. */
  k_ra8_dcfg_shift_dsjw   = 24U, /**< RA8 dcfg shift dsjw.   */
} ra8_dcfg_shift_t;

typedef enum : uint32_t {
  k_ra8_dcfg_mask_dbrp   = 0xFFUL, /**< [7:0]   8-bit DBRP.   */
  k_ra8_dcfg_mask_dtseg1 = 0x1FUL, /**< [12:8]  5-bit DTSEG1. */
  k_ra8_dcfg_mask_dtseg2 = 0xFUL,  /**< [19:16] 4-bit DTSEG2. */
  k_ra8_dcfg_mask_dsjw   = 0xFUL,  /**< [27:24] 4-bit DSJW.   */
} ra8_dcfg_mask_t;

/**
 * @enum ra8_canfd_bit_timing_limits_t
 * @brief Prescaler / TSEG / SJW resolution bounds shared by both phases.
 */
typedef enum : uint32_t {
  k_ra8_canfd_tq_per_bit         = 20U,   /**< Default time quanta per bit.          */
  k_ra8_canfd_prescaler_min      = 1U,    /**< Smallest valid prescaler.             */
  k_ra8_canfd_prescaler_max      = 1024U, /**< Largest valid nominal prescaler.      */
  k_ra8_canfd_data_prescaler_max = 256U,  /**< Largest valid data prescaler (8-bit). */
  k_ra8_canfd_sjw_max            = 16U,   /**< Nominal SJW cap.                      */
  k_ra8_canfd_dsjw_max           = 16U,   /**< Data-phase SJW cap.                   */
  k_ra8_canfd_dtseg1_max         = 32U,   /**< Data-phase TSEG1 cap.                 */
  k_ra8_canfd_dtseg2_max         = 16U,   /**< Data-phase TSEG2 cap.                 */
} ra8_canfd_bit_timing_limits_t;

/* =============================================================================
 * Channel Error-Flag register (CFDC[0].ERFL) bit positions
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC_ERFL_b`:
 *   bit  0 BEF, 1 EWF, 2 EPF, 3 BOEF, 4 BORF, 5 OVLF, 6 BLF, 7 ALF,
 *   bit  8 SERR, 9 FERR, 10 AERR, 11 CERR, 12 B1ERR, 13 B0ERR,
 *   bit 14 ADERR,
 *   bits [30:16] CRCREG (RO).
 */

typedef enum : uint32_t {
  k_ra8_cnerfl_mask_bef     = 1UL << 0U,  /**< RA8 cnerfl mask bef.            */
  k_ra8_cnerfl_mask_ewf     = 1UL << 1U,  /**< RA8 cnerfl mask ewf.            */
  k_ra8_cnerfl_mask_epf     = 1UL << 2U,  /**< RA8 cnerfl mask epf.            */
  k_ra8_cnerfl_mask_boef    = 1UL << 3U,  /**< RA8 cnerfl mask boef.           */
  k_ra8_cnerfl_mask_borf    = 1UL << 4U,  /**< RA8 cnerfl mask borf.           */
  k_ra8_cnerfl_mask_ovlf    = 1UL << 5U,  /**< RA8 cnerfl mask ovlf.           */
  k_ra8_cnerfl_mask_blf     = 1UL << 6U,  /**< RA8 cnerfl mask blf.            */
  k_ra8_cnerfl_mask_alf     = 1UL << 7U,  /**< RA8 cnerfl mask alf.            */
  k_ra8_cnerfl_mask_serr    = 1UL << 8U,  /**< RA8 cnerfl mask serr.           */
  k_ra8_cnerfl_mask_ferr    = 1UL << 9U,  /**< RA8 cnerfl mask ferr.           */
  k_ra8_cnerfl_mask_aerr    = 1UL << 10U, /**< RA8 cnerfl mask aerr.           */
  k_ra8_cnerfl_mask_cerr    = 1UL << 11U, /**< RA8 cnerfl mask cerr.           */
  k_ra8_cnerfl_mask_b1err   = 1UL << 12U, /**< RA8 cnerfl mask b1err.          */
  k_ra8_cnerfl_mask_b0err   = 1UL << 13U, /**< RA8 cnerfl mask b0err.          */
  k_ra8_cnerfl_mask_aderr   = 1UL << 14U, /**< RA8 cnerfl mask aderr.          */
  k_ra8_cnerfl_mask_all_w1c = 0x7FFFUL,   /**< [14:0] all writable error bits. */
} ra8_cnerfl_mask_t;

/* =============================================================================
 * Global Control (CFDGCTR)
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDGCTR_b`: GMDC[1:0], GSLPR bit 2.
 */

typedef enum : uint8_t {
  k_ra8_gctr_bit_gmdc  = 0U, /**< GMDC field [1:0].         */
  k_ra8_gctr_bit_gslpr = 2U, /**< Global Sleep Request bit. */
} ra8_gctr_bit_t;

typedef enum : uint32_t {
  k_ra8_gctr_mask_gmdc       = 0x3UL,     /**< 2-bit GMDC mask.     */
  k_ra8_gctr_mask_gslpr      = 1UL << 2U, /**< RA8 gctr mask gslpr. */
  k_ra8_gctr_value_operation = 0UL,       /**< GMDC = 0: operation. */
  k_ra8_gctr_value_reset     = 1UL,       /**< GMDC = 1: reset.     */
  k_ra8_gctr_value_halt      = 2UL,       /**< GMDC = 2: halt.      */
} ra8_gctr_value_t;

/**
 * @enum ra8_gsts_bit_t
 * @brief Global Status (`CFDGSTS`) bit positions.
 */
typedef enum : uint8_t {
  k_ra8_gsts_bit_grststs  = 0U, /**< Global Reset Status.       */
  k_ra8_gsts_bit_ghltsts  = 1U, /**< Global Halt Status.        */
  k_ra8_gsts_bit_gslpsts  = 2U, /**< Global Sleep Status.       */
  k_ra8_gsts_bit_graminit = 3U, /**< Global RAM Initialisation. */
} ra8_gsts_bit_t;

/* =============================================================================
 * RX FIFO config / status / control (CFDRFCC, CFDRFSTS, CFDRFPCTR)
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDRFCC_b` and `R_CANFD_CFDRFSTS_b`.
 */

typedef enum : uint32_t {
  k_ra8_rfcc_bit_rfe  = 1UL << 0U, /**< RFE: enable FIFO.       */
  k_ra8_rfcc_bit_rfie = 1UL << 1U, /**< RFIE: interrupt enable. */
} ra8_rfcc_bits_t;

typedef enum : uint32_t {
  k_ra8_rfsts_bit_empty = 1UL << 0U, /**< RFEMP: FIFO empty flag. */
  k_ra8_rfsts_bit_full  = 1UL << 1U, /**< RFFLL: FIFO full flag.  */
  k_ra8_rfsts_bit_mlt   = 1UL << 2U, /**< RFMLT: message lost.    */
  k_ra8_rfsts_bit_if    = 1UL << 3U, /**< RFIF: interrupt flag.   */
} ra8_rfsts_bits_t;

typedef enum : uint32_t {
  k_ra8_rfpctr_value_ack = 0xFFUL, /**< Dummy write to pop entry. */
} ra8_rfpctr_bits_t;

/* =============================================================================
 * Acceptance-filter control (CFDGAFLECTR)
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDGAFLECTR_b`: AFLPN[3:0] page index, bit 8 AFLDAE
 * unlocks data access.
 */

typedef enum : uint32_t {
  k_ra8_gaflectr_mask_aflpn = 0xFUL,     /**< RA8 gaflectr mask aflpn. */
  k_ra8_gaflectr_bit_afldae = 1UL << 8U, /**< RA8 gaflectr bit afldae. */
} ra8_gaflectr_bits_t;

/**
 * @enum ra8_gaflcfg0_bits_t
 * @brief CFDGAFLCFG0 layout (HUM Ch 41.2.18 p 2735).
 *
 * @details RNC0[20:16] selects how many rules on page 0 the AFL
 * consults. Only writable in GL_RESET.
 */
typedef enum : uint32_t {
  k_ra8_gaflcfg0_shift_rnc0 = 16U,   /**< Position of RNC0[4:0]. */
  k_ra8_gaflcfg0_mask_rnc0  = 0x1FU, /**< RNC0 field is 5 bits.  */
} ra8_gaflcfg0_bits_t;

/**
 * @enum ra8_gaflp1_bits_t
 * @brief CFDGAFLP1r layout (HUM Ch 41.2.22 p 2740).
 *
 * @details Bit 0 GAFLFDP0 = route matched frame into RX FIFO 0. At
 * least one of the GAFLFDPn bits MUST be set or the matched frame
 * is dropped instead of being routed.
 */
typedef enum : uint32_t {
  k_ra8_gaflp1_bit_gaflfdp0 = 1UL << 0U, /**< Route to RX FIFO 0. */
} ra8_gaflp1_bits_t;

/**
 * @enum ra8_gaflm_bits_t
 * @brief CFDGAFLM (Acceptance Filter Mask) layout (HUM Ch 41.2.20 p 2736).
 *
 * @details Bit 29 GAFLLB is "Loopback Configuration": when 0 the AFL
 * entry is only valid for standard non-loopback RX; when 1 the AFL
 * entry is also valid in Self-test mode 0/1 (HUM Ch 41.5.5 Table
 * 41.22). For internal-loopback demos (canfd_loopback,
 * canfd_filter_demo) GAFLLB must be set, otherwise every loopback
 * frame passes the filter and a no-match round wrongly receives data.
 *
 * Bits 30/31 are the per-field compare-enable mask bits that sit at the
 * same positions as GAFLRTR / GAFLIDE in the CFDGAFLID word: GAFLRTRM
 * (bit 30) makes the RTR flag part of the acceptance match, and
 * GAFLIDEM (bit 31) makes the IDE flag part of the match so a standard
 * rule does not accidentally accept an extended frame whose low ID bits
 * coincide. Both are 1 = compare, 0 = don't-care (HUM Ch 41.2.20
 * "CFDGAFLM : Acceptance Filter List ID Mask Register" p 2736).
 */
typedef enum : uint32_t {
  k_ra8_gaflm_bit_gafllb   = 1UL << 29U, /**< Loopback Configuration bit. */
  k_ra8_gaflm_bit_gaflrtrm = 1UL << 30U, /**< RTR-compare mask bit.       */
  k_ra8_gaflm_bit_gaflidem = 1UL << 31U, /**< IDE-compare mask bit.       */
} ra8_gaflm_bits_t;

/**
 * @enum ra8_canfd_afl_total_t
 * @brief Maximum number of GAFL filter rules across all paged windows.
 *
 * @details
 * The CFDGAFLECTR.AFLPN field is 4 bits wide so the controller can
 * page through at most 16 windows.  Each window exposes 16 entries
 * (k_ra8_canfd_afl_page_size), giving 256 total filters.  HUM Ch 41
 * "Acceptance Filter List" pp 2702-2867.
 */
typedef enum : uint16_t {
  k_ra8_canfd_afl_total = (uint16_t)(16U * 16U), /**< 16 pages x 16 entries. */
} ra8_canfd_afl_total_t;

/**
 * @enum ra8_canfd_gfdcfg_bits_t
 * @brief CFDGFDCFG (Global FD Config) field positions.
 *
 * @details
 * From FSP `R_CANFD_CFDGFDCFG_b` -- bit 0 is NISO (1 = ISO 11898-1
 * stuff-count / CRC, 0 = original Bosch non-ISO framing).
 */
typedef enum : uint32_t {
  k_ra8_gfdcfg_bit_niso = 1UL << 0U, /**< NISO: ISO mode select. */
} ra8_canfd_gfdcfg_bits_t;

/* =============================================================================
 * Message-ID layout (CFDRF.ID / CFDTM.ID / CFDCF.ID / CFDRM.ID)
 * =============================================================================
 *
 * Common across RX FIFO, TX MB, common FIFO and RX MB:
 *   [28:0]  raw ID (29-bit extended or 11-bit standard right-aligned)
 *   [29]    THLEN (TX) / GAFLLB (AFL) / reserved (RX)
 *   [30]    RTR
 *   [31]    IDE (1 = extended)
 */
typedef enum : uint32_t {
  k_ra8_canfd_id_std_mask = 0x000007FFUL, /**< 11-bit standard ID.     */
  k_ra8_canfd_id_ext_mask = 0x1FFFFFFFUL, /**< 29-bit extended ID.     */
  k_ra8_canfd_id_thlen    = 1UL << 29U,   /**< TX history-list enable. */
  k_ra8_canfd_id_rtr      = 1UL << 30U,   /**< RTR: remote frame.      */
  k_ra8_canfd_id_ide      = 1UL << 31U,   /**< IDE: extended flag.     */
} ra8_canfd_id_bits_t;

/* =============================================================================
 * FD-status layout (CFDTM.FDCTR / CFDRF.FDSTS / CFDCF.FDSTS / CFDRM.FDSTS)
 * =============================================================================
 *
 * From FSP unions: bit 0 = ESI, bit 1 = BRS, bit 2 = FDF.
 * Note this is DIFFERENT from the previous Lite header which had FDF
 * at bit 0 -- that was wrong.
 */

typedef enum : uint32_t {
  k_ra8_canfd_fd_esi = 1UL << 0U, /**< ESI: error state indicator. */
  k_ra8_canfd_fd_brs = 1UL << 1U, /**< BRS: bit-rate switch.       */
  k_ra8_canfd_fd_fdf = 1UL << 2U, /**< FDF: frame is CAN-FD.       */
} ra8_canfd_fd_bits_t;

/* =============================================================================
 * PTR layout (CFDTM.PTR / CFDRF.PTR / CFDCF.PTR / CFDRM.PTR)
 * =============================================================================
 *
 * [15:0]   timestamp,  [31:28] DLC.
 */
typedef enum : uint8_t {
  k_ra8_canfd_ptr_shift_dlc = 28U, /**< DLC field shift. */
} ra8_canfd_ptr_shift_t;

typedef enum : uint32_t {
  k_ra8_canfd_ptr_mask_dlc = 0xFUL, /**< 4-bit DLC mask. */
} ra8_canfd_ptr_mask_t;

/* =============================================================================
 * TX message-buffer control byte (CFDTMC[i])
 * =============================================================================
 *
 * Single byte per buffer: TMTR (bit 0), TMTAR (bit 1), TMOM (bit 2).
 */
typedef enum : uint8_t {
  k_ra8_canfd_tmc_txreq   = 1U << 0U, /**< TMTR: transmit request. */
  k_ra8_canfd_tmc_abort   = 1U << 1U, /**< TMTAR: abort request.   */
  k_ra8_canfd_tmc_oneshot = 1U << 2U, /**< TMOM: one-shot mode.    */
} ra8_canfd_tmc_bits_t;

/* =============================================================================
 * CAN-FD Config register (CFDC2[0].FDCFG) -- TDC fields
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC2_FDCFG_b` (R7KA8D2KF_core0.h):
 *   bits [2:0]   EOCCFG  Error-Occurrence Counter Config
 *   bits [7:3]   reserved
 *   bits [14:8]  TDCO    Transmitter Delay Compensation Offset (7-bit)
 *   bit  [15]    TDCOC   TDC Offset Control (0=measured FDSTS.TDCR, 1=TDCO)
 *   bit  [16]    TDE     Transmitter Delay Compensation Enable
 *   bit  [17]    reserved
 *   bit  [18]    FDOE    FD-Only Enable
 *   bit  [19]    REFE    RX Edge Filter Enable
 *   bit  [20]    CLOE    CAN Classic-frame Loopback Enable
 *   bits [23:21] reserved
 *   bit  [24]    ESIC    Error State Indication Configuration
 *   bits [31:25] reserved
 *
 * HUM Ch 41 "CFDCnFDCFG" p 2788.
 */

/**
 * @enum ra8_fdcfg_shift_t
 * @brief Bit-shift positions for the TDC fields in CFDCnFDCFG.
 *
 * @details
 * TDCO is a 7-bit offset at [14:8]; TDCOC is the offset-control select
 * bit at [15]; TDE is the enable gate at [16]. Together these three
 * fields govern Transmitter Delay Compensation. HUM Ch 41 "CFDCnFDCFG"
 * p 2788.
 *
 * @see ra8_fdcfg_mask_t
 * @see ra8_canfd_set_tdc()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_fdcfg_shift_tdco  = 8U,  /**< TDCO[14:8]: 7-bit TDC offset bit-shift.  */
  k_ra8_fdcfg_shift_tdcoc = 15U, /**< TDCOC[15]: TDC offset-control bit-shift. */
  k_ra8_fdcfg_shift_tde   = 16U, /**< TDE[16]: TDC enable bit-shift.           */
} ra8_fdcfg_shift_t;

/**
 * @enum ra8_fdcfg_mask_t
 * @brief Bit masks for the TDC fields in CFDCnFDCFG.
 *
 * @details
 * ``k_ra8_fdcfg_mask_tdco`` is the pre-shift 7-bit mask for the TDCO
 * offset field; apply it as
 * ``(offset & k_ra8_fdcfg_mask_tdco) << k_ra8_fdcfg_shift_tdco``.
 * ``k_ra8_fdcfg_mask_tdcoc`` and ``k_ra8_fdcfg_mask_tde`` are
 * positioned masks (already at their register bit positions). HUM Ch 41
 * "CFDCnFDCFG" p 2788.
 *
 * @see ra8_fdcfg_shift_t
 * @see ra8_canfd_set_tdc()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_fdcfg_mask_tdco  = 0x7FUL,     /**< TDCO pre-shift 7-bit mask.      */
  k_ra8_fdcfg_mask_tdcoc = 1UL << 15U, /**< TDCOC positioned mask (bit 15). */
  k_ra8_fdcfg_mask_tde   = 1UL << 16U, /**< TDE positioned mask (bit 16).   */
} ra8_fdcfg_mask_t;

/**
 * @enum ra8_canfd_tdc_limits_t
 * @brief Bounds on the Transmitter Delay Compensation Offset field.
 *
 * @details
 * TDCO is a 7-bit field at FDCFG[14:8], so the offset must not exceed
 * ``k_ra8_canfd_tdc_offset_max`` (0x7F = 127). A value of zero is legal
 * and represents a zero-TQ offset when TDE is set. HUM Ch 41
 * "CFDCnFDCFG" p 2788.
 *
 * @see ra8_canfd_set_tdc()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_canfd_tdc_offset_max = 127U, /**< Maximum TDCO value: 7-bit field cap. */
} ra8_canfd_tdc_limits_t;

/* =============================================================================
 * CAN-FD Status register (CFDC2[0].FDSTS) -- TDC result field
 * =============================================================================
 *
 * From FSP `R_CANFD_CFDC2_FDSTS_b`:
 *   bits [7:0]  TDCR    Measured TDC result in time quanta (read-only)
 *   bit  [8]    EOCO    Error-Occurrence Counter Overflow
 *   bit  [9]    SOCO    Stuff-bit Counter Overflow
 *
 * After TDE is set the controller writes the measured transmitter loop
 * delay (in TQ units) into FDSTS.TDCR[7:0] after the first successful
 * data-phase bit. HUM Ch 41 "CFDCnFDSTS" p 2792.
 */

/**
 * @enum ra8_fdsts_mask_t
 * @brief Bit mask for the TDCR read-only measurement in CFDCnFDSTS.
 *
 * @details
 * Reading and masking with ``k_ra8_fdsts_mask_tdcr`` extracts the 8-bit
 * measured transmitter loop delay from FDSTS[7:0]. Compare it against
 * FDCFG.TDCO to tune the manual offset. HUM Ch 41 "CFDCnFDSTS" p 2792.
 *
 * @see ra8_canfd_set_tdc()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_fdsts_mask_tdcr = 0xFFUL, /**< TDCR[7:0]: 8-bit measured TDC result. */
} ra8_fdsts_mask_t;

#ifdef __cplusplus
}
#endif
