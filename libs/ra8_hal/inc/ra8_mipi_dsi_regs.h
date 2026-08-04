/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_dsi_regs.h
 * @brief MIPI DSI Host (DSI-2) register layout for the RA8D2
 * @ingroup grp_hal_display
 *
 * @details
 * The RA8D2 MIPI DSI Host module is a MIPI DSI-2 transmitter that
 * pairs with the GLCDC parallel-RGB stream and the on-die D-PHY
 * (1 or 2 lanes, 80-720 Mbps/lane). The block exposes a 0x880-byte
 * window at base `0x40346000` (Secure alias) / `0x50346000`
 * (Non-Secure alias).
 *
 * This header is the **complete** register layout for HUM Ch 65
 * (MIPI DSI, p 3839-3934). Every documented offset is named, and
 * every documented bit field is exposed via a typed enum so the
 * driver code can stay magic-number-free.
 *
 * Register table (slim summary -- bit fields are in dedicated
 * enums below):
 *
 * | Offset  | Name           | Width | Purpose                                                   |
 * |--------:|----------------|------:|-----------------------------------------------------------|
 * |  0x000  | ISR            | 32    | Top-level interrupt status (read-only).                   |
 * |  0x010  | LINKSR         | 32    | Link status (SQ0/SQ1/VRUN/HSBUSY/LPBUSY).                 |
 * |  0x100  | TXSETR         | 32    | Number of lanes + clock/data lane enable.                 |
 * |  0x104  | HSCLKSETR      | 32    | HS clock start + continuous mode select.                  |
 * |  0x108  | ULPSSETR       | 32    | ULPS wakeup period.                                       |
 * |  0x10C  | ULPSCR         | 32    | ULPS enter/exit pulses (clock + data lanes).              |
 * |  0x110  | RSTCR          | 32    | Software reset + force-Tx-stop.                           |
 * |  0x114  | RSTSR          | 32    | Reset status (HS/LP/APB/AXI/Video, lane stop, lane dir).  |
 * |  0x120  | DSISETR        | 32    | DSI link configuration (ECC, CRC, scramble, EoTp, etc).   |
 * |  0x160  | TXPPD0..3R     | 32    | Transmit packet payload data 0..15 (4 x 32-bit).          |
 * |  0x200  | RXSR           | 32    | Receive status register.                                  |
 * |  0x204  | RXSCR          | 32    | Receive status clear register.                            |
 * |  0x208  | RXIER          | 32    | Receive interrupt enable.                                 |
 * |  0x210  | PRESPTOBTASETR | 32    | Peripheral response timeout (BTA).                        |
 * |  0x214  | PRESPTOLPSETR  | 32    | Peripheral response timeout (LP read/write).              |
 * |  0x218  | PRESPTOHSSETR  | 32    | Peripheral response timeout (HS read/write).              |
 * |  0x220  | AKEPLATIR      | 32    | Ack-and-error report latest info (RO).                    |
 * |  0x224  | AKEPACMSR      | 32    | Ack-and-error report accumulated status (RO).             |
 * |  0x228  | AKEPSCR        | 32    | Ack-and-error report status clear.                        |
 * |  0x230  | RXRSSR         | 32    | Receive Result Save status (slot valid bits).             |
 * |  0x234  | RXRSSCR        | 32    | Receive Result Save status clear.                         |
 * |  0x238  | RXRINFOOWSR    | 32    | Receive Result info-overwrite status.                     |
 * |  0x23C  | RXRINFOOWSCR   | 32    | Receive Result info-overwrite status clear.               |
 * |  0x240  | RXRSS0..3R     | 32    | Receive result save slot 0..3 (header captured).          |
 * |  0x2C0  | RXPPD0..3R     | 32    | Receive packet payload data (4 x 32-bit, 16 bytes).       |
 * |  0x2E0  | HSTXTOSETR     | 32    | HS transmit timeout count.                                |
 * |  0x2E4  | LRXHTOSETR     | 32    | LP-RX host processor timeout.                             |
 * |  0x2E8  | TATOSETR       | 32    | Turnaround acknowledge timeout.                           |
 * |  0x300  | FERRSR/SCR/IER | 32    | Fatal-error status / clear / enable.                      |
 * |  0x314  | CLSTPTSETR     | 32    | Clock-lane stop / beforehand / keep timing.               |
 * |  0x318  | LPTRNSTSETR    | 32    | LP transition (Go LP and Back) timing.                    |
 * |  0x320  | PLSR/SCR/IER   | 32    | PHY (PPI) status / clear / enable.                        |
 * |  0x400  | VMSET0R        | 32    | Video-mode set 0 (HSA/HBP/HFP no-LP, START/STOP).         |
 * |  0x404  | VMSET1R        | 32    | Video-mode set 1 (DLY value).                             |
 * |  0x410  | VMSR/SCR/IER   | 32    | Video-mode status / clear / enable.                       |
 * |  0x420  | VMPPSETR       | 32    | Video-mode pixel-packet set (TXESYNC, DT, VC).            |
 * |  0x428  | VMVSSETR       | 32    | Vertical sync (VSA, VSPOL, VACT).                         |
 * |  0x42C  | VMVPSETR       | 32    | Vertical porch (VBP, VFP).                                |
 * |  0x430  | VMHSSETR       | 32    | Horizontal sync (HSA, HSPOL, HACT).                       |
 * |  0x434  | VMHPSETR       | 32    | Horizontal porch (HBP, HFP).                              |
 * |  0x5C0  | SQCH0SET0R     | 32    | Sequence channel 0 START bit.                             |
 * |  0x5D0  | SQCH0SR/SCR/IER| 32    | Sequence channel 0 status / clear / enable.               |
 * |  0x600  | SQCH1SET0R     | 32    | Sequence channel 1 START bit.                             |
 * |  0x610  | SQCH1SR/SCR/IER| 32    | Sequence channel 1 status / clear / enable.               |
 * |  0x780  | SQCH0DSC0..7   | 32x4  | Sequence ch0 descriptor 0..7 (A/B/C/D each).              |
 * |  0x800  | SQCH1DSC0..7   | 32x4  | Sequence ch1 descriptor 0..7 (A/B/C/D each).              |
 *
 * Citations: HUM Ch 65 "MIPI DSI" subsections 65.2.1..65.2.140,
 * pages 3839-3934. The struct member offsets and total window size
 * (0x880 bytes) match the FSP `R_MIPI_DSI_Type` declaration in
 * `R7KA8D2KF_core0.h` line 54968-61047.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_mipi_dsi_addr_t
 * @brief Memory-mapped base addresses for the MIPI DSI host.
 *
 * @details
 * HUM Ch 65.2.1 lists the Secure alias as `0x4034_6000` and the
 * Non-Secure alias as `0x5034_6000`. The bare-metal driver always
 * targets the Secure alias.
 */
typedef enum : uintptr_t {
  k_ra8_mipi_dsi_base_addr_s  = 0x40346000UL, /**< Secure-world MMIO base. */
  k_ra8_mipi_dsi_base_addr_ns = 0x50346000UL, /**< Non-Secure alias base.  */
} ra8_mipi_dsi_addr_t;

/**
 * @enum ra8_mipi_dsi_size_t
 * @brief Window size and key constants for the MIPI DSI block.
 */
