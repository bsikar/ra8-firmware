/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rmac_regs.h
 * @brief Ethernet MAC (RMAC) per-port register layout for the RA8D2
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * RMAC is the per-port Ethernet MAC block. One RMAC instance exists
 * per Ethernet port (m = 0, 1) and the two instances are mapped 0x2000
 * apart, interleaved with the matching ETHA instance:
 *
 *   ETHA0 = 0x403C_A000   RMAC0 = 0x403C_B000
 *   ETHA1 = 0x403C_C000   RMAC1 = 0x403C_D000
 *
 * The base address constants live in ra8_ether_regs.h; this file
 * only adds the per-port register window struct, the bit-field enums,
 * and the register-window accessor used by the per-port driver in
 * libs/ra8_hal/src/ra8_rmac.c.
 *
 * The layout is derived against the FSP CMSIS RA8D2 header
 * R7KA8D2KF_core0.h for the R_RMAC0_Type structure (Size = 0x530).
 * Every offset has been cross-checked against HUM Ch 33.3 Table 33.4.
 *
 * The full register surface is exposed: PHY MDIO (MPSM/MPIC/MPIM),
 * I/O configuration (MIOC), TX frame format / pause / PFC config
 * (MTFFC/MTPFC/MTPFC2/MTPFC3t), TX timestamp config (MTATCt/MTIM),
 * RX general / address / filter / storm / size / PTP-filter / pause
 * monitoring (MRGC/MRMAC0/1/MRAFC/MRSCE/P/MRSCC/MRFSCE/P/MTRC/MRPFM/
 * MPFCt), link verification (MLVC), Energy Efficient Ethernet
 * (MEEEC), loopback (MLBC), 10G XGMII config + auto-negotiation
 * (MXGMIIC/MPCH/MANM), error and monitoring interrupt
 * status/enable/disable (MEIS/MEIE/MEID, MMIS0..2/MMIE0..2/MMID0..2),
 * and the full statistic counter block (MMPFTCT..MTXBCPL).
 *
 * PTP timestamping itself stays in ra8_eth_gptp -- this header exposes
 * the per-frame TX-capture filter table (MPFCt) so RMAC can flag
 * PTP frames to the GPTP block, but the timestamp counters are owned
 * by the GPTP driver.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_ether_regs.h"

/**
 * @enum ra8_rmac_port_t
 * @brief RMAC port index (m = 0, 1).
 *
 * @details
 * Used as the first argument to every ra8_rmac_* call. Maps directly
 * to the per-port base addresses defined in ra8_ether_regs.h.
 */
typedef enum : uint8_t {
  k_ra8_rmac_port_0     = 0U, /**< RMAC port 0 (base 0x403C_B000). */
  k_ra8_rmac_port_1     = 1U, /**< RMAC port 1 (base 0x403C_D000). */
  k_ra8_rmac_port_count = 2U, /**< Number of RMAC ports.           */
} ra8_rmac_port_t;

/**
 * @enum ra8_rmac_mac_len_t
 * @brief Length of an Ethernet MAC address in bytes.
 *
 * @details
 * Six-byte / 48-bit IEEE 802 MAC address, split across MRMAC0
 * (high 16 bits in MAU field) and MRMAC1 (low 32 bits in MAL field)
 * per HUM Ch 33.4.
 */
typedef enum : uint8_t {
  k_ra8_rmac_mac_byte_count = 6U, /**< Bytes in a unicast MAC. */
} ra8_rmac_mac_len_t;

/**
 * @enum ra8_rmac_mac_byte_idx_t
 * @brief Byte indices into a 6-byte MAC address (network byte order).
 */
typedef enum : uint8_t {
  k_ra8_rmac_mac_byte_0 = 0U, /**< First byte (high octet, OUI). */
  k_ra8_rmac_mac_byte_1 = 1U, /**< RA8 rmac MAC byte 1.          */
  k_ra8_rmac_mac_byte_2 = 2U, /**< RA8 rmac MAC byte 2.          */
  k_ra8_rmac_mac_byte_3 = 3U, /**< RA8 rmac MAC byte 3.          */
  k_ra8_rmac_mac_byte_4 = 4U, /**< RA8 rmac MAC byte 4.          */
  k_ra8_rmac_mac_byte_5 = 5U, /**< Last byte (low octet).        */
} ra8_rmac_mac_byte_idx_t;

/**
 * @enum ra8_rmac_shift_t
 * @brief Bit shifts used across the RMAC register surface.
 */
typedef enum : uint8_t {
  k_ra8_rmac_shift_byte0         = 0U,  /**< First byte position.  */
  k_ra8_rmac_shift_byte1         = 8U,  /**< Second byte position. */
  k_ra8_rmac_shift_byte2         = 16U, /**< Third byte position.  */
  k_ra8_rmac_shift_byte3         = 24U, /**< Fourth byte position. */
  k_ra8_rmac_shift_high16        = 0U,  /**< MRMAC0.MAU low half.  */
  k_ra8_rmac_shift_high24        = 8U,  /**< MRMAC0.MAU high half. */
  k_ra8_rmac_shift_mpsm_pda      = 3U,  /**< MPSM.PDA[7:3].        */
  k_ra8_rmac_shift_mpsm_pra      = 8U,  /**< MPSM.PRA[12:8].       */
  k_ra8_rmac_shift_mpsm_pop      = 13U, /**< MPSM.POP[14:13].      */
  k_ra8_rmac_shift_mpsm_prd      = 16U, /**< MPSM.PRD[31:16].      */
  k_ra8_rmac_shift_mpic_lsc      = 3U,  /**< MPIC.LSC[5:3].        */
  k_ra8_rmac_shift_mpic_pis      = 0U,  /**< MPIC.PIS[2:0].        */
  k_ra8_rmac_shift_mpic_psmcs    = 16U, /**< MPIC.PSMCS[22:16].    */
  k_ra8_rmac_shift_mtpfc_pt      = 0U,  /**< MTPFC.PT[15:0].       */
  k_ra8_rmac_shift_mtpfc_pfrt    = 16U, /**< MTPFC.PFRT[23:16].    */
  k_ra8_rmac_shift_mrfsce_max    = 0U,  /**< MRFSCE.EMXS[15:0].    */
  k_ra8_rmac_shift_mrfsce_min    = 16U, /**< MRFSCE.EMNS[31:16].   */
  k_ra8_rmac_shift_mrgc_pfcrc    = 16U, /**< MRGC.PFCRC[23:16].    */
  k_ra8_rmac_shift_mpfc_val      = 8U,  /**< MPFCt.PFBV[15:8].     */
  k_ra8_rmac_shift_mrafc_perfect = 16U, /**< MRAFC perfect-match
                                        * filter half (MCENP etc.).*/
} ra8_rmac_shift_t;

