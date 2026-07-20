/**
 * @file ra8_rmac.h
 * @brief Per-port Ethernet MAC (RMAC) driver -- HUM Ch 33
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HAL coverage of the RA8D2 RMAC block (HUM Ch 33, p 1703-1786).
 * RMAC is the on-chip MAC layer that talks to an off-chip PHY over
 * MII / RMII / GMII / RGMII / XGMII / XFI. It sits between the
 * per-port Ethernet Agent (ETHA, see @ref ra8_etha.h) above and the
 * off-chip PHY below:
 *
 * @verbatim
 *   ESWM (Ch 29, switch fabric) -- top
 *      |
 *   ETHA (Ch 32, per-port agent)
 *      |
 *   RMAC (Ch 33, per-port MAC)   <-- this driver
 *      |
 *   off-chip PHY                  -- bottom
 * @endverbatim
 *
 * The driver exposes every register field documented in HUM Ch 33.4
 * except those owned by ra8_eth_gptp (PTP timestamping). The split is:
 *
 *   - This driver: lifecycle, MAC address program, RX address
 *     filter table (perfect-match + multicast hash), VLAN tagging
 *     (via MTFFC framing), PFC priority groups (MTPFC3t), Energy
 *     Efficient Ethernet / LPI (MEEEC + MMIS2 LPI bits), magic-
 *     packet WoL (MRGC.MPDE + MMIS2.MPDIS), loopback (MLBC), link
 *     verification (MLVC), XGMII / 10G control (MXGMIIC, MPCH),
 *     PHY MDIO Clause-22 + Clause-45 (MPSM driven by MMIS1 events),
 *     all link speeds (MPIC.LSC), all duplex modes (MPIC.PIPP),
 *     statistics counters (MMPFTCT..MTXBCPL), and full IRQ
 *     management (MEIS/MEIE/MEID, MMIS0/1/2 + enables/disables).
 *   - ra8_eth_gptp: PTP timestamping itself; this driver only
 *     programs the MPFCt PTP filter slots and sets up the RMAC
 *     MTRC capture-config bits so the GPTP block sees the frame.
 *
 * @par State Machine
 * @dot
 * digraph ra8_rmac_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *   __end [shape=doublecircle, width=0.20, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   RESET [label="RESET"];
 *   CONFIG [label="CONFIG"];
 *   RUN [label="RUN"];
 *   SLEEP [label="SLEEP"];
 *   STOPPED [label="STOPPED"];
 *
 *   __start -> RESET [label="ra8_rmac_init()"];
 *   RESET -> CONFIG [label="caller drives ETHA into\\nCONFIG mode"];
 *   CONFIG -> RUN [label="caller drives ETHA into\\nOPERATION mode"];
 *   RUN -> SLEEP [label="ra8_rmac_enter_lpi() (TX LPI\\nrequest)"];
 *   SLEEP -> RUN [label="RX activity /\\nra8_rmac_exit_lpi()"];
 *   RUN -> STOPPED [label="ra8_rmac_deinit()"];
 *   STOPPED -> __end;
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rmac_regs.h"

/**
 * @enum ra8_rmac_mdc_clk_t
 * @brief MPIC PHY-station-management clock-rate constants.
 *
 * @details
 * The MPIC register packs three MDC-related fields:
 *   - PSMCS[22:16] : clock divider; MDC = ESWCLK / (2 * (PSMCS + 1)).
 *   - PSMHT[26:24] : MDIO hold-time adjust (FSP default 0).
 *   - PSMCT[30:28] : MDIO capture-time adjust (FSP default 0).
 *
 * Exposed publicly so the board-level bring-up code can pick a
 * conservative ``mdc_hz`` target without having to know the PSMCS
 * field width. IEEE 802.3 Clause 22 caps MDC at 2.5 MHz; the
 * GPY111 / PEF7071 family is happy at 1 MHz which is what the
 * default constant below provides.
 *
 * Cross-checked against FSP r_rmac_phy_calculate_mpic and CMSIS
 * R7KA8D2KF_core0.h R_RMAC0_MPIC_PSMCS_Pos / PSMHT_Pos / PSMCT_Pos.
 */
typedef enum : uint32_t {
  k_ra8_rmac_mdc_default_hz = 1000000UL, /**< 1 MHz MDC -- well below the 2.5 MHz ceiling. */
  k_ra8_rmac_mdc_min_hz     = 100000UL,  /**< Lower-bound sanity (PSMCS is 7 bits wide).   */
  k_ra8_rmac_mdc_psmcs_max  = 127UL,     /**< MPIC.PSMCS[22:16] is 7 bits wide.            */
} ra8_rmac_mdc_clk_t;

/**
 * @struct ra8_rmac_config_t
 * @brief Per-port RMAC configuration knobs.
 *
 * @details
 * Captured by ::ra8_rmac_init. All fields default to safe values when
 * zero-initialized: 100-Mbit MII full-duplex, unicast-only filter,
 * no IRQs enabled, no PFC, no EEE.
 */