typedef enum : uint16_t {
  k_ra8_mipi_dsi_window_bytes = 0x880U, /**< Total MMIO window (FSP size).  */
  k_ra8_mipi_dsi_max_lanes    = 2U,     /**< D-PHY lane configurations 1/2. */
  k_ra8_mipi_dsi_payload_max  = 16U,    /**< TXPPD bytes (4 x 32-bit regs). */
  k_ra8_mipi_dsi_rx_slots     = 4U,     /**< RXRSS0..3 receive slot count.  */
  k_ra8_mipi_dsi_descriptors  = 8U,     /**< 8 descriptors per channel.     */
  k_ra8_mipi_dsi_max_lp_bytes = 128U,   /**< LP (ch 0) max long-pkt size.   */
  k_ra8_mipi_dsi_max_hs_bytes = 1024U,  /**< HS (ch 1) max long-pkt size.   */
} ra8_mipi_dsi_size_t;

/**
 * @enum ra8_mipi_dsi_busy_loop_t
 * @brief Bounded poll iteration cap shared by every busy-wait helper.
 *
 * @details
 * Honours NASA Power of 10 Rule 2: every loop in the driver has a
 * statically provable upper bound. Tuned so the worst-case wait is
 * still under one ms at 1 GHz Cortex-M85, but small enough that the
 * unit tests on host don't spin meaningfully.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_busy_loop_max = 100000UL, /**< Max iterations per wait. */
} ra8_mipi_dsi_busy_loop_t;

/**
 * @enum ra8_mipi_dsi_rxrinfoowscr_t
 * @brief Bit masks for RXRINFOOWSCR (receive-result info-overwrite clear).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RXRINFOOWSR @ 0x238,
 * RXRINFOOWSCR @ 0x23C.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rxrinfoow_sl0 = 1UL << 0, /**< Slot 0 overwrite flag. */
  k_ra8_mipi_dsi_rxrinfoow_sl1 = 1UL << 1, /**< Slot 1 overwrite flag. */
  k_ra8_mipi_dsi_rxrinfoow_sl2 = 1UL << 2, /**< Slot 2 overwrite flag. */
  k_ra8_mipi_dsi_rxrinfoow_sl3 = 1UL << 3, /**< Slot 3 overwrite flag. */
} ra8_mipi_dsi_rxrinfoowscr_t;

/**
 * @enum ra8_mipi_dsi_isr_mask_t
 * @brief Bit masks for the top-level Interrupt Status Register (ISR).
 *
 * @details
 * HUM Ch 65.2.1 "ISR : Interrupt Status Register" p 3840.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_isr_sq0  = 1UL << 0,  /**< Sequence channel 0 interrupt. */
  k_ra8_mipi_dsi_isr_sq1  = 1UL << 4,  /**< Sequence channel 1 interrupt. */
  k_ra8_mipi_dsi_isr_vm   = 1UL << 8,  /**< Video-mode interrupt.         */
  k_ra8_mipi_dsi_isr_rcv  = 1UL << 12, /**< Receive interrupt.            */
  k_ra8_mipi_dsi_isr_ferr = 1UL << 16, /**< Fatal-error interrupt.        */
  k_ra8_mipi_dsi_isr_ppi  = 1UL << 20, /**< PPI (D-PHY) interrupt.        */
} ra8_mipi_dsi_isr_mask_t;

/**
 * @enum ra8_mipi_dsi_linksr_mask_t
 * @brief Bit masks for the Link Status Register (LINKSR).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- LINKSR @ 0x010, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_link_sq0run = 1UL << 0,  /**< Sequence ch 0 running.        */
  k_ra8_mipi_dsi_link_sq1run = 1UL << 4,  /**< Sequence ch 1 running.        */
  k_ra8_mipi_dsi_link_vrun   = 1UL << 8,  /**< Video-mode operation running. */
  k_ra8_mipi_dsi_link_hsbusy = 1UL << 12, /**< HS operation busy.            */
  k_ra8_mipi_dsi_link_lpbusy = 1UL << 13, /**< LP operation busy.            */
} ra8_mipi_dsi_linksr_mask_t;

/**
 * @enum ra8_mipi_dsi_txset_t
 * @brief Bit fields for TXSETR (lane configuration).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- TXSETR @ 0x100, p 3839-3934.
 * NUMLANE [1:0] = 0 -> 1 lane, 1 -> 2 lanes.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_txset_lane1 = 0UL << 0, /**< NUMLANE = 0: 1 data lane.  */
  k_ra8_mipi_dsi_txset_lane2 = 1UL << 0, /**< NUMLANE = 1: 2 data lanes. */
  k_ra8_mipi_dsi_txset_clen  = 1UL << 8, /**< CLEN: clock-lane enable.   */
  k_ra8_mipi_dsi_txset_dlen  = 1UL << 9, /**< DLEN: data-lane enable.    */
} ra8_mipi_dsi_txset_t;

/**
 * @enum ra8_mipi_dsi_hsclk_t
 * @brief Bit fields for HSCLKSETR.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- HSCLKSETR @ 0x104, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_hsclk_start      = 1UL << 0, /**< HSCLST: HS clock start.  */
  k_ra8_mipi_dsi_hsclk_continuous = 1UL << 1, /**< HSCLMD: continuous mode. */
} ra8_mipi_dsi_hsclk_t;

/**
 * @enum ra8_mipi_dsi_ulpscr_t
 * @brief Bit fields for ULPSCR.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- ULPSCR @ 0x10C, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_ulpscr_clent  = 1UL << 24, /**< Clock-lane ULPS enter. */
  k_ra8_mipi_dsi_ulpscr_clexit = 1UL << 25, /**< Clock-lane ULPS exit.  */
  k_ra8_mipi_dsi_ulpscr_dlent  = 1UL << 28, /**< Data-lane ULPS enter.  */
  k_ra8_mipi_dsi_ulpscr_dlexit = 1UL << 29, /**< Data-lane ULPS exit.   */
} ra8_mipi_dsi_ulpscr_t;

/**
 * @enum ra8_mipi_dsi_rstcr_t
 * @brief Bit fields for RSTCR (software reset control).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RSTCR @ 0x110, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rstcr_swrst  = 1UL << 0,  /**< Assert software reset. */
  k_ra8_mipi_dsi_rstcr_ftxstp = 1UL << 16, /**< Force Tx stop mode.    */
} ra8_mipi_dsi_rstcr_t;

/**
 * @enum ra8_mipi_dsi_rstsr_t
 * @brief Bit fields for RSTSR (software reset status).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RSTSR @ 0x114, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rstsr_rsths     = 1UL << 0,  /**< HS subsystem in reset.    */
  k_ra8_mipi_dsi_rstsr_rstlp     = 1UL << 1,  /**< LP subsystem in reset.    */
  k_ra8_mipi_dsi_rstsr_rstapb    = 1UL << 2,  /**< APB bridge in reset.      */
  k_ra8_mipi_dsi_rstsr_rstaxi    = 1UL << 3,  /**< AXI bridge in reset.      */
  k_ra8_mipi_dsi_rstsr_rstv      = 1UL << 4,  /**< Video subsystem in reset. */
  k_ra8_mipi_dsi_rstsr_dl0stp    = 1UL << 8,  /**< Data lane 0 stopped.      */
  k_ra8_mipi_dsi_rstsr_dl1stp    = 1UL << 9,  /**< Data lane 1 stopped.      */
  k_ra8_mipi_dsi_rstsr_dl0dir    = 1UL << 15, /**< Data lane 0 direction.    */
  k_ra8_mipi_dsi_rstsr_all_reset = (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 3) | (1UL << 4),
  /**< Composite mask: HS|LP|APB|AXI|V (all 5 reset bits). */
} ra8_mipi_dsi_rstsr_t;