/**
 * @enum ra8_rmac_mask_t
 * @brief Bit masks used across the RMAC register surface.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mask_byte         = 0x000000FFUL, /**< One MAC octet.       */
  k_ra8_rmac_mask_mrmac0_mau   = 0x0000FFFFUL, /**< MRMAC0.MAU valid.    */
  k_ra8_rmac_mask_mpsm_phy_reg = 0x0000001FUL, /**< 5-bit PHY reg/dev.   */
  k_ra8_rmac_mask_mpsm_op      = 0x00000003UL, /**< MPSM.POP[14:13].     */
  k_ra8_rmac_mask_mpsm_data    = 0x0000FFFFUL, /**< MPSM.PRD[31:16].     */
  k_ra8_rmac_mask_mpic_pis     = 0x00000007UL, /**< MPIC.PIS[2:0].       */
  k_ra8_rmac_mask_mpic_lsc     = 0x00000007UL, /**< MPIC.LSC[5:3].       */
  k_ra8_rmac_mask_mpic_psmcs   = 0x0000007FUL, /**< MPIC.PSMCS[22:16].   */
  k_ra8_rmac_mask_mtpfc_pt     = 0x0000FFFFUL, /**< MTPFC.PT[15:0].      */
  k_ra8_rmac_mask_mtpfc_pfrt   = 0x000000FFUL, /**< MTPFC.PFRT[23:16].   */
  k_ra8_rmac_mask_mrfsc        = 0x0000FFFFUL, /**< 16-bit frame size.   */
  k_ra8_rmac_mask_mrgc_pfcrc   = 0x000000FFUL, /**< MRGC.PFCRC.          */
  k_ra8_rmac_mask_mpfc_val     = 0x000000FFUL, /**< MPFCt.PFBV octet.    */
  k_ra8_rmac_mask_pfc_priority = 0x000000FFUL, /**< 8-bit prio bitmap.   */
  k_ra8_rmac_mask_meis_all     = 0x3FFF3FFFUL, /**< MEIS valid bits.     */
  k_ra8_rmac_mask_mmis0_all    = 0x00001FFFUL, /**< MMIS0 valid bits.    */
  k_ra8_rmac_mask_mmis1_all    = 0x0000000FUL, /**< MMIS1 valid bits.    */
  k_ra8_rmac_mask_mmis2_all    = 0x00000007UL, /**< MMIS2 valid bits.    */
  k_ra8_rmac_mask_meis_rx      = 0x00000001UL, /**< Sample RX-error bit. */
  k_ra8_rmac_mask_all          = 0xFFFFFFFFUL, /**< Wildcard write.      */
} ra8_rmac_mask_t;

/**
 * @enum ra8_rmac_offset_t
 * @brief RMAC per-port register offsets (HUM Ch 33.3 Table 33.4).
 *
 * @details
 * Provided as an enum so test code and trace tools can resolve any
 * register name to its offset without grepping the struct.
 */
typedef enum : uint16_t {
  k_ra8_rmac_off_mpsm      = 0x0000U, /**< PHY Station Management.    */
  k_ra8_rmac_off_mpic      = 0x0004U, /**< PHY Interfaces Cfg.        */
  k_ra8_rmac_off_mpim      = 0x0008U, /**< PHY Interfaces Monitoring. */
  k_ra8_rmac_off_mioc      = 0x0010U, /**< I/O Configuration.         */
  k_ra8_rmac_off_mtffc     = 0x0020U, /**< TX Frame Format Cfg.       */
  k_ra8_rmac_off_mtpfc     = 0x0024U, /**< TX Pause/PFC Frame Cfg.    */
  k_ra8_rmac_off_mtpfc2    = 0x0028U, /**< TX Pause/PFC Cfg 2.        */
  k_ra8_rmac_off_mtpfc30   = 0x0030U, /**< TX PFC Cfg 3 group 0.      */
  k_ra8_rmac_off_mtpfc31   = 0x0034U, /**< TX PFC Cfg 3 group 1.      */
  k_ra8_rmac_off_mtatc0    = 0x0050U, /**< TX Auto Timestamp 0.       */
  k_ra8_rmac_off_mtatc1    = 0x0054U, /**< TX Auto Timestamp 1.       */
  k_ra8_rmac_off_mtim      = 0x0060U, /**< TX Interfaces Monitoring.  */
  k_ra8_rmac_off_mrgc      = 0x0080U, /**< RX General Configuration.  */
  k_ra8_rmac_off_mrmac0    = 0x0084U, /**< RX MAC Address Cfg 0.      */
  k_ra8_rmac_off_mrmac1    = 0x0088U, /**< RX MAC Address Cfg 1.      */
  k_ra8_rmac_off_mrafc     = 0x008CU, /**< RX Address Filter Cfg.     */
  k_ra8_rmac_off_mrsce     = 0x0090U, /**< RX Storm Cfg E-frames.     */
  k_ra8_rmac_off_mrscp     = 0x0094U, /**< RX Storm Cfg P-frames.     */
  k_ra8_rmac_off_mrscc     = 0x0098U, /**< RX Storm Counter Cfg.      */
  k_ra8_rmac_off_mrfsce    = 0x009CU, /**< RX Frame Size Cfg E.       */
  k_ra8_rmac_off_mrfscp    = 0x00A0U, /**< RX Frame Size Cfg P.       */
  k_ra8_rmac_off_mtrc      = 0x00A4U, /**< Timestamp RX Cfg.          */
  k_ra8_rmac_off_mrpfm     = 0x00ACU, /**< RX Pause/PFC Monitoring.   */
  k_ra8_rmac_off_mpfc0     = 0x0100U, /**< PTP Filter Cfg 0..15.      */
  k_ra8_rmac_off_mlvc      = 0x0180U, /**< Link Verification Cfg.     */
  k_ra8_rmac_off_meeec     = 0x0184U, /**< Energy Efficient Ethernet. */
  k_ra8_rmac_off_mlbc      = 0x0188U, /**< Loopback Configuration.    */
  k_ra8_rmac_off_mxgmiic   = 0x0190U, /**< XGMII Configuration.       */
  k_ra8_rmac_off_mpch      = 0x0194U, /**< XGMII PCH Configuration.   */
  k_ra8_rmac_off_manm      = 0x019CU, /**< Auto-Negotiation Message.  */
  k_ra8_rmac_off_meis      = 0x0200U, /**< Error IRQ Status.          */
  k_ra8_rmac_off_meie      = 0x0204U, /**< Error IRQ Enable.          */
  k_ra8_rmac_off_meid      = 0x0208U, /**< Error IRQ Disable.         */
  k_ra8_rmac_off_mmis0     = 0x0210U, /**< Monitor IRQ Status 0.      */
  k_ra8_rmac_off_mmie0     = 0x0214U, /**< Monitor IRQ Enable 0.      */
  k_ra8_rmac_off_mmid0     = 0x0218U, /**< Monitor IRQ Disable 0.     */
  k_ra8_rmac_off_mmis1     = 0x0220U, /**< Monitor IRQ Status 1.      */
  k_ra8_rmac_off_mmie1     = 0x0224U, /**< Monitor IRQ Enable 1.      */
  k_ra8_rmac_off_mmid1     = 0x0228U, /**< Monitor IRQ Disable 1.     */
  k_ra8_rmac_off_mmis2     = 0x0230U, /**< Monitor IRQ Status 2.      */
  k_ra8_rmac_off_mmie2     = 0x0234U, /**< Monitor IRQ Enable 2.      */
  k_ra8_rmac_off_mmid2     = 0x0238U, /**< Monitor IRQ Disable 2.     */
  k_ra8_rmac_off_mmpftct   = 0x0300U, /**< Manual pause TX cnt.       */
  k_ra8_rmac_off_mapftct   = 0x0304U, /**< Auto pause TX cnt.         */
  k_ra8_rmac_off_mpfrct    = 0x0308U, /**< Pause RX cnt.              */
  k_ra8_rmac_off_mfcict    = 0x030CU, /**< False carrier indication.  */
  k_ra8_rmac_off_meeect    = 0x0310U, /**< EEE counter.               */
  k_ra8_rmac_off_mmpcftct0 = 0x0320U, /**< Manual PFC TX cnt 0.       */
  k_ra8_rmac_off_mmpcftct1 = 0x0324U, /**< Manual PFC TX cnt 1.       */
  k_ra8_rmac_off_mapcftct0 = 0x0330U, /**< Auto PFC TX cnt 0.         */
  k_ra8_rmac_off_mapcftct1 = 0x0334U, /**< Auto PFC TX cnt 1.         */
  k_ra8_rmac_off_mpcfrct0  = 0x0340U, /**< PFC RX cnt 0..7.           */
  k_ra8_rmac_off_mrovfc    = 0x0360U, /**< RX overflow cnt.           */
  k_ra8_rmac_off_mrhcrcec  = 0x0364U, /**< RX header CRC err cnt.     */
  k_ra8_rmac_off_mrgfce    = 0x0408U, /**< RX good E-frame cnt.       */
  k_ra8_rmac_off_mrgfcp    = 0x040CU, /**< RX good P-frame cnt.       */
  k_ra8_rmac_off_mtgfce    = 0x0508U, /**< TX good E-frame cnt.       */
  k_ra8_rmac_off_mtgfcp    = 0x050CU, /**< TX good P-frame cnt.       */
} ra8_rmac_offset_t;