typedef struct {
  ra8_rmac_mrafc_t  rx_filter;       /**< Initial MRAFC mask.           */
  uint32_t          err_irq_enable;  /**< Bits to OR into MEIE on init. */
  uint32_t          mon0_irq_enable; /**< Bits to OR into MMIE0.        */
  uint32_t          mon1_irq_enable; /**< Bits to OR into MMIE1.        */
  uint32_t          mon2_irq_enable; /**< Bits to OR into MMIE2.        */
  ra8_rmac_pis_t    phy_interface;   /**< Initial MPIC.PIS encoding.    */
  ra8_rmac_lsc_t    link_speed;      /**< Initial MPIC.LSC encoding.    */
  ra8_rmac_duplex_t duplex;          /**< Initial MPIC.PIPP encoding.   */
  /**
   * @brief Live ESWCLK frequency in Hz (input to the MDC divider).
   *
   * @details
   * Used together with ::mdc_hz to program MPIC.PSMCS. Must equal the
   * frequency actually delivered by ESWCKCR/ESWCKDIVCR after
   * ``ra8_cgc_eswclk_init`` runs. A value of 0 falls back to a safe
   * "PSMCS = max" code (slowest MDC).
   */
  uint32_t eswclk_hz;
  /**
   * @brief Target MDC (PHY station-management clock) frequency in Hz.
   *
   * @details
   * IEEE 802.3 Clause 22 caps MDC at 2.5 MHz; most PHYs (including the
   * GPY111 / PEF7071 on the EK-RA8D2) work happily at 1 MHz. A value
   * of 0 falls back to ::k_ra8_rmac_mdc_default_hz (1 MHz).
   */
  uint32_t mdc_hz;
} ra8_rmac_config_t;

/**
 * @enum ra8_rmac_phy_speed_t
 * @brief Resolved PHY link speed/duplex.
 *
 * @details
 * Returned by ::ra8_rmac_phy_auto_neg_wait once the off-chip PHY's
 * IEEE 802.3 Clause 22 BMSR.AN_COMPLETE bit asserts. Encodes the
 * highest-priority capability common to both link partners (the PHY
 * itself decodes the LPA register and presents the resolved value).
 */
typedef enum : uint8_t {
  k_ra8_rmac_phy_speed_unknown = 0U, /**< No link / not yet resolved. */
  k_ra8_rmac_phy_speed_10_hd   = 1U, /**< 10BASE-T half duplex.       */
  k_ra8_rmac_phy_speed_10_fd   = 2U, /**< 10BASE-T full duplex.       */
  k_ra8_rmac_phy_speed_100_hd  = 3U, /**< 100BASE-TX half duplex.     */
  k_ra8_rmac_phy_speed_100_fd  = 4U, /**< 100BASE-TX full duplex.     */
  k_ra8_rmac_phy_speed_count   = 5U, /**< Sentinel.                   */
} ra8_rmac_phy_speed_t;

/**
 * @struct ra8_rmac_phy_link_t
 * @brief Resolved PHY link status snapshot.
 *
 * @details
 * Populated by ::ra8_rmac_phy_auto_neg_wait and ::ra8_rmac_phy_link_status.
 * ``up`` mirrors IEEE 802.3 Clause 22 BMSR.LINK_STATUS (bit 2). When
 * ``up`` is true, ``speed`` reflects the negotiated speed/duplex pair
 * resolved against the link partner. When ``up`` is false the speed
 * value is ::k_ra8_rmac_phy_speed_unknown.
 */
typedef struct {
  bool                 up;    /**< true iff BMSR.LINK_STATUS = 1.   */
  ra8_rmac_phy_speed_t speed; /**< Negotiated speed/duplex when up. */
} ra8_rmac_phy_link_t;

/**
 * @enum ra8_rmac_phy_advert_t
 * @brief 802.3 Clause 22 ANAR (auto-negotiation advertisement) bits.
 *
 * @details
 * These match the IEEE 802.3 Clause 28.2.4 ANAR register bit layout
 * for 10/100M abilities. Pass any bitwise OR to
 * ::ra8_rmac_phy_set_advertise. The selector field (bits 0-4) is set
 * to ``00001`` (IEEE 802.3) by the helper; callers do not need to
 * include it.
 */
typedef enum : uint16_t {
  k_ra8_rmac_phy_advert_10_hd  = 0x0020U, /**< IEEE 802.3 ANAR.10BASE-T.      */
  k_ra8_rmac_phy_advert_10_fd  = 0x0040U, /**< IEEE 802.3 ANAR.10BASE-T FD.   */
  k_ra8_rmac_phy_advert_100_hd = 0x0080U, /**< IEEE 802.3 ANAR.100BASE-TX.    */
  k_ra8_rmac_phy_advert_100_fd = 0x0100U, /**< IEEE 802.3 ANAR.100BASE-TX FD. */
  k_ra8_rmac_phy_advert_pause  = 0x0400U, /**< IEEE 802.3 ANAR.PAUSE.         */
} ra8_rmac_phy_advert_t;