/**
 * @enum ra8_mipi_dsi_dsisetr_t
 * @brief Bit fields for DSISETR (DSI link configuration).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- DSISETR @ 0x120, p 3839-3934.
 * MRPSZ[15:0] is the maximum return packet size (set as a value, not
 * a single-bit mask, so it is exposed as a shift).
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_dsisetr_eccen       = 1UL << 16, /**< ECC check enable.        */
  k_ra8_mipi_dsi_dsisetr_vc0crcen    = 1UL << 20, /**< VC-0 CRC check enable.   */
  k_ra8_mipi_dsi_dsisetr_vc1crcen    = 1UL << 21, /**< VC-1 CRC check enable.   */
  k_ra8_mipi_dsi_dsisetr_vc2crcen    = 1UL << 22, /**< VC-2 CRC check enable.   */
  k_ra8_mipi_dsi_dsisetr_vc3crcen    = 1UL << 23, /**< VC-3 CRC check enable.   */
  k_ra8_mipi_dsi_dsisetr_scren       = 1UL << 29, /**< Data scramble enable.    */
  k_ra8_mipi_dsi_dsisetr_extemd      = 1UL << 30, /**< External tearing detect. */
  k_ra8_mipi_dsi_dsisetr_eotpen      = 1UL << 31, /**< HS Tx EoTp enable.       */
  k_ra8_mipi_dsi_dsisetr_mrpsz_mask  = 0xFFFFUL,  /**< MRPSZ field mask.        */
  k_ra8_mipi_dsi_dsisetr_vc_crc_mask = (1UL << 20) | (1UL << 21) | (1UL << 22) | (1UL << 23),
  /**< Mask for the four per-VC CRC enable bits. */
} ra8_mipi_dsi_dsisetr_t;

/**
 * @enum ra8_mipi_dsi_rxsr_t
 * @brief Bit fields for RXSR / RXSCR / RXIER (receive status path).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RXSR @ 0x200, RXSCR @ 0x204,
 * RXIER @ 0x208, p 3839-3934. The same layout is used for status
 * (read-only), clear (RW1C), and interrupt enable.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rxsr_btarend  = 1UL << 0,  /**< BTA request end.           */
  k_ra8_mipi_dsi_rxsr_lrxhto   = 1UL << 1,  /**< LP-RX host processor TO.   */
  k_ra8_mipi_dsi_rxsr_tato     = 1UL << 2,  /**< Turnaround ack timeout.    */
  k_ra8_mipi_dsi_rxsr_rxresp   = 1UL << 8,  /**< Response packet received.  */
  k_ra8_mipi_dsi_rxsr_rxeotp   = 1UL << 10, /**< EoTp received.             */
  k_ra8_mipi_dsi_rxsr_rxte     = 1UL << 13, /**< Tearing-effect trigger rx. */
  k_ra8_mipi_dsi_rxsr_rxack    = 1UL << 14, /**< ACK trigger received.      */
  k_ra8_mipi_dsi_rxsr_extedet  = 1UL << 15, /**< External TE detect.        */
  k_ra8_mipi_dsi_rxsr_mlferr   = 1UL << 16, /**< Malform error.             */
  k_ra8_mipi_dsi_rxsr_eccerrm  = 1UL << 17, /**< Multi-bit ECC error.       */
  k_ra8_mipi_dsi_rxsr_unexerr  = 1UL << 18, /**< Unexpected packet error.   */
  k_ra8_mipi_dsi_rxsr_wcerr    = 1UL << 20, /**< Word count error.          */
  k_ra8_mipi_dsi_rxsr_crcerr   = 1UL << 21, /**< CRC error.                 */
  k_ra8_mipi_dsi_rxsr_iberr    = 1UL << 22, /**< Internal bus error.        */
  k_ra8_mipi_dsi_rxsr_rxovferr = 1UL << 23, /**< Receive buffer overflow.   */
  k_ra8_mipi_dsi_rxsr_prtoerr  = 1UL << 24, /**< Peripheral resp timeout.   */
  k_ra8_mipi_dsi_rxsr_noreserr = 1UL << 25, /**< No response error.         */
  k_ra8_mipi_dsi_rxsr_rsizeerr = 1UL << 26, /**< Return packet size error.  */
  k_ra8_mipi_dsi_rxsr_eccerrs  = 1UL << 28, /**< Single-bit ECC error.      */
  k_ra8_mipi_dsi_rxsr_rxake    = 1UL << 30, /**< Ack-and-error report rx.   */
  k_ra8_mipi_dsi_rxsr_clear_all =
    (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 8) | (1UL << 10) | (1UL << 13) | (1UL << 14) |
    (1UL << 15) | (1UL << 16) | (1UL << 17) | (1UL << 18) | (1UL << 20) | (1UL << 21) |
    (1UL << 22) | (1UL << 23) | (1UL << 24) | (1UL << 25) | (1UL << 26) | (1UL << 28) | (1UL << 30),
  /**< Mask of every defined RXSR bit -- write to RXSCR to scrub. */
} ra8_mipi_dsi_rxsr_t;

/**
 * @enum ra8_mipi_dsi_rxrssr_t
 * @brief Bit fields for RXRSSR / RXRSSCR (receive-result slot valid).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RXRSSR @ 0x230, RXRSSCR @ 0x234.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rxrssr_slt0vld = 1UL << 0, /**< Slot 0 valid flag. */
  k_ra8_mipi_dsi_rxrssr_slt1vld = 1UL << 1, /**< Slot 1 valid flag. */
  k_ra8_mipi_dsi_rxrssr_slt2vld = 1UL << 2, /**< Slot 2 valid flag. */
  k_ra8_mipi_dsi_rxrssr_slt3vld = 1UL << 3, /**< Slot 3 valid flag. */
} ra8_mipi_dsi_rxrssr_t;

/**
 * @enum ra8_mipi_dsi_rxrss_t
 * @brief Decoded fields for an RXRSS slot value (header capture).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- RXRSS0..3R @ 0x240..0x24C.
 * Each 32-bit slot register packs a captured DSI packet header:
 * DATA0[7:0], DATA1[15:8], DT[21:16], VC[23:22], FMT[24], status flags
 * RXSUC[25], RXFERR[26], RXFAIL[27], RXPFAIL[28], RXCERR[29],
 * RXAKE[30], INFOOW[31].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_rxrss_data0_mask = 0x000000FFUL, /**< DATA0 byte 1 of pkt. */
  k_ra8_mipi_dsi_rxrss_data1_mask = 0x0000FF00UL, /**< DATA1 byte 2 of pkt. */
  k_ra8_mipi_dsi_rxrss_dt_mask    = 0x003F0000UL, /**< DT data type bits.   */
  k_ra8_mipi_dsi_rxrss_vc_mask    = 0x00C00000UL, /**< Virtual channel.     */
  k_ra8_mipi_dsi_rxrss_fmt        = 1UL << 24,    /**< Long-packet flag.    */
  k_ra8_mipi_dsi_rxrss_rxsuc      = 1UL << 25,    /**< Receive success.     */
  k_ra8_mipi_dsi_rxrss_rxferr     = 1UL << 26,    /**< Fatal error.         */
  k_ra8_mipi_dsi_rxrss_rxfail     = 1UL << 27,    /**< Receive fail.        */
  k_ra8_mipi_dsi_rxrss_rxpfail    = 1UL << 28,    /**< Packet data fail.    */
  k_ra8_mipi_dsi_rxrss_rxcerr     = 1UL << 29,    /**< Correctable error.   */
  k_ra8_mipi_dsi_rxrss_rxake      = 1UL << 30,    /**< Ack/error report.    */
  k_ra8_mipi_dsi_rxrss_infoow     = 1UL << 31,    /**< Info overwritten.    */
} ra8_mipi_dsi_rxrss_t;