/**
 * @enum ra8_rmac_mrafc_t
 * @brief MRAFC reception address filter mode bits (HUM Ch 33.4).
 *
 * @details
 * Bit layout (HUM Ch 33.4 "MRAFC : MAC Reception Address Filter
 * Configuration Register" p 1707):
 *   - bits [10:0]  hash/class enables  (UCENE / MCENE / BCENE / ...)
 *   - bits [26:16] perfect-match enables (UCENP / MCENP / BCENP / ...)
 *
 * The MAC's receive filter passes a frame only when BOTH halves accept
 * it: the hash-class enable AND the corresponding perfect-match enable.
 * FSP's r_rmac.c::rmac_configure_reception_filter (commit 8b3f2c1)
 * always programs the two halves in lockstep -- e.g. enabling broadcast
 * sets ``BCENE | BCENP`` together; enabling unicast sets
 * ``UCENE | UCENP`` together. Setting only the low half silently drops
 * every frame on real silicon.
 *
 * To make the public enum impossible to misuse, the three primary
 * class-enable constants below already bundle their high-half twin
 * (the FSP-canonical pairing). The bare register-level bits are still
 * exposed under the ``k_ra8_rmac_mrafc_perfect_*`` /
 * ``k_ra8_rmac_mrafc_hash_*`` aliases for callers that need to drive the
 * halves independently (e.g. address-hash-table maintenance).
 *
 * Reference: FSP renesas/fsp r_rmac.c (RMAC_REG_MRAFC_PROMISCUOUS_VALUE
 * and rmac_configure_reception_filter) for the canonical pairing.
 */
typedef enum : uint32_t {
  /* Low-half (hash / class enable) raw bits -- bits [10:0]. */
  k_ra8_rmac_mrafc_hash_unicast      = 0x00000001UL, /**< UCENE alone. */
  k_ra8_rmac_mrafc_hash_multicast    = 0x00000002UL, /**< MCENE alone. */
  k_ra8_rmac_mrafc_hash_broadcast    = 0x00000004UL, /**< BCENE alone. */
  k_ra8_rmac_mrafc_mst_storm         = 0x00000008UL, /**< MSTENE.      */
  k_ra8_rmac_mrafc_bst_storm         = 0x00000010UL, /**< BSTENE.      */
  k_ra8_rmac_mrafc_mc_accept         = 0x00000020UL, /**< MCACE.       */
  k_ra8_rmac_mrafc_bc_accept         = 0x00000040UL, /**< BCACE.       */
  k_ra8_rmac_mrafc_no_destmac_reject = 0x00000080UL, /**< NDAREE.      */
  k_ra8_rmac_mrafc_short_frame       = 0x00000100UL, /**< SDSFREE.     */
  k_ra8_rmac_mrafc_no_srcmac_reject  = 0x00000200UL, /**< NSAREE.      */
  k_ra8_rmac_mrafc_multicast_src     = 0x00000400UL, /**< MSAREE.      */
  /* High-half (perfect-match enable) raw bits -- bits [26:16]. */
  k_ra8_rmac_mrafc_perfect_unicast   = 0x00010000UL, /**< UCENP alone. */
  k_ra8_rmac_mrafc_perfect_multicast = 0x00020000UL, /**< MCENP alone. */
  k_ra8_rmac_mrafc_perfect_broadcast = 0x00040000UL, /**< BCENP alone. */
  /*
   * FSP-canonical paired class enables. These are what drivers /
   * board-init code should normally set in ``ra8_rmac_config_t::rx_filter``.
   * Each constant ORs the low-half (hash) bit with its high-half
   * (perfect-match) twin so the MAC's two-stage filter actually accepts
   * the frame class on silicon. Reference: FSP r_rmac.c
   * rmac_configure_reception_filter.
   */
  k_ra8_rmac_mrafc_unicast_match =
    (k_ra8_rmac_mrafc_hash_unicast | k_ra8_rmac_mrafc_perfect_unicast), /**< UCENE|UCENP. */
  k_ra8_rmac_mrafc_multicast_hash =
    (k_ra8_rmac_mrafc_hash_multicast | k_ra8_rmac_mrafc_perfect_multicast), /**< MCENE|MCENP. */
  k_ra8_rmac_mrafc_broadcast =
    (k_ra8_rmac_mrafc_hash_broadcast | k_ra8_rmac_mrafc_perfect_broadcast), /**< BCENE|BCENP. */
  k_ra8_rmac_mrafc_promiscuous =
    (k_ra8_rmac_mrafc_unicast_match | k_ra8_rmac_mrafc_multicast_hash | k_ra8_rmac_mrafc_broadcast |
     k_ra8_rmac_mrafc_mc_accept | k_ra8_rmac_mrafc_bc_accept), /**< RA8 rmac mrafc promiscuous. */
} ra8_rmac_mrafc_t;

/**
 * @enum ra8_rmac_pis_t
 * @brief Physical interface selector for MPIC.PIS[2:0].
 *
 * @details
 * HUM Ch 33.4.1.2 "MPIC : PHY Interfaces Configuration Register"
 * p 1707: MPIC.PIS[2:0] has only TWO defined encodings -- 000b = MII
 * and 010b = GMII. All other values are Reserved.
 *
 * MPIC.PIS describes the *internal* xMII interface between the RMAC
 * and the ESWM media mux. For an external RGMII link the ESWM
 * (MIICRn.MIISEL = 01b, HUM Table 29.11) converts RGMII <-> internal
 * GMII/MII, so the RMAC still sees MII or GMII. Per Table 29.11 the
 * RMAC.MPIC.PIS value is purely a function of link speed:
 *   - 10 / 100 Mbps -> MII  (000b)
 *   - 1 Gbps        -> GMII (010b)
 * regardless of whether the external PHY interface is MII or RGMII.
 */