/**
 * @struct ra8_rmac_status_t
 * @brief Snapshot of an RMAC port's runtime state.
 *
 * @details
 * Returned by ::ra8_rmac_get_status. Includes the cached MAC-address
 * registers, PHY monitoring (link / LPI), and the three IRQ status
 * words (error + monitor[0..2]).
 */
typedef struct {
  uint32_t err_status;    /**< Raw MEIS contents.      */
  uint32_t mon_status[3]; /**< MMIS0..MMIS2 contents.  */
  uint32_t phy_monitor;   /**< Raw MPIM contents.      */
  uint32_t mrmac0;        /**< Cached MRMAC0 contents. */
  uint32_t mrmac1;        /**< Cached MRMAC1 contents. */
} ra8_rmac_status_t;

/**
 * @struct ra8_rmac_stats_t
 * @brief Snapshot of every RMAC statistic counter.
 *
 * @details
 * Returned by ::ra8_rmac_read_stats. Each member maps to the like-named
 * RMAC register in HUM Ch 33.4. RX and TX byte counters expose both
 * the upper and lower halves so callers can reconstruct the 64-bit
 * value: ``((uint64_t)rx_bytes_e_upper << 32) | rx_bytes_e_lower``.
 *
 * Counters are read in a fixed order so a single snapshot represents
 * a near-coherent view; callers that need true atomicity must mask
 * RMAC IRQs across the read.
 */
typedef struct {
  uint32_t pause_tx_manual;                           /**< MMPFTCT.  */
  uint32_t pause_tx_auto;                             /**< MAPFTCT.  */
  uint32_t pause_rx;                                  /**< MPFRCT.   */
  uint32_t false_carrier;                             /**< MFCICT.   */
  uint32_t eee_count;                                 /**< MEEECT.   */
  uint32_t pfc_tx_manual[k_ra8_rmac_pfc_group_count]; /**< MMPCFTCT  */
  uint32_t pfc_tx_auto[k_ra8_rmac_pfc_group_count];   /**< MAPCFTCT  */
  uint32_t pfc_rx[k_ra8_rmac_pfc_rx_count];           /**< MPCFRCT   */
  uint32_t rx_overflow;                               /**< MROVFC.   */
  uint32_t rx_hdr_crc_err;                            /**< MRHCRCEC. */
  uint32_t rx_good_e;                                 /**< MRGFCE.   */
  uint32_t rx_good_p;                                 /**< MRGFCP.   */
  uint32_t rx_broadcast;                              /**< MRBFC.    */
  uint32_t rx_multicast;                              /**< MRMFC.    */
  uint32_t rx_unicast;                                /**< MRUFC.    */
  uint32_t rx_phy_err;                                /**< MRPEFC.   */
  uint32_t rx_nibble_err;                             /**< MRNEFC.   */
  uint32_t rx_fcs_err;                                /**< MRFMEFC.  */
  uint32_t rx_final_frag_miss;                        /**< MRFFMEFC. */
  uint32_t rx_c_frag_err;                             /**< MRCFCEFC. */
  uint32_t rx_frag_count_err;                         /**< MRFCEFC.  */
  uint32_t rx_filter_rejected;                        /**< MRRCFEFC. */
  uint32_t rx_total;                                  /**< MRFC.     */
  uint32_t rx_good_undersize;                         /**< MRGUEFC.  */
  uint32_t rx_bad_undersize;                          /**< MRBUEFC.  */
  uint32_t rx_good_oversize;                          /**< MRGOEFC.  */
  uint32_t rx_bad_oversize;                           /**< MRBOEFC.  */
  uint32_t rx_bytes_e_upper;                          /**< MRXBCEU.  */
  uint32_t rx_bytes_e_lower;                          /**< MRXBCEL.  */
  uint32_t rx_bytes_p_upper;                          /**< MRXBCPU.  */
  uint32_t rx_bytes_p_lower;                          /**< MRXBCPL.  */
  uint32_t tx_good_e;                                 /**< MTGFCE.   */
  uint32_t tx_good_p;                                 /**< MTGFCP.   */
  uint32_t tx_broadcast;                              /**< MTBFC.    */
  uint32_t tx_multicast;                              /**< MTMFC.    */
  uint32_t tx_unicast;                                /**< MTUFC.    */
  uint32_t tx_error;                                  /**< MTEFC.    */
  uint32_t tx_bytes_e_upper;                          /**< MTXBCEU.  */
  uint32_t tx_bytes_e_lower;                          /**< MTXBCEL.  */
  uint32_t tx_bytes_p_upper;                          /**< MTXBCPU.  */
  uint32_t tx_bytes_p_lower;                          /**< MTXBCPL.  */
} ra8_rmac_stats_t;