/**
 * @enum ra8_mipi_dsi_akep_t
 * @brief Bit fields for AKEPLATIR / AKEPACMSR / AKEPSCR.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- AKEPLATIR @ 0x220,
 * AKEPACMSR @ 0x224, AKEPSCR @ 0x228, p 3839-3934. EREP[15:0] is the
 * 16-bit DSI ack/error-report bitmap, VC[19:16] tags the virtual
 * channel that issued the report.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_akep_erep_mask = 0xFFFFUL,    /**< 16-bit error report. */
  k_ra8_mipi_dsi_akep_vc_mask   = 0xFUL << 16, /**< Virtual channel id.  */
} ra8_mipi_dsi_akep_t;

/**
 * @enum ra8_mipi_dsi_ferrsr_t
 * @brief Bit fields for FERRSR / FERRSCR / FERRIER (fatal errors).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- FERRSR @ 0x300, FERRSCR @ 0x304,
 * FERRIER @ 0x308, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_ferrsr_htxto     = 1UL << 0,  /**< HS Tx timeout.              */
  k_ra8_mipi_dsi_ferrsr_lrxhto    = 1UL << 1,  /**< LP-RX host timeout.         */
  k_ra8_mipi_dsi_ferrsr_tato      = 1UL << 2,  /**< Turnaround ack timeout.     */
  k_ra8_mipi_dsi_ferrsr_escent    = 1UL << 16, /**< Escape-mode entry error.    */
  k_ra8_mipi_dsi_ferrsr_syncesc   = 1UL << 17, /**< LPDT sync error.            */
  k_ra8_mipi_dsi_ferrsr_ctrl      = 1UL << 18, /**< Control error.              */
  k_ra8_mipi_dsi_ferrsr_clp0      = 1UL << 19, /**< LP0 contention error.       */
  k_ra8_mipi_dsi_ferrsr_clp1      = 1UL << 20, /**< LP1 contention error.       */
  k_ra8_mipi_dsi_ferrsr_clp0s     = 1UL << 27, /**< LP0 contention status (RO). */
  k_ra8_mipi_dsi_ferrsr_clp1s     = 1UL << 28, /**< LP1 contention status (RO). */
  k_ra8_mipi_dsi_ferrsr_clear_all = (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 16) |
                                    (1UL << 17) | (1UL << 18) | (1UL << 19) | (1UL << 20),
  /**< RW1C-able fields in FERRSCR (status-only bits 27/28 excluded). */
} ra8_mipi_dsi_ferrsr_t;

/**
 * @enum ra8_mipi_dsi_plsr_t
 * @brief Bit fields for PLSR / PLSCR / PLIER (PHY/PPI lane status).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- PLSR @ 0x320, PLSCR @ 0x324,
 * PLIER @ 0x328, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_plsr_cluan     = 1UL << 0,  /**< CL UlpsActiveNot status. */
  k_ra8_mipi_dsi_plsr_clstp     = 1UL << 1,  /**< CL stop status.          */
  k_ra8_mipi_dsi_plsr_dl0rle    = 1UL << 2,  /**< DL0 RxLpdtEsc status.    */
  k_ra8_mipi_dsi_plsr_dl0rue    = 1UL << 3,  /**< DL0 RxUlpsEsc status.    */
  k_ra8_mipi_dsi_plsr_dl0uan    = 1UL << 4,  /**< DL0 UlpsActiveNot.       */
  k_ra8_mipi_dsi_plsr_dl1uan    = 1UL << 5,  /**< DL1 UlpsActiveNot.       */
  k_ra8_mipi_dsi_plsr_dl0stp    = 1UL << 8,  /**< DL0 stop status.         */
  k_ra8_mipi_dsi_plsr_dl1stp    = 1UL << 9,  /**< DL1 stop status.         */
  k_ra8_mipi_dsi_plsr_dl0rx2tx  = 1UL << 12, /**< DL0 RX-to-TX transition. */
  k_ra8_mipi_dsi_plsr_dl0tx2rx  = 1UL << 13, /**< DL0 TX-to-RX transition. */
  k_ra8_mipi_dsi_plsr_dl0dir    = 1UL << 15, /**< DL0 direction.           */
  k_ra8_mipi_dsi_plsr_clulpent  = 1UL << 24, /**< CL ULPS enter event.     */
  k_ra8_mipi_dsi_plsr_clulpext  = 1UL << 25, /**< CL ULPS exit event.      */
  k_ra8_mipi_dsi_plsr_cllp2hs   = 1UL << 26, /**< CL LP-to-HS transition.  */
  k_ra8_mipi_dsi_plsr_clhs2lp   = 1UL << 27, /**< CL HS-to-LP transition.  */
  k_ra8_mipi_dsi_plsr_dlulpent  = 1UL << 28, /**< DL ULPS enter event.     */
  k_ra8_mipi_dsi_plsr_dlulpext  = 1UL << 29, /**< DL ULPS exit event.      */
  k_ra8_mipi_dsi_plsr_clear_all = (1UL << 12) | (1UL << 13) | (1UL << 24) | (1UL << 25) |
                                  (1UL << 26) | (1UL << 27) | (1UL << 28) | (1UL << 29),
  /**< Bit subset that is RW1C in PLSCR (steady-state lane bits aren't). */
} ra8_mipi_dsi_plsr_t;

/**
 * @enum ra8_mipi_dsi_vmset0_t
 * @brief Bit fields for VMSET0R (video-mode set 0).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMSET0R @ 0x400, p 3839-3934.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmset0_vstart  = 1UL << 0,  /**< VSTART: start video mode.   */
  k_ra8_mipi_dsi_vmset0_vstop   = 1UL << 1,  /**< VSTOP : request video stop. */
  k_ra8_mipi_dsi_vmset0_hsanolp = 1UL << 8,  /**< HSA period stays HS.        */
  k_ra8_mipi_dsi_vmset0_hbpnolp = 1UL << 9,  /**< HBP period stays HS.        */
  k_ra8_mipi_dsi_vmset0_hfpnolp = 1UL << 10, /**< HFP period stays HS.        */
} ra8_mipi_dsi_vmset0_t;

/**
 * @enum ra8_mipi_dsi_vmset1_t
 * @brief Bit fields for VMSET1R (video-mode set 1: pipeline delay).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMSET1R @ 0x404, p 3839-3934.
 * DLY[13:2] -- 12-bit delay value, scaled by 4 by hardware.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmset1_dly_shift = 2U,       /**< DLY position.         */
  k_ra8_mipi_dsi_vmset1_dly_mask  = 0x3FFCUL, /**< DLY[13:2] field mask. */
} ra8_mipi_dsi_vmset1_t;

/**
 * @enum ra8_mipi_dsi_vmsr_t
 * @brief Bit fields for VMSR / VMSCR / VMIER.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMSR @ 0x410, VMSCR @ 0x414,
 * VMIER @ 0x418, p 3839-3934. NOTE the VMSR layout from the FSP shows
 * `start[0] / stop[1] / running[2] / virdy[3]` but the HUM places
 * RUNNING at bit 0 and VIRDY at bit 4. We follow the HUM (and
 * downstream test fixtures) which use the latter layout.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmsr_running = 1UL << 0,  /**< Video mode running.         */
  k_ra8_mipi_dsi_vmsr_virdy   = 1UL << 4,  /**< Video mode ready (started). */
  k_ra8_mipi_dsi_vmsr_stop    = 1UL << 8,  /**< Video mode stopped.         */
  k_ra8_mipi_dsi_vmsr_timerr  = 1UL << 20, /**< Timing error.               */
  k_ra8_mipi_dsi_vmsr_vbufudf = 1UL << 22, /**< Video buffer underflow.     */
  k_ra8_mipi_dsi_vmsr_vbufovf = 1UL << 23, /**< Video buffer overflow.      */
  k_ra8_mipi_dsi_vmsr_clear_all =
    (1UL << 0) | (1UL << 4) | (1UL << 8) | (1UL << 20) | (1UL << 22) | (1UL << 23),
  /**< Mask of every defined VMSR bit -- write to VMSCR to scrub. */
} ra8_mipi_dsi_vmsr_t;