typedef enum : uint8_t {
  k_ra8_rmac_pis_mii  = 0U, /**< MII (000b) -- internal xMII for 10/100 Mbps. */
  k_ra8_rmac_pis_gmii = 2U, /**< GMII (010b) -- internal xMII for 1 Gbps.     */
} ra8_rmac_pis_t;

/**
 * @enum ra8_rmac_lsc_t
 * @brief Link-speed selector for MPIC.LSC[5:3] (HUM Ch 33.4).
 */
typedef enum : uint8_t {
  k_ra8_rmac_lsc_10mbit   = 0U, /**< 10 Mbit/s.  */
  k_ra8_rmac_lsc_100mbit  = 1U, /**< 100 Mbit/s. */
  k_ra8_rmac_lsc_1000mbit = 2U, /**< 1 Gbit/s.   */
  k_ra8_rmac_lsc_2500mbit = 3U, /**< 2.5 Gbit/s. */
  k_ra8_rmac_lsc_10gbit   = 4U, /**< 10 Gbit/s.  */
  k_ra8_rmac_lsc_count    = 5U, /**< Sentinel.   */
} ra8_rmac_lsc_t;

/**
 * @enum ra8_rmac_duplex_t
 * @brief Duplex selection (programmed via MPIC.PIPP / MPIC.PIP).
 */
typedef enum : uint8_t {
  k_ra8_rmac_duplex_half = 0U, /**< Half duplex. */
  k_ra8_rmac_duplex_full = 1U, /**< Full duplex. */
} ra8_rmac_duplex_t;

/**
 * @enum ra8_rmac_mdio_op_t
 * @brief MPSM.POP[14:13] PHY MDIO operation selector.
 *
 * @details
 * For Clause-22 access the encoding maps to "read" / "write".
 * For Clause-45 access the encoding additionally selects "address
 * frame" vs "post-read-incr" -- the driver wraps the lower-level
 * operations into ra8_rmac_mdio_c45_* helpers.
 */
typedef enum : uint8_t {
  k_ra8_rmac_mdio_op_c22_write    = 1U, /**< 01 -- C22 write.        */
  k_ra8_rmac_mdio_op_c22_read     = 2U, /**< 10 -- C22 read.         */
  k_ra8_rmac_mdio_op_c45_address  = 0U, /**< 00 -- C45 address.      */
  k_ra8_rmac_mdio_op_c45_write    = 1U, /**< 01 -- C45 write.        */
  k_ra8_rmac_mdio_op_c45_read_inc = 2U, /**< 10 -- C45 post-incr rd. */
  k_ra8_rmac_mdio_op_c45_read     = 3U, /**< 11 -- C45 read.         */
} ra8_rmac_mdio_op_t;

/**
 * @enum ra8_rmac_pause_mode_t
 * @brief Selects PFM bit in MTPFC2 -- pause vs PFC frame.
 */
typedef enum : uint8_t {
  k_ra8_rmac_pause_mode_802_3x = 0U, /**< Classic IEEE 802.3x pause. */
  k_ra8_rmac_pause_mode_pfc    = 1U, /**< IEEE 802.1Qbb PFC frame.   */
} ra8_rmac_pause_mode_t;

/**
 * @enum ra8_rmac_pfc_group_t
 * @brief PFC priority group index (MTPFC3t, t = 0..1).
 */
typedef enum : uint8_t {
  k_ra8_rmac_pfc_group_0     = 0U, /**< Priority group 0.         */
  k_ra8_rmac_pfc_group_1     = 1U, /**< Priority group 1.         */
  k_ra8_rmac_pfc_group_count = 2U, /**< RA8 rmac pfc group count. */
} ra8_rmac_pfc_group_t;

/**
 * @enum ra8_rmac_ptp_filter_idx_t
 * @brief PTP filter table index for MPFCt (t = 0..15).
 */
typedef enum : uint8_t {
  k_ra8_rmac_ptp_filter_count = 16U, /**< 16 filter entries total. */
} ra8_rmac_ptp_filter_idx_t;

/**
 * @enum ra8_rmac_pfc_rx_idx_t
 * @brief PFC RX counter index (MPCFRCTt, t = 0..7).
 */
typedef enum : uint8_t {
  k_ra8_rmac_pfc_rx_count = 8U, /**< 8 PFC priority RX counters. */
} ra8_rmac_pfc_rx_idx_t;

/**
 * @enum ra8_rmac_meis_bit_t
 * @brief Per-bit names inside MEIS / MEIE / MEID (HUM Ch 33.4).
 */
typedef enum : uint32_t {
  k_ra8_rmac_meis_tsls   = (1UL << 0),  /**< Transmit-side link state.  */
  k_ra8_rmac_meis_ties   = (1UL << 1),  /**< TX interrupt error.        */
  k_ra8_rmac_meis_pres   = (1UL << 2),  /**< PHY register error.        */
  k_ra8_rmac_meis_pfrros = (1UL << 3),  /**< Pause/PFC frame RX over.   */
  k_ra8_rmac_meis_fcds   = (1UL << 4),  /**< False carrier detect.      */
  k_ra8_rmac_meis_tces   = (1UL << 5),  /**< TX collision error.        */
  k_ra8_rmac_meis_tbcis  = (1UL << 6),  /**< TX byte counter overflow.  */
  k_ra8_rmac_meis_bfes   = (1UL << 7),  /**< RX bad-fragment error.     */
  k_ra8_rmac_meis_fces   = (1UL << 8),  /**< RX fragment-count error.   */
  k_ra8_rmac_meis_reoes  = (1UL << 9),  /**< RX end-of-frame error.     */
  k_ra8_rmac_meis_rpoes  = (1UL << 10), /**< RX preamble error.         */
  k_ra8_rmac_meis_rpcres = (1UL << 11), /**< RX PCH CRC error.          */
  k_ra8_rmac_meis_ctles0 = (1UL << 12), /**< Counter overflow group 0.  */
  k_ra8_rmac_meis_ctles1 = (1UL << 13), /**< Counter overflow group 1.  */
  k_ra8_rmac_meis_pdes   = (1UL << 20), /**< PFC frame decode error.    */
  k_ra8_rmac_meis_pnaes  = (1UL << 21), /**< PFC not-allowed.           */
  k_ra8_rmac_meis_fcmces = (1UL << 22), /**< Frame-check mCRC error.    */
  k_ra8_rmac_meis_ffmes  = (1UL << 23), /**< Final fragment missing.    */
  k_ra8_rmac_meis_cfces  = (1UL << 24), /**< Continuation fragment cnt. */
  k_ra8_rmac_meis_frces  = (1UL << 25), /**< RMAC filter rejected.      */
  k_ra8_rmac_meis_rpooms = (1UL << 26), /**< RX preamble out of margin. */
  k_ra8_rmac_meis_ffs    = (1UL << 27), /**< First-fragment status.     */
  k_ra8_rmac_meis_fues   = (1UL << 28), /**< Frame undersize error.     */
  k_ra8_rmac_meis_foes   = (1UL << 29), /**< Frame oversize error.      */
} ra8_rmac_meis_bit_t;