/**
 * @typedef ra8_rmac_event_fn_t
 * @brief Per-port RMAC IRQ callback.
 *
 * @param[in] ctx     Caller-supplied opaque cookie.
 * @param[in] port    Port that raised the event.
 * @param[in] err_msk Snapshot of MEIS at dispatch time.
 * @param[in] mon0_msk Snapshot of MMIS0 at dispatch time.
 * @param[in] mon1_msk Snapshot of MMIS1 at dispatch time.
 * @param[in] mon2_msk Snapshot of MMIS2 at dispatch time.
 */
typedef void (*ra8_rmac_event_fn_t)(void*           ctx,
                                    ra8_rmac_port_t port,
                                    uint32_t        err_msk,
                                    uint32_t        mon0_msk,
                                    uint32_t        mon1_msk,
                                    uint32_t        mon2_msk);

/**
 * @brief Initialise an RMAC port (MSTP gate + reset regs + filter mode).
 *
 * @param[in] port Port identifier (::k_ra8_rmac_port_0 or _1).
 * @param[in] cfg  Per-port configuration. Must not be nullptr.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                Port brought up.
 * @retval k_ra8_err_null_ptr      cfg is nullptr.
 * @retval k_ra8_err_invalid_arg   port out of range or cfg field invalid.
 *
 * @pre Caller is single-threaded with respect to this port.
 * @pre Ethernet subsystem MSTP gate may already be enabled by ra8_eth.
 * @post MRAFC = cfg->rx_filter.
 * @post MEIE = cfg->err_irq_enable; MMIE0/1/2 set; status regs cleared.
 * @post MPIC.PIS / .LSC / .PIPP programmed from cfg.
 *
 * @note Per-port; safe to call concurrently for distinct ports.
 * @see ra8_rmac_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_init(ra8_rmac_port_t port, const ra8_rmac_config_t* cfg);

/**
 * @brief Tear down an RMAC port.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Port stopped, regs cleared.
 * @retval k_ra8_err_invalid_arg  port out of range.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre No outstanding TX/RX traffic on this port.
 * @post MRAFC, MRGC, MTFFC, MLBC, MEEEC cleared.
 * @post MEIE, MMIE0, MMIE1, MMIE2 cleared.
 *
 * @note Does NOT drop the shared ESWM MSTP gate; ra8_eth_deinit owns
 *       the final reference.
 * @see ra8_rmac_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_deinit(ra8_rmac_port_t port);

/**
 * @brief Enter low-power "stop" by disabling RX address filtering.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port has been brought up via ra8_rmac_init.
 * @pre No PHY MDIO transaction outstanding.
 * @post MRAFC = 0 (no frame class accepted).
 * @post Existing IRQ enables preserved so wake events still fire.
 *
 * @note Pair with ::ra8_rmac_exit_stop. Magic-packet WoL stays armed
 *       if MRGC.MPDE was set before this call.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_enter_stop(ra8_rmac_port_t port);

/**
 * @brief Exit low-power "stop" by reasserting the saved RX filter.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Caller previously invoked ::ra8_rmac_enter_stop on this port.
 * @pre Port still has its MSTP gate held by ra8_eth.
 * @post MRAFC restored to the value cached by ra8_rmac_init / set_filter.
 * @post Counters reset to zero so the new accounting window starts clean.
 *
 * @see ra8_rmac_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_exit_stop(ra8_rmac_port_t port);

/**
 * @brief Program the reception MAC address (MRMAC0 / MRMAC1).
 *
 * @param[in] port Port identifier.
 * @param[in] mac  6-byte network-byte-order MAC address (mac[0] = OUI hi).
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               MAC address programmed.
 * @retval k_ra8_err_null_ptr     mac is nullptr.
 * @retval k_ra8_err_invalid_arg  port out of range.
 *
 * @pre Port is in CONFIG mode (MRMAC0/1 are CONFIG-only writes per
 *      Table 33.4).
 * @pre mac points to a 6-byte readable buffer.
 * @post MRMAC0.MAU = mac[0..1] big-endian (MSB in bits [15:8]).
 * @post MRMAC1.MAL = mac[2..5] big-endian (mac[2] in bits [31:24]).
 *
 * @note Caller is responsible for switching to/from CONFIG mode via
 *       the ETHA EAMC.OPC field; see ra8_etha_init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_mac_address(ra8_rmac_port_t port,
                                                 const uint8_t   mac[k_ra8_rmac_mac_byte_count]);

/**
 * @brief Program the RX address filter mask (MRAFC).
 *
 * @param[in] port   Port identifier.
 * @param[in] filter Bitwise OR of ::ra8_rmac_mrafc_t values.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port has been brought up via ra8_rmac_init.
 * @pre filter only sets bits documented in ::ra8_rmac_mrafc_t.
 * @post MRAFC = filter.
 * @post Driver caches filter for ::ra8_rmac_exit_stop restore.
 *
 * @note Filter writes are CONFIG/OPERATION-mode safe per HUM
 *       Table 33.4 ("MRAFC -- C, O").
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_rx_filter(ra8_rmac_port_t port, ra8_rmac_mrafc_t filter);

/**
 * @brief Program a PTP-frame filter table entry (MPFCt, t = 0..15).
 *
 * @param[in] port   Port identifier.
 * @param[in] index  Slot index (0..15).
 * @param[in] byte_offset Offset into the frame payload to test.
 * @param[in] value  Reference byte value to compare.
 * @param[in] tef0   true to flag GPTP TX-event-frame trigger 0.
 * @param[in] tef1   true to flag GPTP TX-event-frame trigger 1.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre index < ::k_ra8_rmac_ptp_filter_count.
 * @post MPFC[index] = encoded slot.
 * @post Subsequent matching frames raise the requested GPTP trigger.
 *
 * @note Pair with ra8_eth_gptp to consume the timestamp.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_ptp_filter(ra8_rmac_port_t port,
                                                uint8_t         index,
                                                uint8_t         byte_offset,
                                                uint8_t         value,
                                                bool            tef0,
                                                bool            tef1);

/**
 * @brief Configure jumbo-frame minimum / maximum sizes.
 *
 * @param[in] port      Port identifier.
 * @param[in] is_pframe true for P-frames (MRFSCP), false for E-frames (MRFSCE).
 * @param[in] min_size  Minimum accepted frame size (bytes).
 * @param[in] max_size  Maximum accepted frame size (bytes, up to 16383).
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre min_size <= max_size.
 * @post MRFSCE / MRFSCP packed with min and max in the right halves.
 * @post Frames outside the window count toward MRGUEFC / MRBUEFC /
 *       MRGOEFC / MRBOEFC depending on size and FCS validity.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_set_frame_size(ra8_rmac_port_t port, bool is_pframe, uint16_t min_size, uint16_t max_size);

/**
 * @brief Configure VLAN double-tagging behaviour via MTFFC.
 *
 * @param[in] port    Port identifier.
 * @param[in] disable_pad true to disable TX pad (DPAD).
 * @param[in] use_mcrc true to use mCRC framing instead of standard CRC (FCM).
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @post MTFFC = (DPAD?1) | (FCM<<1).
 *
 * @note 802.1Q double-tagging itself is honoured by the chip when both
 *       outer and inner Ethertypes are recognised; this call only
 *       toggles the framing mode that the rest of the pipeline uses.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_set_vlan_framing(ra8_rmac_port_t port, bool disable_pad, bool use_mcrc);

/**
 * @brief Program a TX pause / PFC frame template.
 *
 * @param[in] port      Port identifier.
 * @param[in] mode      Pause-vs-PFC (MTPFC2.PFM).
 * @param[in] pause_time 16-bit pause quanta (MTPFC.PT).
 * @param[in] retry_time 8-bit retry threshold (MTPFC.PFRT).
 * @param[in] retry_level 5-bit retry-level (MTPFC.PFRLV).
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @post MTPFC packed with pause_time + retry_time + retry_level.
 * @post MTPFC2.PFM written from mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_pause_frame(ra8_rmac_port_t       port,
                                                 ra8_rmac_pause_mode_t mode,
                                                 uint16_t              pause_time,
                                                 uint8_t               retry_time,
                                                 uint8_t               retry_level);

/**
 * @brief Configure a PFC (802.1Qbb) priority group bitmap.
 *
 * @param[in] port      Port identifier.
 * @param[in] group     Priority group index (0..1).
 * @param[in] pcp_mask  8-bit PCP-priority bitmap.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre group < ::k_ra8_rmac_pfc_group_count.
 * @post MTPFC3[group] = pcp_mask & 0xFF.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_set_pfc_group(ra8_rmac_port_t port, ra8_rmac_pfc_group_t group, uint8_t pcp_mask);

/**
 * @brief Enable / disable Energy Efficient Ethernet LPI request.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true to assert TX LPI; false to clear.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre PHY supports IEEE 802.3az LPI (caller responsibility).
 * @post MEEEC.LPITR set / cleared accordingly.
 * @post Subsequent TX traffic enters / leaves LPI; MMIS2.LPIAIS /
 *       LPIDIS fires at the transition.
 *
 * @see ra8_rmac_get_status to observe MPIM.LPIA bit.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_lpi(ra8_rmac_port_t port, bool enable);

/**
 * @brief Enable / disable magic-packet wake-on-LAN detection.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true arms detection; false disarms.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre RX path is otherwise quiesced (only WoL frames expected).
 * @post MRGC.MPDE set / cleared.
 * @post On magic-packet arrival, MMIS2.MPDIS asserts and the IRQ
 *       handler fires if MMIE2.MPDIE is set.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_magic_packet(ra8_rmac_port_t port, bool enable);

/**
 * @brief Enable / disable internal MAC loopback.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true to short TX -> RX inside the MAC.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre PHY pulled into electrical loopback or detached for cleanest test.
 * @post MLBC.LBME set / cleared.
 *
 * @note Some PHYs also offer line-side loopback configured via
 *       MDIO; this driver only manages the on-chip MAC-side path.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_loopback(ra8_rmac_port_t port, bool enable);

/**
 * @brief Configure the link layer (interface, speed, duplex) atomically.
 *
 * @param[in] port    Port identifier.
 * @param[in] iface   PHY-side interface (GMII / MII / RMII / RGMII / XGMII).
 * @param[in] speed   Link speed (10 / 100 / 1000 / 2500 / 10000 Mbit/s).
 * @param[in] duplex  Half / full duplex.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre iface, speed, duplex are within their respective enums.
 * @post MPIC.PIS = iface; MPIC.LSC = speed; MPIC.PIPP = duplex.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_set_link(ra8_rmac_port_t   port,
                                          ra8_rmac_pis_t    iface,
                                          ra8_rmac_lsc_t    speed,
                                          ra8_rmac_duplex_t duplex);

/**
 * @brief Trigger an MDIO Clause-22 register read on the off-chip PHY.
 *
 * @param[in]  port      Port identifier.
 * @param[in]  phy_addr  5-bit PHY address (0..31).
 * @param[in]  reg_addr  5-bit register address (0..31).
 * @param[out] out_value 16-bit register value on success.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok            Read complete, *out_value populated.
 * @retval k_ra8_err_null_ptr  out_value is nullptr.
 * @retval k_ra8_err_hw_timeout MMIS1.PRACS never set within the bounded poll.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre MPIC.PSMCS programmed for the chosen MDC clock.
 * @post MPSM transaction issued; MMIS1.PRACS cleared.
 * @post PHY register snapshot in *out_value when k_ra8_ok is returned.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_mdio_c22_read(ra8_rmac_port_t port,
                                               uint8_t         phy_addr,
                                               uint8_t         reg_addr,
                                               uint16_t*       out_value);

/**
 * @brief Trigger an MDIO Clause-22 register write on the off-chip PHY.
 *
 * @param[in] port      Port identifier.
 * @param[in] phy_addr  5-bit PHY address.
 * @param[in] reg_addr  5-bit register address.
 * @param[in] value     16-bit value to drive into the PHY register.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre MPIC.PSMCS programmed for the chosen MDC clock.
 * @post MPSM transaction issued; MMIS1.PWACS cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_mdio_c22_write(ra8_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t value);

/**
 * @brief Trigger an MDIO Clause-45 register read.
 *
 * @param[in]  port     Port identifier.
 * @param[in]  phy_addr 5-bit PHY address.
 * @param[in]  dev_addr 5-bit MMD device address.
 * @param[in]  reg_addr 16-bit MMD register address.
 * @param[out] out_value 16-bit register value on success.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok            Read complete.
 * @retval k_ra8_err_null_ptr  out_value is nullptr.
 * @retval k_ra8_err_hw_timeout MMIS1 completion bit never asserted.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre MPIC.PSMHT configured for Clause-45 hold time.
 * @post Address frame followed by read frame both completed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_mdio_c45_read(ra8_rmac_port_t port,
                                               uint8_t         phy_addr,
                                               uint8_t         dev_addr,
                                               uint16_t        reg_addr,
                                               uint16_t*       out_value);

/**
 * @brief Trigger an MDIO Clause-45 register write.
 *
 * @param[in] port     Port identifier.
 * @param[in] phy_addr 5-bit PHY address.
 * @param[in] dev_addr 5-bit MMD device address.
 * @param[in] reg_addr 16-bit MMD register address.
 * @param[in] value    16-bit value to drive into the MMD register.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre MPIC.PSMHT configured for Clause-45 hold time.
 * @post Address frame followed by write frame both completed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_mdio_c45_write(ra8_rmac_port_t port,
                                                uint8_t         phy_addr,
                                                uint8_t         dev_addr,
                                                uint16_t        reg_addr,
                                                uint16_t        value);

/**
 * @brief Read a port's status snapshot (MEIS + MMIS0..2 + MPIM + MAC).
 *
 * @param[in]  port Port identifier.
 * @param[out] out  Destination for the status snapshot.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port has been brought up via ra8_rmac_init.
 * @pre out is a writable pointer.
 * @post out->err_status reflects MEIS at the call site.
 * @post out->mon_status[i] reflect MMIS{i} for i in 0..2.
 *
 * @note Reads are non-destructive; status bits are cleared via
 *       ::ra8_rmac_clear_status.
 * @see ra8_rmac_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_get_status(ra8_rmac_port_t port, ra8_rmac_status_t* out);

/**
 * @brief Clear bits in MEIS / MMIS0 / MMIS1 / MMIS2.
 *
 * @param[in] port      Port identifier.
 * @param[in] err_mask  Bits to clear in MEIS via MEID.
 * @param[in] mon0_mask Bits to clear in MMIS0 via MMID0.
 * @param[in] mon1_mask Bits to clear in MMIS1 via MMID1.
 * @param[in] mon2_mask Bits to clear in MMIS2 via MMID2.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre Caller has read the status registers via ::ra8_rmac_get_status.
 * @post Bits in masks are cleared in their respective status registers.
 * @post Re-reads of the status registers exclude the cleared bits.
 *
 * @note IRQ-safe; the underlying writes are atomic on Cortex-M85.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_clear_status(ra8_rmac_port_t port,
                                              uint32_t        err_mask,
                                              uint32_t        mon0_mask,
                                              uint32_t        mon1_mask,
                                              uint32_t        mon2_mask);

/**
 * @brief Read the full statistic counter snapshot (HUM Ch 33.4).
 *
 * @param[in]  port Port identifier.
 * @param[out] out  Destination for the counter snapshot.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port has been brought up via ra8_rmac_init.
 * @pre out is a writable pointer.
 * @post Every member in out matches its register at read time.
 * @post Hardware counters keep accumulating; this is a read-only snap.
 *
 * @note Counters are read-only; clearing happens via deinit/init or
 *       a controlled reset of the ETHA mode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_read_stats(ra8_rmac_port_t port, ra8_rmac_stats_t* out);

/**
 * @brief Attach a per-port IRQ handler for RMAC events.
 *
 * @param[in] port Port identifier.
 * @param[in] cb   Callback (nullptr to detach).
 * @param[in] ctx  Opaque cookie passed back to cb.
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre Caller is single-threaded with respect to attach/dispatch.
 * @post Subsequent calls to ::ra8_rmac_dispatch invoke cb.
 *
 * @see ra8_rmac_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_attach_handler(ra8_rmac_port_t port, ra8_rmac_event_fn_t cb, void* ctx);

/**
 * @brief Dispatch a pending RMAC IRQ.
 *
 * @param[in] port Port identifier.
 *
 * @details
 * Reads MEIS / MMIS0..2 atomically, clears every observed bit through
 * the matching MEID / MMID register, then invokes the attached
 * callback with the snapshot. Designed to be called from the NVIC ISR
 * registered for the RMAC error / monitor interrupt vectors.
 *
 * @pre Port previously brought up via ra8_rmac_init.
 * @pre Called from ISR context or a host-test driver.
 * @post All four status registers are cleared.
 * @post Callback (if any) has run with the snapshot.
 *
 * @note Not thread-safe; pair with NVIC masking. See HUM Ch 41
 *       "Reduced Media Access Controller (RMAC)" pp 1893-2018.
 * @see ra8_rmac_attach_handler
 * @since 0.1.0
 */