/**
 * @enum ra8_mipi_dsi_vmppsetr_t
 * @brief Bit fields for VMPPSETR (video-mode pixel-packet set).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMPPSETR @ 0x420.
 * TXESYNC[15], DT[21:16], VC[23:22].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmpp_txesync  = 1UL << 15, /**< Transmit end-of-sync */
  k_ra8_mipi_dsi_vmpp_dt_shift = 16U,       /**< DT field position.   */
  k_ra8_mipi_dsi_vmpp_dt_mask  = 0x3FUL << 16,
  /**< DT[21:16] mask, used to clamp the data-type field. */
  k_ra8_mipi_dsi_vmpp_vc_shift = 22U, /**< VC field position. */
  k_ra8_mipi_dsi_vmpp_vc_mask  = 0x3UL << 22,
  /**< VC[23:22] mask, used to clamp the virtual-channel field. */
} ra8_mipi_dsi_vmppsetr_t;

/**
 * @enum ra8_mipi_dsi_vmvssetr_t
 * @brief Bit fields for VMVSSETR (vertical sync timing).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMVSSETR @ 0x428.
 * VSA[11:0], VSPOL[15], VACT[30:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmvs_vsa_mask   = 0x00000FFFUL,   /**< VSA[11:0].      */
  k_ra8_mipi_dsi_vmvs_vspol      = 1UL << 15,      /**< VSYNC polarity. */
  k_ra8_mipi_dsi_vmvs_vact_shift = 16U,            /**< VACT shift.     */
  k_ra8_mipi_dsi_vmvs_vact_mask  = 0x7FFFUL << 16, /**< VACT[30:16].    */
} ra8_mipi_dsi_vmvssetr_t;

/**
 * @enum ra8_mipi_dsi_vmvpsetr_t
 * @brief Bit fields for VMVPSETR (vertical porch timing).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMVPSETR @ 0x42C.
 * VBP[12:0], VFP[28:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmvp_vbp_mask  = 0x00001FFFUL,   /**< VBP[12:0].  */
  k_ra8_mipi_dsi_vmvp_vfp_shift = 16U,            /**< VFP shift.  */
  k_ra8_mipi_dsi_vmvp_vfp_mask  = 0x1FFFUL << 16, /**< VFP[28:16]. */
} ra8_mipi_dsi_vmvpsetr_t;

/**
 * @enum ra8_mipi_dsi_vmhssetr_t
 * @brief Bit fields for VMHSSETR (horizontal sync timing).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMHSSETR @ 0x430.
 * HSA[11:0], HSPOL[15], HACT[30:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmhs_hsa_mask   = 0x00000FFFUL,   /**< HSA[11:0].      */
  k_ra8_mipi_dsi_vmhs_hspol      = 1UL << 15,      /**< HSYNC polarity. */
  k_ra8_mipi_dsi_vmhs_hact_shift = 16U,            /**< HACT shift.     */
  k_ra8_mipi_dsi_vmhs_hact_mask  = 0x7FFFUL << 16, /**< HACT[30:16].    */
} ra8_mipi_dsi_vmhssetr_t;

/**
 * @enum ra8_mipi_dsi_vmhpsetr_t
 * @brief Bit fields for VMHPSETR (horizontal porch timing).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- VMHPSETR @ 0x434.
 * HBP[12:0], HFP[28:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_vmhp_hbp_mask  = 0x00001FFFUL,   /**< HBP[12:0].  */
  k_ra8_mipi_dsi_vmhp_hfp_shift = 16U,            /**< HFP shift.  */
  k_ra8_mipi_dsi_vmhp_hfp_mask  = 0x1FFFUL << 16, /**< HFP[28:16]. */
} ra8_mipi_dsi_vmhpsetr_t;

/**
 * @enum ra8_mipi_dsi_clstpt_t
 * @brief Bit fields for CLSTPTSETR (clock-lane stop / beforehand / keep).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- CLSTPTSETR @ 0x314.
 * CLKSTPT[11:2], CLKBFHT[23:16], CLKKPT[31:24].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_clstpt_clkstpt_shift = 2U,      /**< CLKSTPT pos.   */
  k_ra8_mipi_dsi_clstpt_clkstpt_mask  = 0xFFCUL, /**< CLKSTPT[11:2]. */
  k_ra8_mipi_dsi_clstpt_clkbfht_shift = 16U,     /**< CLKBFHT pos.   */
  k_ra8_mipi_dsi_clstpt_clkbfht_mask  = 0xFFUL << 16,
  /**< CLKBFHT[23:16] mask, sets the LP-to-HS clock guard time. */
  k_ra8_mipi_dsi_clstpt_clkkpt_shift = 24U, /**< CLKKPT pos. */
  k_ra8_mipi_dsi_clstpt_clkkpt_mask  = 0xFFUL << 24,
  /**< CLKKPT[31:24] mask, sets the HS-to-LP clock keep time. */
} ra8_mipi_dsi_clstpt_t;

/**
 * @enum ra8_mipi_dsi_lptrnst_t
 * @brief Bit fields for LPTRNSTSETR (LP transition timing).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- LPTRNSTSETR @ 0x318.
 * GOLPBKT[9:0].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_lptrnst_golpbkt_mask = 0x3FFUL, /**< GOLPBKT[9:0]. */
} ra8_mipi_dsi_lptrnst_t;

/**
 * @enum ra8_mipi_dsi_presptolp_t
 * @brief Bit fields for PRESPTOLPSETR (LP read/write timeouts).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- PRESPTOLPSETR @ 0x214.
 * LPWTO[15:0], LPRTO[31:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_presptolp_lpwto_mask  = 0xFFFFUL, /**< LPWTO[15:0].    */
  k_ra8_mipi_dsi_presptolp_lprto_shift = 16U,      /**< LPRTO position. */
  k_ra8_mipi_dsi_presptolp_lprto_mask  = 0xFFFFUL << 16,
  /**< LPRTO[31:16] mask, sets the LP-mode peripheral read timeout. */
} ra8_mipi_dsi_presptolp_t;

/**
 * @enum ra8_mipi_dsi_presptohs_t
 * @brief Bit fields for PRESPTOHSSETR (HS read/write timeouts).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- PRESPTOHSSETR @ 0x218.
 * HSWTO[15:0], HSRTO[31:16].
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_presptohs_hswto_mask  = 0xFFFFUL, /**< HSWTO[15:0].    */
  k_ra8_mipi_dsi_presptohs_hsrto_shift = 16U,      /**< HSRTO position. */
  k_ra8_mipi_dsi_presptohs_hsrto_mask  = 0xFFFFUL << 16,
  /**< HSRTO[31:16] mask, sets the HS-mode peripheral read timeout. */
} ra8_mipi_dsi_presptohs_t;