/**
 * @enum ra8_rmac_mmis0_bit_t
 * @brief MMIS0 monitoring-IRQ bits (HUM Ch 33.4).
 */
typedef enum : uint32_t {
  k_ra8_rmac_mmis0_plscs  = (1UL << 0),  /**< PHY link-state change.  */
  k_ra8_rmac_mmis0_pids   = (1UL << 1),  /**< PHY ID detected.        */
  k_ra8_rmac_mmis0_lvss   = (1UL << 2),  /**< Link verify success.    */
  k_ra8_rmac_mmis0_lvfs   = (1UL << 3),  /**< Link verify failure.    */
  k_ra8_rmac_mmis0_vfrs   = (1UL << 4),  /**< Verify frame received.  */
  k_ra8_rmac_mmis0_andets = (1UL << 6),  /**< Auto-neg detected.      */
  k_ra8_rmac_mmis0_xlfds  = (1UL << 8),  /**< 10G local-fault detect. */
  k_ra8_rmac_mmis0_xlfes  = (1UL << 9),  /**< 10G local-fault end.    */
  k_ra8_rmac_mmis0_xlfsds = (1UL << 10), /**< 10G LF sync drop.       */
  k_ra8_rmac_mmis0_xrfsds = (1UL << 11), /**< 10G RF sync drop.       */
  k_ra8_rmac_mmis0_xlisds = (1UL << 12), /**< 10G LI sync drop.       */
} ra8_rmac_mmis0_bit_t;

/**
 * @enum ra8_rmac_mmis1_bit_t
 * @brief MMIS1 PHY-station-management completion bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mmis1_pracs  = (1UL << 0), /**< PHY read access complete.  */
  k_ra8_rmac_mmis1_pwacs  = (1UL << 1), /**< PHY write access complete. */
  k_ra8_rmac_mmis1_paacs  = (1UL << 2), /**< PHY address access cmpl.   */
  k_ra8_rmac_mmis1_ppracs = (1UL << 3), /**< PHY post-read autoinc cmpl */
} ra8_rmac_mmis1_bit_t;

/**
 * @enum ra8_rmac_mpsm_bit_t
 * @brief MPSM (PHY Station Management Register) control bits.
 *
 * @details HUM Ch 33.4.1.1 "MPSM" p 1707. PSME is the start/busy
 * flag: software writes 1 to launch a PHY register transaction and
 * the hardware self-clears it to 0 on completion. Writes to MPSM
 * while PSME is asserted "have no effect" (HUM Note 2), so the
 * driver must drain PSME to 0 before issuing a new transaction.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mpsm_psme = (1UL << 0), /**< PHY Station Management Enable / busy. */
} ra8_rmac_mpsm_bit_t;

/**
 * @enum ra8_rmac_mmis2_bit_t
 * @brief MMIS2 wake-on-LAN / EEE bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mmis2_mpdis  = (1UL << 0), /**< Magic-packet detect. */
  k_ra8_rmac_mmis2_lpiais = (1UL << 1), /**< LPI assert IRQ.      */
  k_ra8_rmac_mmis2_lpidis = (1UL << 2), /**< LPI de-assert IRQ.   */
} ra8_rmac_mmis2_bit_t;

/**
 * @enum ra8_rmac_mtffc_bit_t
 * @brief MTFFC TX-frame-format bits (HUM Ch 33.4).
 */
typedef enum : uint32_t {
  k_ra8_rmac_mtffc_dpad = (1UL << 0), /**< Disable padding.               */
  k_ra8_rmac_mtffc_fcm  = (1UL << 1), /**< Frame-check mode (mCRC vs CRC) */
} ra8_rmac_mtffc_bit_t;

/**
 * @enum ra8_rmac_mrgc_bit_t
 * @brief MRGC RX-general-config bits (HUM Ch 33.4).
 */
typedef enum : uint32_t {
  k_ra8_rmac_mrgc_rcpt  = (1UL << 0), /**< Reception-control passthru. */
  k_ra8_rmac_mrgc_pfrc  = (1UL << 1), /**< Pause-frame RX control.     */
  k_ra8_rmac_mrgc_pfrtz = (1UL << 2), /**< Pause-frame request to zero */
  k_ra8_rmac_mrgc_mpde  = (1UL << 3), /**< Magic-packet detect enable. */
  k_ra8_rmac_mrgc_rfcfe = (1UL << 4), /**< Receive frame check-fail en */
} ra8_rmac_mrgc_bit_t;

/**
 * @enum ra8_rmac_mlbc_bit_t
 * @brief MLBC loopback bits (HUM Ch 33.4).
 */
typedef enum : uint32_t {
  k_ra8_rmac_mlbc_lbme = (1UL << 0), /**< Loopback mode enable. */
} ra8_rmac_mlbc_bit_t;

/**
 * @enum ra8_rmac_meeec_bit_t
 * @brief MEEEC Energy-Efficient-Ethernet bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_meeec_lpitr = (1UL << 0), /**< LPI transmit request. */
} ra8_rmac_meeec_bit_t;

/**
 * @enum ra8_rmac_mpim_bit_t
 * @brief MPIM PHY-monitoring bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mpim_pls  = (1UL << 0), /**< PHY link status. */
  k_ra8_rmac_mpim_lpia = (1UL << 1), /**< LPI active.      */
} ra8_rmac_mpim_bit_t;

/**
 * @enum ra8_rmac_mlvc_bit_t
 * @brief MLVC link-verification bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mlvc_pase = (1UL << 8),  /**< PHY auto-startup enable.   */
  k_ra8_rmac_mlvc_plv  = (1UL << 16), /**< Perform link verification. */
} ra8_rmac_mlvc_bit_t;

/**
 * @enum ra8_rmac_mxgmiic_bit_t
 * @brief MXGMIIC 10G-XGMII bits.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mxgmiic_lfs_txrfs  = (1UL << 0), /**< TX remote-fault sig. */
  k_ra8_rmac_mxgmiic_lfs_txidle = (1UL << 1), /**< TX local-fault idle. */
} ra8_rmac_mxgmiic_bit_t;

/**
 * @enum ra8_rmac_rsv_words_t
 * @brief Reserved-word array sizes (in 32-bit words) used inside the
 *        RMAC per-port register block.
 *
 * @details
 * Names each reserved gap so the struct definition does not depend on
 * raw integer literals. Cross-checked against HUM Table 33.4.
 */
typedef enum : uint8_t {
  k_ra8_rmac_rsv_words_mtim_to_mrgc       = 7U,  /**< +0x0064..0x007C gap. */
  k_ra8_rmac_rsv_words_mrpfm_to_mpfc      = 20U, /**< +0x00B0..0x00FC gap. */
  k_ra8_rmac_rsv_words_manm_to_meis       = 24U, /**< +0x01A0..0x01FC gap. */
  k_ra8_rmac_rsv_words_mmid2_to_mmpftct   = 49U, /**< +0x023C..0x02FC gap. */
  k_ra8_rmac_rsv_words_mrhcrcec_to_mrgfce = 40U, /**< +0x0368..0x0404 gap. */
  k_ra8_rmac_rsv_words_mrxbcpl_to_mtgfce  = 43U, /**< +0x045C..0x0504 gap. */
} ra8_rmac_rsv_words_t;