void ra8_rmac_dispatch(ra8_rmac_port_t port);

/**
 * @brief Software-reset an off-chip PHY via Clause-22 MDIO.
 *
 * @details
 * Writes IEEE 802.3 Clause 22 BMCR.RESET (bit 15), then polls BMCR
 * until the reset bit self-clears or a bounded poll budget is
 * exhausted.
 *
 * @param[in] port     Port identifier (::k_ra8_rmac_port_0 / _1).
 * @param[in] phy_addr 5-bit PHY address on the MDIO bus (0..31).
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               PHY reset complete.
 * @retval k_ra8_err_invalid_arg  port or phy_addr out of range.
 * @retval k_ra8_err_hw_timeout   BMCR.RESET never self-cleared.
 *
 * @pre Port previously brought up via ::ra8_rmac_init.
 * @pre MPIC.PSMCS programmed for the chosen MDC clock.
 * @post BMCR.RESET observed cleared by the PHY.
 * @post All other BMCR bits reset to power-on defaults by the PHY.
 *
 * @note IEEE 802.3 Clause 22 sec 22.2.4.1.1 "BMCR.RESET" defines the
 *       self-clearing semantics this function relies on.
 * @see ra8_rmac_phy_auto_neg_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_reset(ra8_rmac_port_t port, uint8_t phy_addr);

/**
 * @brief Program the off-chip PHY's auto-negotiation advertisement.
 *
 * @details
 * Writes the IEEE 802.3 Clause 22 ANAR register (register 4) with
 * the supplied capability mask plus the IEEE 802.3 selector field
 * (00001 in bits [4:0]). After updating the advertisement, callers
 * typically invoke ::ra8_rmac_phy_auto_neg_start to cause the PHY to
 * restart auto-negotiation with the new capabilities.
 *
 * @param[in] port         Port identifier.
 * @param[in] phy_addr     5-bit PHY address (0..31).
 * @param[in] capabilities OR of ::ra8_rmac_phy_advert_t bits.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               ANAR programmed.
 * @retval k_ra8_err_invalid_arg  port or phy_addr out of range.
 * @retval k_ra8_err_hw_timeout   MDIO transaction never completed.
 *
 * @pre Port previously brought up via ::ra8_rmac_init.
 * @pre PHY is in a state that accepts ANAR writes (post reset OK).
 * @post ANAR = capabilities | 0x0001 (selector field).
 * @post Auto-neg restart still required to advertise new value.
 *
 * @note IEEE 802.3 Clause 28.2.4 "AN advertisement register" defines
 *       the bit layout this helper writes.
 * @see ra8_rmac_phy_auto_neg_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_phy_set_advertise(ra8_rmac_port_t port, uint8_t phy_addr, uint16_t capabilities);

/**
 * @brief Kick off (or restart) auto-negotiation on the off-chip PHY.
 *
 * @details
 * Sets IEEE 802.3 Clause 22 BMCR.AN_ENABLE (bit 12) and
 * BMCR.AN_RESTART (bit 9) in a single MDIO write. Pair this call
 * with ::ra8_rmac_phy_auto_neg_wait to block until the PHY reports
 * AN_COMPLETE in BMSR.
 *
 * @param[in] port     Port identifier.
 * @param[in] phy_addr 5-bit PHY address.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               AN restart issued.
 * @retval k_ra8_err_invalid_arg  port or phy_addr out of range.
 * @retval k_ra8_err_hw_timeout   MDIO transaction never completed.
 *
 * @pre Port previously brought up via ::ra8_rmac_init.
 * @pre PHY ANAR programmed (see ::ra8_rmac_phy_set_advertise).
 * @post BMCR.AN_ENABLE = 1; BMCR.AN_RESTART = 1.
 * @post PHY transitions to AN_ABILITY_DETECT per IEEE 802.3 Clause 28.
 *
 * @note IEEE 802.3 Clause 22 sec 22.2.4.1 "BMCR" bit definitions.
 * @see ra8_rmac_phy_auto_neg_wait
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_auto_neg_start(ra8_rmac_port_t port, uint8_t phy_addr);

/**
 * @brief Block (with a bounded poll) until the PHY reports AN_COMPLETE.
 *
 * @details
 * Reads IEEE 802.3 Clause 22 BMSR (register 1) in a loop; each
 * iteration costs one MDIO transaction. Returns success when both
 * BMSR.AN_COMPLETE (bit 5) and BMSR.LINK_STATUS (bit 2) are observed
 * set in the same read. The resolved speed/duplex is decoded from
 * the negotiated 10/100 capability bits in ANLPAR (register 5) once
 * AN_COMPLETE asserts.
 *
 * @param[in]  port       Port identifier.
 * @param[in]  phy_addr   5-bit PHY address.
 * @param[in]  timeout_ms Maximum wait in milliseconds (0 -> internal cap).
 * @param[out] out_link   Resolved link state on success.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               AN_COMPLETE + LINK_STATUS observed.
 * @retval k_ra8_err_null_ptr     out_link is nullptr.
 * @retval k_ra8_err_invalid_arg  port or phy_addr out of range.
 * @retval k_ra8_err_hw_timeout   AN_COMPLETE never asserted.
 *
 * @pre Port previously brought up via ::ra8_rmac_init.
 * @pre Auto-neg started via ::ra8_rmac_phy_auto_neg_start.
 * @post On k_ra8_ok, *out_link reflects negotiated speed/duplex/link.
 * @post On k_ra8_err_hw_timeout, *out_link->up = false.
 *
 * @note IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR" defines AN_COMPLETE
 *       and LINK_STATUS; Clause 28.2.4.4 "ANLPAR" defines the
 *       resolved-capability bit layout this helper decodes.
 * @see ra8_rmac_phy_auto_neg_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_auto_neg_wait(ra8_rmac_port_t      port,
                                                   uint8_t              phy_addr,
                                                   uint32_t             timeout_ms,
                                                   ra8_rmac_phy_link_t* out_link);

/**
 * @brief Quick poll of BMSR.LINK_STATUS (no auto-neg block).
 *
 * @details
 * Single MDIO read of IEEE 802.3 Clause 22 BMSR. Reports up/down
 * via ``out_link->up``. When up is true the speed field is decoded
 * from ANLPAR if AN_COMPLETE is set, otherwise it is left as
 * ::k_ra8_rmac_phy_speed_unknown.
 *
 * @param[in]  port     Port identifier.
 * @param[in]  phy_addr 5-bit PHY address.
 * @param[out] out_link Destination for the snapshot.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               BMSR read OK.
 * @retval k_ra8_err_null_ptr     out_link is nullptr.
 * @retval k_ra8_err_invalid_arg  port or phy_addr out of range.
 * @retval k_ra8_err_hw_timeout   MDIO transaction never completed.
 *
 * @pre Port previously brought up via ::ra8_rmac_init.
 * @pre PHY visible on the MDIO bus.
 * @post out_link populated; no PHY register state changed.
 *
 * @note IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR.LINK_STATUS" is
 *       latching-low: a single read clears any latched-down event.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rmac_phy_link_status(ra8_rmac_port_t port, uint8_t phy_addr, ra8_rmac_phy_link_t* out_link);

#ifdef __cplusplus
}
#endif