/**
 * @enum ra8_mipi_dsi_sqch_t
 * @brief Bit fields for SQCH0SET0R / SQCH1SET0R / SQCH*SR.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- SQCH0SET0R @ 0x5C0,
 * SQCH0SR @ 0x5D0, SQCH1SET0R @ 0x600, SQCH1SR @ 0x610. Same layout
 * applies to both channels and to the corresponding SCR / IER
 * registers.
 *
 * NOTE on SQCH*SET0R: the FSP source emits bit 23 alongside the START
 * bit (`SQCHnSET0R_BIT_23 | (n == channel)`). That extra bit is the
 * channel select latch. Both bits are exposed here.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_sqch_start     = 1UL << 0,  /**< START: kick sequence.       */
  k_ra8_mipi_dsi_sqch_chsel     = 1UL << 23, /**< Channel-select bit (FSP).   */
  k_ra8_mipi_dsi_sqch_running   = 1UL << 2,  /**< RUNNING flag in *SR.        */
  k_ra8_mipi_dsi_sqch_aactfin   = 1UL << 4,  /**< AACTFIN: all actions done.  */
  k_ra8_mipi_dsi_sqch_adesfin   = 1UL << 8,  /**< ADESFIN: all descriptors.   */
  k_ra8_mipi_dsi_sqch_dabort    = 1UL << 16, /**< DABORT: descriptor abort.   */
  k_ra8_mipi_dsi_sqch_sizeerr   = 1UL << 19, /**< SIZEERR: packet size error. */
  k_ra8_mipi_dsi_sqch_txiberr   = 1UL << 24, /**< TXIBERR: Tx int-bus error.  */
  k_ra8_mipi_dsi_sqch_rxferr    = 1UL << 26, /**< RXFERR: Rx fatal error.     */
  k_ra8_mipi_dsi_sqch_rxfail    = 1UL << 27, /**< RXFAIL: Rx fail.            */
  k_ra8_mipi_dsi_sqch_rxpfail   = 1UL << 28, /**< RXPFAIL: Rx packet fail.    */
  k_ra8_mipi_dsi_sqch_rxcorerr  = 1UL << 29, /**< RXCORERR: Rx correctable.   */
  k_ra8_mipi_dsi_sqch_rxake     = 1UL << 30, /**< RXAKE: ack/error report rx. */
  k_ra8_mipi_dsi_sqch_clear_all = (1UL << 4) | (1UL << 8) | (1UL << 16) | (1UL << 19) |
                                  (1UL << 24) | (1UL << 26) | (1UL << 27) | (1UL << 28) |
                                  (1UL << 29) | (1UL << 30),
  /**< Mask of every RW1C bit in SQCH*SCR. */
} ra8_mipi_dsi_sqch_t;

/**
 * @enum ra8_mipi_dsi_dsc0a_shift_t
 * @brief Bit shifts for SQCH*DSC?AR (descriptor word A).
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- SQCH0DSC0AR @ 0x780.
 * Bit layout: [DATA0:8][DATA1:8][DT:6][VC:2][FMT:1][SPD:1][BTA:2]
 *             [NXACT:2][rsvd:2]. Used to assemble a generic DSI
 *             short or long packet header in software.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_dsc0a_shift_data0 = 0U,  /**< Byte 1 of short packet.     */
  k_ra8_mipi_dsi_dsc0a_shift_data1 = 8U,  /**< Byte 2 of short packet.     */
  k_ra8_mipi_dsi_dsc0a_shift_dt    = 16U, /**< DSI data type (cmd id).     */
  k_ra8_mipi_dsi_dsc0a_shift_vc    = 22U, /**< Virtual channel.            */
  k_ra8_mipi_dsi_dsc0a_shift_fmt   = 24U, /**< 0 = short packet, 1 = long. */
  k_ra8_mipi_dsi_dsc0a_shift_spd   = 25U, /**< 0 = HS, 1 = LP escape.      */
  k_ra8_mipi_dsi_dsc0a_shift_bta   = 26U, /**< Bus Turn Around bits.       */
  k_ra8_mipi_dsi_dsc0a_shift_nxact = 28U, /**< Next-action bits.           */
} ra8_mipi_dsi_dsc0a_shift_t;

/**
 * @enum ra8_mipi_dsi_dsc0b_t
 * @brief SQCH*DSC?BR (descriptor word B) DTSEL field.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- SQCH0DSC0BR @ 0x784.
 * DTSEL[25:24]: 0 = TXPPD, 1 = sequence RAM.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_dsc0b_dtsel_txppd = 0UL << 24, /**< Use TXPPD0..3R buffer. */
  k_ra8_mipi_dsi_dsc0b_dtsel_seqrm = 1UL << 24, /**< Use sequence RAM.      */
} ra8_mipi_dsi_dsc0b_t;

/**
 * @enum ra8_mipi_dsi_dsc0c_t
 * @brief SQCH*DSC?CR (descriptor word C) bit fields.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- SQCH0DSC0CR @ 0x788.
 * FINACT[0], AUXOP[22], ACTCODE[31:24]. AUXOP signals an auxiliary
 * operation (BTA / skew calibration / NOOP); ACTCODE picks the
 * specific aux command.
 */
typedef enum : uint32_t {
  k_ra8_mipi_dsi_dsc0c_finact        = 1UL << 0,     /**< Finish action flag. */
  k_ra8_mipi_dsi_dsc0c_auxop         = 1UL << 22,    /**< Auxiliary op flag.  */
  k_ra8_mipi_dsi_dsc0c_actcode_shift = 24U,          /**< Action code shift.  */
  k_ra8_mipi_dsi_dsc0c_actcode_mask  = 0xFFUL << 24, /**< ACTCODE[31:24].     */
} ra8_mipi_dsi_dsc0c_t;

/**
 * @enum ra8_mipi_dsi_bta_t
 * @brief Bus Turn Around (BTA) selector for descriptor word A.
 *
 * @details
 * HUM Ch 65.2 "Register Description" -- SQCH0DSC0AR.BTA[27:26].
 * Mirrors `MIPI_DSI_CMD_FLAG_BTA*` from the FSP.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_bta_none     = 0U, /**< No bus-turnaround.                  */
  k_ra8_mipi_dsi_bta_after    = 1U, /**< BTA after Tx (default ack request). */
  k_ra8_mipi_dsi_bta_read     = 2U, /**< BTA followed by read.               */
  k_ra8_mipi_dsi_bta_no_write = 3U, /**< Immediate BTA, no write first.      */
} ra8_mipi_dsi_bta_t;

/**
 * @struct r_mipi_dsi_descriptor_t
 * @brief One sequence-channel descriptor slot (4 x 32-bit registers).
 *
 * @details
 * The MIPI DSI host stores 8 of these per sequence channel
 * (descriptors 0..7) at the SQCH0DSC@ / SQCH1DSC@ blocks. Word A is
 * the packet header, B selects payload source, C carries auxiliary
 * action codes, D is the lower address of an external payload buffer.
 */
typedef struct {
  volatile uint32_t A; /**< Descriptor word A: packet header.          */
  volatile uint32_t B; /**< Descriptor word B: payload select.         */
  volatile uint32_t C; /**< Descriptor word C: action / finish.        */
  volatile uint32_t D; /**< Descriptor word D: payload buffer address. */
} r_mipi_dsi_descriptor_t;

/**
 * @enum ra8_mipi_dsi_reserved_words_t
 * @brief Reserved-word array sizes (in 32-bit words) used by the host
 *        register block to keep named offsets aligned with HUM Ch 65.
 *
 * @details
 * Each value corresponds to a reserved gap between named registers.
 * Replaces inline magic numbers in the `r_mipi_dsi_regs_t` struct.
 */