/**
 * @enum ra8_rmac_size_t
 * @brief RMAC register window total size (FSP R_RMAC0_Type).
 */
typedef enum : uint16_t {
  k_ra8_rmac_window_bytes = 0x530U, /**< Total MMIO window (FSP size). */
} ra8_rmac_size_t;

/**
 * @struct r_rmac_regs_t
 * @brief Full RMAC per-port register window (HUM Ch 33.3, FSP R_RMAC0_Type).
 *
 * @details
 * Layout matches the FSP CMSIS RA8D2 R_RMAC0_Type byte-for-byte
 * (size = 0x530). Reserved gaps are spelled out so the offsets always
 * line up with HUM Table 33.4. cppcheck cannot see tests/ so it flags
 * every field as unused; each member is accessed via ``ra8_rmac(port)``
 * in ``libs/ra8_hal/src/ra8_rmac.c``.
 */
typedef struct {
  volatile uint32_t MPSM;                               /**< +0x0000 PHY Station Mgmt.        */
  volatile uint32_t MPIC;                               /**< +0x0004 PHY Interfaces Cfg.      */
  volatile uint32_t MPIM;                               /**< +0x0008 PHY Monitoring.          */
  volatile uint32_t reserved0c;                         /**< +0x000C Reserved.                */
  volatile uint32_t MIOC;                               /**< +0x0010 I/O Configuration.       */
  volatile uint32_t reserved14[3];                      /**< +0x0014..0x001C Reserved.        */
  volatile uint32_t MTFFC;                              /**< +0x0020 TX Frame Format Cfg.     */
  volatile uint32_t MTPFC;                              /**< +0x0024 TX Pause/PFC Frame Cfg.  */
  volatile uint32_t MTPFC2;                             /**< +0x0028 TX Pause/PFC Frame Cfg2. */
  volatile uint32_t reserved2c;                         /**< +0x002C Reserved.                */
  volatile uint32_t MTPFC3[k_ra8_rmac_pfc_group_count]; /**< +0x0030..0x0034                  */
  volatile uint32_t reserved38[6];                      /**< +0x0038..0x004C Reserved.        */
  volatile uint32_t MTATC[2];                           /**< +0x0050..0x0054 TX Auto Tstamp.  */
  volatile uint32_t reserved58[2];                      /**< +0x0058..0x005C Reserved.        */
  volatile uint32_t MTIM;                               /**< +0x0060 TX Interfaces Monitor.   */
  volatile uint32_t
    reserved64[k_ra8_rmac_rsv_words_mtim_to_mrgc]; /**< +0x0064..0x007C Reserved.      */
  volatile uint32_t MRGC;                          /**< +0x0080 RX General Cfg.        */
  volatile uint32_t MRMAC0;                        /**< +0x0084 RX MAC Address 0.      */
  volatile uint32_t MRMAC1;                        /**< +0x0088 RX MAC Address 1.      */
  volatile uint32_t MRAFC;                         /**< +0x008C RX Address Filter Cfg. */
  volatile uint32_t MRSCE;                         /**< +0x0090 RX Storm Cfg E.        */
  volatile uint32_t MRSCP;                         /**< +0x0094 RX Storm Cfg P.        */
  volatile uint32_t MRSCC;                         /**< +0x0098 RX Storm Counter Cfg.  */
  volatile uint32_t MRFSCE;                        /**< +0x009C RX Frame Size Cfg E.   */
  volatile uint32_t MRFSCP;                        /**< +0x00A0 RX Frame Size Cfg P.   */
  volatile uint32_t MTRC;                          /**< +0x00A4 Timestamp RX Cfg.      */
  volatile uint32_t reserveda8;                    /**< +0x00A8 Reserved.              */
  volatile uint32_t MRPFM;                         /**< +0x00AC RX Pause/PFC Monitor.  */
  volatile uint32_t
    reservedb0[k_ra8_rmac_rsv_words_mrpfm_to_mpfc];    /**< +0x00B0..0x00FC Reserved.      */
  volatile uint32_t MPFC[k_ra8_rmac_ptp_filter_count]; /**< +0x0100..0x013C                */
  volatile uint32_t reserved140[16];                   /**< +0x0140..0x017C Reserved.      */
  volatile uint32_t MLVC;                              /**< +0x0180 Link Verification Cfg. */
  volatile uint32_t MEEEC;                             /**< +0x0184 EEE Configuration.     */
  volatile uint32_t MLBC;                              /**< +0x0188 Loopback Cfg.          */
  volatile uint32_t reserved18c;                       /**< +0x018C Reserved.              */
  volatile uint32_t MXGMIIC;                           /**< +0x0190 XGMII Cfg.             */
  volatile uint32_t MPCH;                              /**< +0x0194 XGMII PCH Cfg.         */
  volatile uint32_t reserved198;                       /**< +0x0198 Reserved.              */
  volatile uint32_t MANM;                              /**< +0x019C Auto-Neg Message.      */
  volatile uint32_t
    reserved1a0[k_ra8_rmac_rsv_words_manm_to_meis]; /**< +0x01A0..0x01FC Reserved.  */
  volatile uint32_t MEIS;                           /**< +0x0200 Error IRQ Status.  */
  volatile uint32_t MEIE;                           /**< +0x0204 Error IRQ Enable.  */
  volatile uint32_t MEID;                           /**< +0x0208 Error IRQ Disable. */
  volatile uint32_t reserved20c;                    /**< +0x020C Reserved.          */
  volatile uint32_t MMIS0;                          /**< +0x0210 Monitor Status 0.  */
  volatile uint32_t MMIE0;                          /**< +0x0214 Monitor Enable 0.  */
  volatile uint32_t MMID0;                          /**< +0x0218 Monitor Disable 0. */
  volatile uint32_t reserved21c;                    /**< +0x021C Reserved.          */
  volatile uint32_t MMIS1;                          /**< +0x0220 Monitor Status 1.  */
  volatile uint32_t MMIE1;                          /**< +0x0224 Monitor Enable 1.  */
  volatile uint32_t MMID1;                          /**< +0x0228 Monitor Disable 1. */
  volatile uint32_t reserved22c;                    /**< +0x022C Reserved.          */
  volatile uint32_t MMIS2;                          /**< +0x0230 Monitor Status 2.  */
  volatile uint32_t MMIE2;                          /**< +0x0234 Monitor Enable 2.  */
  volatile uint32_t MMID2;                          /**< +0x0238 Monitor Disable 2. */
  volatile uint32_t
    reserved23c[k_ra8_rmac_rsv_words_mmid2_to_mmpftct];   /**< +0x023C..0x02FC Reserved.        */
  volatile uint32_t MMPFTCT;                              /**< +0x0300 Manual pause TX cnt.     */
  volatile uint32_t MAPFTCT;                              /**< +0x0304 Auto pause TX cnt.       */
  volatile uint32_t MPFRCT;                               /**< +0x0308 Pause frame RX cnt.      */
  volatile uint32_t MFCICT;                               /**< +0x030C False carrier indication */
  volatile uint32_t MEEECT;                               /**< +0x0310 EEE counter.             */
  volatile uint32_t reserved314[3];                       /**< +0x0314..0x031C Reserved.        */
  volatile uint32_t MMPCFTCT[k_ra8_rmac_pfc_group_count]; /**< +0x0320..0x0324                  */
  volatile uint32_t reserved328[2];                       /**< +0x0328..0x032C Reserved.        */
  volatile uint32_t MAPCFTCT[k_ra8_rmac_pfc_group_count]; /**< +0x0330..0x0334                  */
  volatile uint32_t reserved338[2];                       /**< +0x0338..0x033C Reserved.        */
  volatile uint32_t MPCFRCT[k_ra8_rmac_pfc_rx_count];     /**< +0x0340..0x035C                  */
  volatile uint32_t MROVFC;                               /**< +0x0360 RX overflow cnt.         */
  volatile uint32_t MRHCRCEC;                             /**< +0x0364 RX header CRC err cnt.   */
  volatile uint32_t
    reserved368[k_ra8_rmac_rsv_words_mrhcrcec_to_mrgfce]; /**< +0x0368..0x0404 Reserved.       */
  volatile uint32_t MRGFCE;                               /**< +0x0408 RX good E-frame cnt.    */
  volatile uint32_t MRGFCP;                               /**< +0x040C RX good P-frame cnt.    */
  volatile uint32_t MRBFC;                                /**< +0x0410 RX broadcast cnt.       */
  volatile uint32_t MRMFC;                                /**< +0x0414 RX multicast cnt.       */
  volatile uint32_t MRUFC;                                /**< +0x0418 RX unicast cnt.         */
  volatile uint32_t MRPEFC;                               /**< +0x041C RX PHY error cnt.       */
  volatile uint32_t MRNEFC;                               /**< +0x0420 RX nibble error cnt.    */
  volatile uint32_t MRFMEFC;                              /**< +0x0424 RX FCS/mCRC err cnt.    */
  volatile uint32_t MRFFMEFC;                             /**< +0x0428 RX final-frag missing.  */
  volatile uint32_t MRCFCEFC;                             /**< +0x042C RX C-fragment err cnt.  */
  volatile uint32_t MRFCEFC;                              /**< +0x0430 RX fragment-cnt err.    */
  volatile uint32_t MRRCFEFC;                             /**< +0x0434 RX filter-rejected cnt. */
  volatile uint32_t MRFC;                                 /**< +0x0438 RX total frame cnt.     */
  volatile uint32_t MRGUEFC;                              /**< +0x043C RX good undersize cnt.  */
  volatile uint32_t MRBUEFC;                              /**< +0x0440 RX bad undersize cnt.   */
  volatile uint32_t MRGOEFC;                              /**< +0x0444 RX good oversize cnt.   */
  volatile uint32_t MRBOEFC;                              /**< +0x0448 RX bad oversize cnt.    */
  volatile uint32_t MRXBCEU;                              /**< +0x044C RX byte cnt E upper.    */
  volatile uint32_t MRXBCEL;                              /**< +0x0450 RX byte cnt E lower.    */
  volatile uint32_t MRXBCPU;                              /**< +0x0454 RX byte cnt P upper.    */
  volatile uint32_t MRXBCPL;                              /**< +0x0458 RX byte cnt P lower.    */
  volatile uint32_t
    reserved45c[k_ra8_rmac_rsv_words_mrxbcpl_to_mtgfce]; /**< +0x045C..0x0504 Reserved.    */
  volatile uint32_t MTGFCE;                              /**< +0x0508 TX good E-frame cnt. */
  volatile uint32_t MTGFCP;                              /**< +0x050C TX good P-frame cnt. */
  volatile uint32_t MTBFC;                               /**< +0x0510 TX broadcast cnt.    */
  volatile uint32_t MTMFC;                               /**< +0x0514 TX multicast cnt.    */
  volatile uint32_t MTUFC;                               /**< +0x0518 TX unicast cnt.      */
  volatile uint32_t MTEFC;                               /**< +0x051C TX error frame cnt.  */
  volatile uint32_t MTXBCEU;                             /**< +0x0520 TX byte cnt E upper. */
  volatile uint32_t MTXBCEL;                             /**< +0x0524 TX byte cnt E lower. */
  volatile uint32_t MTXBCPU;                             /**< +0x0528 TX byte cnt P upper. */
  volatile uint32_t MTXBCPL;                             /**< +0x052C TX byte cnt P lower. */
} r_rmac_regs_t;