typedef enum : uint8_t {
  k_ra8_mipi_dsi_rsv_words_isr_to_linksr    = 3U,  /**< +0x004..0x00C gap. */
  k_ra8_mipi_dsi_rsv_words_linksr_to_txsetr = 59U, /**< +0x014..0x0FC gap. */
  k_ra8_mipi_dsi_rsv_words_dsisetr_to_txppd = 15U, /**< +0x124..0x15C gap. */
  k_ra8_mipi_dsi_rsv_words_txppd_to_rxsr    = 36U, /**< +0x170..0x1FC gap. */
  k_ra8_mipi_dsi_rsv_words_rxrss_to_rxppd   = 28U, /**< +0x250..0x2BC gap. */
  k_ra8_mipi_dsi_rsv_words_rxppd_to_hstxto  = 4U,  /**< +0x2D0..0x2DC gap. */
  k_ra8_mipi_dsi_rsv_words_tato_to_ferrsr   = 5U,  /**< +0x2EC..0x2FC gap. */
  k_ra8_mipi_dsi_rsv_words_plier_to_vmset   = 53U, /**< +0x32C..0x3FC gap. */
  k_ra8_mipi_dsi_rsv_words_hsporch_to_sqch0 = 98U, /**< +0x438..0x5BC gap. */
  k_ra8_mipi_dsi_rsv_words_sqch0_set0_to_sr = 3U,  /**< +0x5C4..0x5CC gap. */
  k_ra8_mipi_dsi_rsv_words_sqch0_ier_to_ch1 = 9U,  /**< +0x5DC..0x5FC gap. */
  k_ra8_mipi_dsi_rsv_words_sqch1_ier_to_dsc = 89U, /**< +0x61C..0x77C gap. */
} ra8_mipi_dsi_reserved_words_t;

/**
 * @enum ra8_mipi_dsi_offset_t
 * @brief Verified MMIO offsets (in bytes) of select named registers.
 *
 * @details
 * Used by `static_assert` declarations below to lock layout to the
 * HUM/FSP register window definition.
 */
typedef enum : uint16_t {
  k_ra8_mipi_dsi_offset_akeplatir = 0x220U, /**< AKEPLATIR offset.   */
  k_ra8_mipi_dsi_offset_vmppsetr  = 0x420U, /**< VMPPSETR offset.    */
  k_ra8_mipi_dsi_offset_sqch0dsc  = 0x780U, /**< SQCH0DSC[0] offset. */
  k_ra8_mipi_dsi_offset_sqch1dsc  = 0x800U, /**< SQCH1DSC[0] offset. */
} ra8_mipi_dsi_offset_t;

/**
 * @struct r_mipi_dsi_regs_t
 * @brief MIPI DSI host register block (full layout).
 *
 * @details
 * Every documented register in HUM Ch 65 has a named field. The
 * intervening reserved arrays hold the exact byte count to keep the
 * named offsets aligned with the FSP `R_MIPI_DSI_Type` layout. Total
 * struct size matches the FSP `Size = 2176 (0x880)` declaration so a
 * `static_assert` below catches any accidental drift.
 *
 * @invariant `sizeof(r_mipi_dsi_regs_t) == k_ra8_mipi_dsi_window_bytes`
 */
typedef struct {
  volatile uint32_t ISR;                                         /**< +0x000 IRQ status (RO).  */
  volatile uint32_t _r0[k_ra8_mipi_dsi_rsv_words_isr_to_linksr]; /**< +0x004..0x00C reserved.  */
  volatile uint32_t LINKSR;                                      /**< +0x010 Link status (RO). */
  volatile uint32_t
    _r1[k_ra8_mipi_dsi_rsv_words_linksr_to_txsetr]; /**< +0x014..0x0FC reserved.            */
  volatile uint32_t TXSETR;                         /**< +0x100 Lane enable + count.        */
  volatile uint32_t HSCLKSETR;                      /**< +0x104 HS clock control.           */
  volatile uint32_t ULPSSETR;                       /**< +0x108 ULPS wakeup period.         */
  volatile uint32_t ULPSCR;                         /**< +0x10C ULPS enter/exit pulses.     */
  volatile uint32_t RSTCR;                          /**< +0x110 Software reset control.     */
  volatile uint32_t RSTSR;                          /**< +0x114 Software reset status (RO). */
  volatile uint32_t _r2[2];                         /**< +0x118..0x11C reserved.            */
  volatile uint32_t DSISETR;                        /**< +0x120 DSI link configuration.     */
  volatile uint32_t
    _r3[k_ra8_mipi_dsi_rsv_words_dsisetr_to_txppd]; /**< +0x124..0x15C reserved.          */
  volatile uint32_t TXPPD0R;                        /**< +0x160 Tx packet payload data 0. */
  volatile uint32_t TXPPD1R;                        /**< +0x164 Tx packet payload data 1. */
  volatile uint32_t TXPPD2R;                        /**< +0x168 Tx packet payload data 2. */
  volatile uint32_t TXPPD3R;                        /**< +0x16C Tx packet payload data 3. */
  volatile uint32_t
    _r4[k_ra8_mipi_dsi_rsv_words_txppd_to_rxsr]; /**< +0x170..0x1FC reserved.            */
  volatile uint32_t RXSR;                        /**< +0x200 Receive status (RO).        */
  volatile uint32_t RXSCR;                       /**< +0x204 Receive status clear.       */
  volatile uint32_t RXIER;                       /**< +0x208 Receive interrupt enable.   */
  volatile uint32_t _r5;                         /**< +0x20C reserved.                   */
  volatile uint32_t PRESPTOBTASETR;              /**< +0x210 Peripheral resp TO (BTA).   */
  volatile uint32_t PRESPTOLPSETR;               /**< +0x214 Peripheral resp TO (LP).    */
  volatile uint32_t PRESPTOHSSETR;               /**< +0x218 Peripheral resp TO (HS).    */
  volatile uint32_t _r6;                         /**< +0x21C reserved.                   */
  volatile uint32_t AKEPLATIR;                   /**< +0x220 Ack/err report latest (RO). */
  volatile uint32_t AKEPACMSR;                   /**< +0x224 Ack/err accumulated (RO).   */
  volatile uint32_t AKEPSCR;                     /**< +0x228 Ack/err clear.              */
  volatile uint32_t _r7;                         /**< +0x22C reserved.                   */
  volatile uint32_t RXRSSR;                      /**< +0x230 Rx result save status.      */
  volatile uint32_t RXRSSCR;                     /**< +0x234 Rx result save clear.       */
  volatile uint32_t RXRINFOOWSR;                 /**< +0x238 Rx info-overwrite status.   */
  volatile uint32_t RXRINFOOWSCR;                /**< +0x23C Rx info-overwrite clear.    */
  volatile uint32_t RXRSS0R;                     /**< +0x240 Rx result save slot 0.      */
  volatile uint32_t RXRSS1R;                     /**< +0x244 Rx result save slot 1.      */
  volatile uint32_t RXRSS2R;                     /**< +0x248 Rx result save slot 2.      */
  volatile uint32_t RXRSS3R;                     /**< +0x24C Rx result save slot 3.      */
  volatile uint32_t
    _r8[k_ra8_mipi_dsi_rsv_words_rxrss_to_rxppd]; /**< +0x250..0x2BC reserved.          */
  volatile uint32_t RXPPD0R;                      /**< +0x2C0 Rx packet payload data 0. */
  volatile uint32_t RXPPD1R;                      /**< +0x2C4 Rx packet payload data 1. */
  volatile uint32_t RXPPD2R;                      /**< +0x2C8 Rx packet payload data 2. */
  volatile uint32_t RXPPD3R;                      /**< +0x2CC Rx packet payload data 3. */
  volatile uint32_t
    _r9[k_ra8_mipi_dsi_rsv_words_rxppd_to_hstxto]; /**< +0x2D0..0x2DC reserved.        */
  volatile uint32_t HSTXTOSETR;                    /**< +0x2E0 HS Tx timeout count.    */
  volatile uint32_t LRXHTOSETR;                    /**< +0x2E4 LP-RX host timeout.     */
  volatile uint32_t TATOSETR;                      /**< +0x2E8 Turnaround ack timeout. */
  volatile uint32_t
    _r10[k_ra8_mipi_dsi_rsv_words_tato_to_ferrsr]; /**< +0x2EC..0x2FC reserved.          */
  volatile uint32_t FERRSR;                        /**< +0x300 Fatal-error status (RO).  */
  volatile uint32_t FERRSCR;                       /**< +0x304 Fatal-error status clear. */
  volatile uint32_t FERRIER;                       /**< +0x308 Fatal-error int enable.   */
  volatile uint32_t _r11[2];                       /**< +0x30C..0x310 reserved.          */
  volatile uint32_t CLSTPTSETR;                    /**< +0x314 Clock-lane stop time.     */
  volatile uint32_t LPTRNSTSETR;                   /**< +0x318 LP transition time.       */
  volatile uint32_t _r12;                          /**< +0x31C reserved.                 */
  volatile uint32_t PLSR;                          /**< +0x320 PHY (PPI) status (RO).    */
  volatile uint32_t PLSCR;                         /**< +0x324 PHY status clear.         */
  volatile uint32_t PLIER;                         /**< +0x328 PHY interrupt enable.     */
  volatile uint32_t
    _r13[k_ra8_mipi_dsi_rsv_words_plier_to_vmset]; /**< +0x32C..0x3FC reserved.         */
  volatile uint32_t VMSET0R;                       /**< +0x400 Video-mode set 0.        */
  volatile uint32_t VMSET1R;                       /**< +0x404 Video-mode set 1.        */
  volatile uint32_t _r14[2];                       /**< +0x408..0x40C reserved.         */
  volatile uint32_t VMSR;                          /**< +0x410 Video-mode status (RO).  */
  volatile uint32_t VMSCR;                         /**< +0x414 Video-mode status clear. */
  volatile uint32_t VMIER;                         /**< +0x418 Video-mode int enable.   */
  volatile uint32_t _r15;                          /**< +0x41C reserved.                */
  volatile uint32_t VMPPSETR;                      /**< +0x420 Video pixel-packet set.  */
  volatile uint32_t _r16;                          /**< +0x424 reserved.                */
  volatile uint32_t VMVSSETR;                      /**< +0x428 Vertical sync timing.    */
  volatile uint32_t VMVPSETR;                      /**< +0x42C Vertical porch timing.   */
  volatile uint32_t VMHSSETR;                      /**< +0x430 Horizontal sync timing.  */
  volatile uint32_t VMHPSETR;                      /**< +0x434 Horizontal porch timing. */
  volatile uint32_t
    _r17[k_ra8_mipi_dsi_rsv_words_hsporch_to_sqch0]; /**< +0x438..0x5BC reserved.         */
  volatile uint32_t SQCH0SET0R;                      /**< +0x5C0 Sequence ch 0 START bit. */
  volatile uint32_t
    _r18[k_ra8_mipi_dsi_rsv_words_sqch0_set0_to_sr]; /**< +0x5C4..0x5CC reserved.           */
  volatile uint32_t SQCH0SR;                         /**< +0x5D0 Sequence ch 0 status (RO). */
  volatile uint32_t SQCH0SCR;                        /**< +0x5D4 Sequence ch 0 clear.       */
  volatile uint32_t SQCH0IER;                        /**< +0x5D8 Sequence ch 0 int enable.  */
  volatile uint32_t
    _r19[k_ra8_mipi_dsi_rsv_words_sqch0_ier_to_ch1]; /**< +0x5DC..0x5FC reserved.         */
  volatile uint32_t SQCH1SET0R;                      /**< +0x600 Sequence ch 1 START bit. */
  volatile uint32_t
    _r20[k_ra8_mipi_dsi_rsv_words_sqch0_set0_to_sr]; /**< +0x604..0x60C reserved.           */
  volatile uint32_t SQCH1SR;                         /**< +0x610 Sequence ch 1 status (RO). */
  volatile uint32_t SQCH1SCR;                        /**< +0x614 Sequence ch 1 clear.       */
  volatile uint32_t SQCH1IER;                        /**< +0x618 Sequence ch 1 int enable.  */
  volatile uint32_t
    _r21[k_ra8_mipi_dsi_rsv_words_sqch1_ier_to_dsc]; /**< +0x61C..0x77C reserved.      */
  r_mipi_dsi_descriptor_t SQCH0DSC[8];               /**< +0x780..0x7FC ch0 desc 0..7. */
  r_mipi_dsi_descriptor_t SQCH1DSC[8];               /**< +0x800..0x87C ch1 desc 0..7. */
} r_mipi_dsi_regs_t;

static_assert(sizeof(r_mipi_dsi_descriptor_t) == 16U,
              "r_mipi_dsi_descriptor_t must be 4 x uint32 = 16 bytes");
static_assert(sizeof(r_mipi_dsi_regs_t) == (size_t)k_ra8_mipi_dsi_window_bytes,
              "r_mipi_dsi_regs_t size must match HUM Ch 65 register window");
static_assert(offsetof(r_mipi_dsi_regs_t, SQCH0DSC) == (size_t)k_ra8_mipi_dsi_offset_sqch0dsc,
              "SQCH0DSC must land at offset 0x780");
static_assert(offsetof(r_mipi_dsi_regs_t, SQCH1DSC) == (size_t)k_ra8_mipi_dsi_offset_sqch1dsc,
              "SQCH1DSC must land at offset 0x800");
static_assert(offsetof(r_mipi_dsi_regs_t, VMPPSETR) == (size_t)k_ra8_mipi_dsi_offset_vmppsetr,
              "VMPPSETR must land at offset 0x420");
static_assert(offsetof(r_mipi_dsi_regs_t, AKEPLATIR) == (size_t)k_ra8_mipi_dsi_offset_akeplatir,
              "AKEPLATIR must land at offset 0x220");

/**
 * @brief Pointer to the RA8D2 MIPI DSI host register block.
 *
 * @details
 * Always returns the Secure-world alias. In `RA8_OFF_TARGET` the
 * host test harness has already mmap'd the peripheral window so this
 * pointer is dereferenceable.
 *
 * @pre Caller has called `ra8_mstp_enable(k_ra8_mstp_mipi_dsi)` first
 *      (or this is the call site doing it).
 * @pre The peripheral bus clock is running.
 *
 * @post Returned pointer is non-NULL and aligned to 32 bits.
 * @post Returned pointer is `volatile`-qualified.
 *
 * @return Volatile pointer to the MIPI DSI register block.
 *
 * @note Thread-safety: returned pointer is shared, all accesses go to
 *       the same physical block; the driver serialises register I/O.
 *
 * @see ra8_mipi_dsi_init
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_mipi_dsi_regs_t* ra8_mipi_dsi(void)
{
  return (volatile r_mipi_dsi_regs_t*)k_ra8_mipi_dsi_base_addr_s;
}

#ifdef __cplusplus
}
#endif