/**
 * @brief Compile-time check that the RMAC register window matches FSP.
 *
 * @details
 * FSP's R_RMAC0_Type is documented at "Size = 1328 (0x530)". If a
 * future register addition perturbs the layout, this assertion fires
 * before any test runs.
 */
static_assert(sizeof(r_rmac_regs_t) == (size_t)k_ra8_rmac_window_bytes,
              "r_rmac_regs_t must match FSP R_RMAC0_Type size 0x530");

/**
 * @brief Compile-time offset insurance against silent layout drift.
 *
 * @details
 * Each named register's offset is asserted against the matching
 * `k_ra8_rmac_off_*` constant (which is the source of truth from HUM
 * Ch 33.3 Table 33.4). If a future edit perturbs a reserved gap or
 * field width, the build fails before any silent frame corruption
 * can hit the wire.
 */
static_assert(offsetof(r_rmac_regs_t, MPSM) == (size_t)k_ra8_rmac_off_mpsm, "MPSM offset");
static_assert(offsetof(r_rmac_regs_t, MPIC) == (size_t)k_ra8_rmac_off_mpic, "MPIC offset");
static_assert(offsetof(r_rmac_regs_t, MPIM) == (size_t)k_ra8_rmac_off_mpim, "MPIM offset");
static_assert(offsetof(r_rmac_regs_t, MIOC) == (size_t)k_ra8_rmac_off_mioc, "MIOC offset");
static_assert(offsetof(r_rmac_regs_t, MTFFC) == (size_t)k_ra8_rmac_off_mtffc, "MTFFC offset");
static_assert(offsetof(r_rmac_regs_t, MTPFC) == (size_t)k_ra8_rmac_off_mtpfc, "MTPFC offset");
static_assert(offsetof(r_rmac_regs_t, MTPFC2) == (size_t)k_ra8_rmac_off_mtpfc2, "MTPFC2 offset");
static_assert(offsetof(r_rmac_regs_t, MTPFC3) == (size_t)k_ra8_rmac_off_mtpfc30, "MTPFC3 offset");
static_assert(offsetof(r_rmac_regs_t, MTATC) == (size_t)k_ra8_rmac_off_mtatc0, "MTATC offset");
static_assert(offsetof(r_rmac_regs_t, MTIM) == (size_t)k_ra8_rmac_off_mtim, "MTIM offset");
static_assert(offsetof(r_rmac_regs_t, MRGC) == (size_t)k_ra8_rmac_off_mrgc, "MRGC offset");
static_assert(offsetof(r_rmac_regs_t, MRMAC0) == (size_t)k_ra8_rmac_off_mrmac0, "MRMAC0 offset");
static_assert(offsetof(r_rmac_regs_t, MRMAC1) == (size_t)k_ra8_rmac_off_mrmac1, "MRMAC1 offset");
static_assert(offsetof(r_rmac_regs_t, MRAFC) == (size_t)k_ra8_rmac_off_mrafc, "MRAFC offset");
static_assert(offsetof(r_rmac_regs_t, MRSCE) == (size_t)k_ra8_rmac_off_mrsce, "MRSCE offset");
static_assert(offsetof(r_rmac_regs_t, MRSCP) == (size_t)k_ra8_rmac_off_mrscp, "MRSCP offset");
static_assert(offsetof(r_rmac_regs_t, MRSCC) == (size_t)k_ra8_rmac_off_mrscc, "MRSCC offset");
static_assert(offsetof(r_rmac_regs_t, MRFSCE) == (size_t)k_ra8_rmac_off_mrfsce, "MRFSCE offset");
static_assert(offsetof(r_rmac_regs_t, MRFSCP) == (size_t)k_ra8_rmac_off_mrfscp, "MRFSCP offset");
static_assert(offsetof(r_rmac_regs_t, MTRC) == (size_t)k_ra8_rmac_off_mtrc, "MTRC offset");
static_assert(offsetof(r_rmac_regs_t, MRPFM) == (size_t)k_ra8_rmac_off_mrpfm, "MRPFM offset");
static_assert(offsetof(r_rmac_regs_t, MPFC) == (size_t)k_ra8_rmac_off_mpfc0, "MPFC offset");
static_assert(offsetof(r_rmac_regs_t, MLVC) == (size_t)k_ra8_rmac_off_mlvc, "MLVC offset");
static_assert(offsetof(r_rmac_regs_t, MEEEC) == (size_t)k_ra8_rmac_off_meeec, "MEEEC offset");
static_assert(offsetof(r_rmac_regs_t, MLBC) == (size_t)k_ra8_rmac_off_mlbc, "MLBC offset");
static_assert(offsetof(r_rmac_regs_t, MXGMIIC) == (size_t)k_ra8_rmac_off_mxgmiic, "MXGMIIC offset");
static_assert(offsetof(r_rmac_regs_t, MPCH) == (size_t)k_ra8_rmac_off_mpch, "MPCH offset");
static_assert(offsetof(r_rmac_regs_t, MANM) == (size_t)k_ra8_rmac_off_manm, "MANM offset");
static_assert(offsetof(r_rmac_regs_t, MEIS) == (size_t)k_ra8_rmac_off_meis, "MEIS offset");
static_assert(offsetof(r_rmac_regs_t, MEIE) == (size_t)k_ra8_rmac_off_meie, "MEIE offset");
static_assert(offsetof(r_rmac_regs_t, MEID) == (size_t)k_ra8_rmac_off_meid, "MEID offset");
static_assert(offsetof(r_rmac_regs_t, MMIS0) == (size_t)k_ra8_rmac_off_mmis0, "MMIS0 offset");
static_assert(offsetof(r_rmac_regs_t, MMIE0) == (size_t)k_ra8_rmac_off_mmie0, "MMIE0 offset");
static_assert(offsetof(r_rmac_regs_t, MMID0) == (size_t)k_ra8_rmac_off_mmid0, "MMID0 offset");
static_assert(offsetof(r_rmac_regs_t, MMIS1) == (size_t)k_ra8_rmac_off_mmis1, "MMIS1 offset");
static_assert(offsetof(r_rmac_regs_t, MMIE1) == (size_t)k_ra8_rmac_off_mmie1, "MMIE1 offset");
static_assert(offsetof(r_rmac_regs_t, MMID1) == (size_t)k_ra8_rmac_off_mmid1, "MMID1 offset");
static_assert(offsetof(r_rmac_regs_t, MMIS2) == (size_t)k_ra8_rmac_off_mmis2, "MMIS2 offset");
static_assert(offsetof(r_rmac_regs_t, MMIE2) == (size_t)k_ra8_rmac_off_mmie2, "MMIE2 offset");
static_assert(offsetof(r_rmac_regs_t, MMID2) == (size_t)k_ra8_rmac_off_mmid2, "MMID2 offset");
static_assert(offsetof(r_rmac_regs_t, MMPFTCT) == (size_t)k_ra8_rmac_off_mmpftct, "MMPFTCT offset");
static_assert(offsetof(r_rmac_regs_t, MAPFTCT) == (size_t)k_ra8_rmac_off_mapftct, "MAPFTCT offset");
static_assert(offsetof(r_rmac_regs_t, MPFRCT) == (size_t)k_ra8_rmac_off_mpfrct, "MPFRCT offset");
static_assert(offsetof(r_rmac_regs_t, MFCICT) == (size_t)k_ra8_rmac_off_mfcict, "MFCICT offset");
static_assert(offsetof(r_rmac_regs_t, MEEECT) == (size_t)k_ra8_rmac_off_meeect, "MEEECT offset");
static_assert(offsetof(r_rmac_regs_t, MMPCFTCT) == (size_t)k_ra8_rmac_off_mmpcftct0,
              "MMPCFTCT offset");
static_assert(offsetof(r_rmac_regs_t, MAPCFTCT) == (size_t)k_ra8_rmac_off_mapcftct0,
              "MAPCFTCT offset");
static_assert(offsetof(r_rmac_regs_t, MPCFRCT) == (size_t)k_ra8_rmac_off_mpcfrct0,
              "MPCFRCT offset");
static_assert(offsetof(r_rmac_regs_t, MROVFC) == (size_t)k_ra8_rmac_off_mrovfc, "MROVFC offset");
static_assert(offsetof(r_rmac_regs_t, MRHCRCEC) == (size_t)k_ra8_rmac_off_mrhcrcec,
              "MRHCRCEC offset");
static_assert(offsetof(r_rmac_regs_t, MRGFCE) == (size_t)k_ra8_rmac_off_mrgfce, "MRGFCE offset");
static_assert(offsetof(r_rmac_regs_t, MRGFCP) == (size_t)k_ra8_rmac_off_mrgfcp, "MRGFCP offset");
static_assert(offsetof(r_rmac_regs_t, MTGFCE) == (size_t)k_ra8_rmac_off_mtgfce, "MTGFCE offset");
static_assert(offsetof(r_rmac_regs_t, MTGFCP) == (size_t)k_ra8_rmac_off_mtgfcp, "MTGFCP offset");

/**
 * @brief Get pointer to a per-port RMAC register block.
 *
 * @param[in] port Port identifier (k_ra8_rmac_port_0 or k_ra8_rmac_port_1).
 *
 * @return Pointer to the requested RMAC register window. Out-of-range
 *         port values fall back to port 0 so the caller still sees
 *         deterministic behaviour rather than a null-pointer
 *         dereference.
 *
 * @pre Power to the Ethernet subsystem is on (k_ra8_mstp_eswm enabled).
 * @pre ``port`` is one of ::ra8_rmac_port_t.
 * @post Returned pointer is non-null and aligned to 4 bytes.
 *
 * @note Not thread-safe; per-port serialisation is the caller's job.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_rmac_regs_t* ra8_rmac(ra8_rmac_port_t port)
{
  uintptr_t base = k_ra8_rmac0_base_addr;
  if (port == k_ra8_rmac_port_1) {
    base = k_ra8_rmac1_base_addr;
  }
  return (volatile r_rmac_regs_t*)base;
}

#ifdef __cplusplus
}
#endif
