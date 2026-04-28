/**
 * @file ra_rmac.h
 * @brief Per-port Ethernet MAC (RMAC) driver -- HUM Ch 33
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HAL coverage of the RA8D2 RMAC block (HUM Ch 33, p 1703-1786).
 * RMAC is the on-chip MAC layer that talks to an off-chip PHY over
 * MII / RMII / GMII / RGMII / XGMII / XFI. It sits between the
 * per-port Ethernet Agent (ETHA, see @ref ra_etha.h) above and the
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
 * except those owned by ra_eth_gptp (PTP timestamping). The split is:
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
 *   - ra_eth_gptp: PTP timestamping itself; this driver only
 *     programs the MPFCt PTP filter slots and sets up the RMAC
 *     MTRC capture-config bits so the GPTP block sees the frame.
 *
 * @par State Machine
 * @startuml
 * [*] --> RESET     : ra_rmac_init()
 * RESET   --> CONFIG : caller drives ETHA into CONFIG mode
 * CONFIG  --> RUN    : caller drives ETHA into OPERATION mode
 * RUN     --> SLEEP  : ra_rmac_enter_lpi() (TX LPI request)
 * SLEEP   --> RUN    : RX activity / ra_rmac_exit_lpi()
 * RUN     --> STOPPED: ra_rmac_deinit()
 * STOPPED --> [*]
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_rmac_regs.h"
#include "ra_err.h"

/**
 * @struct ra_rmac_config_t
 * @brief Per-port RMAC configuration knobs.
 *
 * @details
 * Captured by ::ra_rmac_init. All fields default to safe values when
 * zero-initialised: 100-Mbit MII full-duplex, unicast-only filter,
 * no IRQs enabled, no PFC, no EEE.
 */
typedef struct {
  ra_rmac_mrafc_t  rx_filter;       /**< Initial MRAFC mask.            */
  uint32_t         err_irq_enable;  /**< Bits to OR into MEIE on init.*/
  uint32_t         mon0_irq_enable; /**< Bits to OR into MMIE0.       */
  uint32_t         mon1_irq_enable; /**< Bits to OR into MMIE1.       */
  uint32_t         mon2_irq_enable; /**< Bits to OR into MMIE2.       */
  ra_rmac_pis_t    phy_interface;   /**< Initial MPIC.PIS encoding.     */
  ra_rmac_lsc_t    link_speed;      /**< Initial MPIC.LSC encoding.     */
  ra_rmac_duplex_t duplex;          /**< Initial MPIC.PIPP encoding.    */
} ra_rmac_config_t;

/**
 * @struct ra_rmac_status_t
 * @brief Snapshot of an RMAC port's runtime state.
 *
 * @details
 * Returned by ::ra_rmac_get_status. Includes the cached MAC-address
 * registers, PHY monitoring (link / LPI), and the three IRQ status
 * words (error + monitor[0..2]).
 */
typedef struct {
  uint32_t err_status;    /**< Raw MEIS contents.                  */
  uint32_t mon_status[3]; /**< MMIS0..MMIS2 contents.              */
  uint32_t phy_monitor;   /**< Raw MPIM contents.                  */
  uint32_t mrmac0;        /**< Cached MRMAC0 contents.             */
  uint32_t mrmac1;        /**< Cached MRMAC1 contents.             */
} ra_rmac_status_t;

/**
 * @struct ra_rmac_stats_t
 * @brief Snapshot of every RMAC statistic counter.
 *
 * @details
 * Returned by ::ra_rmac_read_stats. Each member maps to the like-named
 * RMAC register in HUM Ch 33.4. RX and TX byte counters expose both
 * the upper and lower halves so callers can reconstruct the 64-bit
 * value: ``((uint64_t)rx_bytes_e_upper << 32) | rx_bytes_e_lower``.
 *
 * Counters are read in a fixed order so a single snapshot represents
 * a near-coherent view; callers that need true atomicity must mask
 * RMAC IRQs across the read.
 */
typedef struct {
  uint32_t pause_tx_manual;                          /**< MMPFTCT.                       */
  uint32_t pause_tx_auto;                            /**< MAPFTCT.                       */
  uint32_t pause_rx;                                 /**< MPFRCT.                        */
  uint32_t false_carrier;                            /**< MFCICT.                        */
  uint32_t eee_count;                                /**< MEEECT.                        */
  uint32_t pfc_tx_manual[k_ra_rmac_pfc_group_count]; /**< MMPCFTCT*/
  uint32_t pfc_tx_auto[k_ra_rmac_pfc_group_count];   /**< MAPCFTCT*/
  uint32_t pfc_rx[k_ra_rmac_pfc_rx_count];           /**< MPCFRCT */
  uint32_t rx_overflow;                              /**< MROVFC.                        */
  uint32_t rx_hdr_crc_err;                           /**< MRHCRCEC.                      */
  uint32_t rx_good_e;                                /**< MRGFCE.                        */
  uint32_t rx_good_p;                                /**< MRGFCP.                        */
  uint32_t rx_broadcast;                             /**< MRBFC.                         */
  uint32_t rx_multicast;                             /**< MRMFC.                         */
  uint32_t rx_unicast;                               /**< MRUFC.                         */
  uint32_t rx_phy_err;                               /**< MRPEFC.                        */
  uint32_t rx_nibble_err;                            /**< MRNEFC.                        */
  uint32_t rx_fcs_err;                               /**< MRFMEFC.                       */
  uint32_t rx_final_frag_miss;                       /**< MRFFMEFC.                      */
  uint32_t rx_c_frag_err;                            /**< MRCFCEFC.                      */
  uint32_t rx_frag_count_err;                        /**< MRFCEFC.                       */
  uint32_t rx_filter_rejected;                       /**< MRRCFEFC.                      */
  uint32_t rx_total;                                 /**< MRFC.                          */
  uint32_t rx_good_undersize;                        /**< MRGUEFC.                       */
  uint32_t rx_bad_undersize;                         /**< MRBUEFC.                       */
  uint32_t rx_good_oversize;                         /**< MRGOEFC.                       */
  uint32_t rx_bad_oversize;                          /**< MRBOEFC.                       */
  uint32_t rx_bytes_e_upper;                         /**< MRXBCEU.                       */
  uint32_t rx_bytes_e_lower;                         /**< MRXBCEL.                       */
  uint32_t rx_bytes_p_upper;                         /**< MRXBCPU.                       */
  uint32_t rx_bytes_p_lower;                         /**< MRXBCPL.                       */
  uint32_t tx_good_e;                                /**< MTGFCE.                        */
  uint32_t tx_good_p;                                /**< MTGFCP.                        */
  uint32_t tx_broadcast;                             /**< MTBFC.                         */
  uint32_t tx_multicast;                             /**< MTMFC.                         */
  uint32_t tx_unicast;                               /**< MTUFC.                         */
  uint32_t tx_error;                                 /**< MTEFC.                         */
  uint32_t tx_bytes_e_upper;                         /**< MTXBCEU.                       */
  uint32_t tx_bytes_e_lower;                         /**< MTXBCEL.                       */
  uint32_t tx_bytes_p_upper;                         /**< MTXBCPU.                       */
  uint32_t tx_bytes_p_lower;                         /**< MTXBCPL.                       */
} ra_rmac_stats_t;

/**
 * @typedef ra_rmac_event_fn_t
 * @brief Per-port RMAC IRQ callback.
 *
 * @param[in] ctx     Caller-supplied opaque cookie.
 * @param[in] port    Port that raised the event.
 * @param[in] err_msk Snapshot of MEIS at dispatch time.
 * @param[in] mon0_msk Snapshot of MMIS0 at dispatch time.
 * @param[in] mon1_msk Snapshot of MMIS1 at dispatch time.
 * @param[in] mon2_msk Snapshot of MMIS2 at dispatch time.
 */
typedef void (*ra_rmac_event_fn_t)(void*          ctx,
                                   ra_rmac_port_t port,
                                   uint32_t       err_msk,
                                   uint32_t       mon0_msk,
                                   uint32_t       mon1_msk,
                                   uint32_t       mon2_msk);

/**
 * @brief Initialise an RMAC port (MSTP gate + reset regs + filter mode).
 *
 * @param[in] port Port identifier (::k_ra_rmac_port_0 or _1).
 * @param[in] cfg  Per-port configuration. Must not be nullptr.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok                Port brought up.
 * @retval k_ra_err_null_ptr      cfg is nullptr.
 * @retval k_ra_err_invalid_arg   port out of range or cfg field invalid.
 *
 * @pre Caller is single-threaded with respect to this port.
 * @pre Ethernet subsystem MSTP gate may already be enabled by ra_eth.
 * @post MRAFC = cfg->rx_filter.
 * @post MEIE = cfg->err_irq_enable; MMIE0/1/2 set; status regs cleared.
 * @post MPIC.PIS / .LSC / .PIPP programmed from cfg.
 *
 * @note Per-port; safe to call concurrently for distinct ports.
 * @see ra_rmac_deinit
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_init(ra_rmac_port_t port, const ra_rmac_config_t* cfg);

/**
 * @brief Tear down an RMAC port.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Port stopped, regs cleared.
 * @retval k_ra_err_invalid_arg  port out of range.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre No outstanding TX/RX traffic on this port.
 * @post MRAFC, MRGC, MTFFC, MLBC, MEEEC cleared.
 * @post MEIE, MMIE0, MMIE1, MMIE2 cleared.
 *
 * @note Does NOT drop the shared ESWM MSTP gate; ra_eth_deinit owns
 *       the final reference.
 * @see ra_rmac_init
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_deinit(ra_rmac_port_t port);

/**
 * @brief Enter low-power "stop" by disabling RX address filtering.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port has been brought up via ra_rmac_init.
 * @pre No PHY MDIO transaction outstanding.
 * @post MRAFC = 0 (no frame class accepted).
 * @post Existing IRQ enables preserved so wake events still fire.
 *
 * @note Pair with ::ra_rmac_exit_stop. Magic-packet WoL stays armed
 *       if MRGC.MPDE was set before this call.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_enter_stop(ra_rmac_port_t port);

/**
 * @brief Exit low-power "stop" by reasserting the saved RX filter.
 *
 * @param[in] port Port identifier.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Caller previously invoked ::ra_rmac_enter_stop on this port.
 * @pre Port still has its MSTP gate held by ra_eth.
 * @post MRAFC restored to the value cached by ra_rmac_init / set_filter.
 * @post Counters reset to zero so the new accounting window starts clean.
 *
 * @see ra_rmac_enter_stop
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_exit_stop(ra_rmac_port_t port);

/**
 * @brief Program the reception MAC address (MRMAC0 / MRMAC1).
 *
 * @param[in] port Port identifier.
 * @param[in] mac  6-byte network-byte-order MAC address (mac[0] = OUI hi).
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               MAC address programmed.
 * @retval k_ra_err_null_ptr     mac is nullptr.
 * @retval k_ra_err_invalid_arg  port out of range.
 *
 * @pre Port is in CONFIG mode (MRMAC0/1 are CONFIG-only writes per
 *      Table 33.4).
 * @pre mac points to a 6-byte readable buffer.
 * @post MRMAC0.MAU = mac[0..1] big-endian (MSB in bits [15:8]).
 * @post MRMAC1.MAL = mac[2..5] big-endian (mac[2] in bits [31:24]).
 *
 * @note Caller is responsible for switching to/from CONFIG mode via
 *       the ETHA EAMC.OPC field; see ra_etha_init.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_mac_address(ra_rmac_port_t port,
                                               const uint8_t  mac[k_ra_rmac_mac_byte_count]);

/**
 * @brief Program the RX address filter mask (MRAFC).
 *
 * @param[in] port   Port identifier.
 * @param[in] filter Bitwise OR of ::ra_rmac_mrafc_t values.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port has been brought up via ra_rmac_init.
 * @pre filter only sets bits documented in ::ra_rmac_mrafc_t.
 * @post MRAFC = filter.
 * @post Driver caches filter for ::ra_rmac_exit_stop restore.
 *
 * @note Filter writes are CONFIG/OPERATION-mode safe per HUM
 *       Table 33.4 ("MRAFC -- C, O").
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_rx_filter(ra_rmac_port_t port, ra_rmac_mrafc_t filter);

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
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre index < ::k_ra_rmac_ptp_filter_count.
 * @post MPFC[index] = encoded slot.
 * @post Subsequent matching frames raise the requested GPTP trigger.
 *
 * @note Pair with ra_eth_gptp to consume the timestamp.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_ptp_filter(ra_rmac_port_t port,
                                              uint8_t        index,
                                              uint8_t        byte_offset,
                                              uint8_t        value,
                                              bool           tef0,
                                              bool           tef1);

/**
 * @brief Configure jumbo-frame minimum / maximum sizes.
 *
 * @param[in] port      Port identifier.
 * @param[in] is_pframe true for P-frames (MRFSCP), false for E-frames (MRFSCE).
 * @param[in] min_size  Minimum accepted frame size (bytes).
 * @param[in] max_size  Maximum accepted frame size (bytes, up to 16383).
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre min_size <= max_size.
 * @post MRFSCE / MRFSCP packed with min and max in the right halves.
 * @post Frames outside the window count toward MRGUEFC / MRBUEFC /
 *       MRGOEFC / MRBOEFC depending on size and FCS validity.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_set_frame_size(ra_rmac_port_t port, bool is_pframe, uint16_t min_size, uint16_t max_size);

/**
 * @brief Configure VLAN double-tagging behaviour via MTFFC.
 *
 * @param[in] port    Port identifier.
 * @param[in] disable_pad true to disable TX pad (DPAD).
 * @param[in] use_mcrc true to use mCRC framing instead of standard CRC (FCM).
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @post MTFFC = (DPAD?1) | (FCM<<1).
 *
 * @note 802.1Q double-tagging itself is honoured by the chip when both
 *       outer and inner Ethertypes are recognised; this call only
 *       toggles the framing mode that the rest of the pipeline uses.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_set_vlan_framing(ra_rmac_port_t port, bool disable_pad, bool use_mcrc);

/**
 * @brief Program a TX pause / PFC frame template.
 *
 * @param[in] port      Port identifier.
 * @param[in] mode      Pause-vs-PFC (MTPFC2.PFM).
 * @param[in] pause_time 16-bit pause quanta (MTPFC.PT).
 * @param[in] retry_time 8-bit retry threshold (MTPFC.PFRT).
 * @param[in] retry_level 5-bit retry-level (MTPFC.PFRLV).
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @post MTPFC packed with pause_time + retry_time + retry_level.
 * @post MTPFC2.PFM written from mode.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_pause_frame(ra_rmac_port_t       port,
                                               ra_rmac_pause_mode_t mode,
                                               uint16_t             pause_time,
                                               uint8_t              retry_time,
                                               uint8_t              retry_level);

/**
 * @brief Configure a PFC (802.1Qbb) priority group bitmap.
 *
 * @param[in] port      Port identifier.
 * @param[in] group     Priority group index (0..1).
 * @param[in] pcp_mask  8-bit PCP-priority bitmap.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre group < ::k_ra_rmac_pfc_group_count.
 * @post MTPFC3[group] = pcp_mask & 0xFF.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_set_pfc_group(ra_rmac_port_t port, ra_rmac_pfc_group_t group, uint8_t pcp_mask);

/**
 * @brief Enable / disable Energy Efficient Ethernet LPI request.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true to assert TX LPI; false to clear.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre PHY supports IEEE 802.3az LPI (caller responsibility).
 * @post MEEEC.LPITR set / cleared accordingly.
 * @post Subsequent TX traffic enters / leaves LPI; MMIS2.LPIAIS /
 *       LPIDIS fires at the transition.
 *
 * @see ra_rmac_get_status to observe MPIM.LPIA bit.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_lpi(ra_rmac_port_t port, bool enable);

/**
 * @brief Enable / disable magic-packet wake-on-LAN detection.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true arms detection; false disarms.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre RX path is otherwise quiesced (only WoL frames expected).
 * @post MRGC.MPDE set / cleared.
 * @post On magic-packet arrival, MMIS2.MPDIS asserts and the IRQ
 *       handler fires if MMIE2.MPDIE is set.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_magic_packet(ra_rmac_port_t port, bool enable);

/**
 * @brief Enable / disable internal MAC loopback.
 *
 * @param[in] port   Port identifier.
 * @param[in] enable true to short TX -> RX inside the MAC.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre PHY pulled into electrical loopback or detached for cleanest test.
 * @post MLBC.LBME set / cleared.
 *
 * @note Some PHYs also offer line-side loopback configured via
 *       MDIO; this driver only manages the on-chip MAC-side path.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_loopback(ra_rmac_port_t port, bool enable);

/**
 * @brief Configure the link layer (interface, speed, duplex) atomically.
 *
 * @param[in] port    Port identifier.
 * @param[in] iface   PHY-side interface (GMII / MII / RMII / RGMII / XGMII).
 * @param[in] speed   Link speed (10 / 100 / 1000 / 2500 / 10000 Mbit/s).
 * @param[in] duplex  Half / full duplex.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port is in CONFIG mode.
 * @pre iface, speed, duplex are within their respective enums.
 * @post MPIC.PIS = iface; MPIC.LSC = speed; MPIC.PIPP = duplex.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_set_link(ra_rmac_port_t   port,
                                        ra_rmac_pis_t    iface,
                                        ra_rmac_lsc_t    speed,
                                        ra_rmac_duplex_t duplex);

/**
 * @brief Trigger an MDIO Clause-22 register read on the off-chip PHY.
 *
 * @param[in]  port      Port identifier.
 * @param[in]  phy_addr  5-bit PHY address (0..31).
 * @param[in]  reg_addr  5-bit register address (0..31).
 * @param[out] out_value 16-bit register value on success.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok            Read complete, *out_value populated.
 * @retval k_ra_err_null_ptr  out_value is nullptr.
 * @retval k_ra_err_hw_timeout MMIS1.PRACS never set within the bounded poll.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre MPIC.PSMCS programmed for the chosen MDC clock.
 * @post MPSM transaction issued; MMIS1.PRACS cleared.
 * @post PHY register snapshot in *out_value when k_ra_ok is returned.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_mdio_c22_read(ra_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t* out_value);

/**
 * @brief Trigger an MDIO Clause-22 register write on the off-chip PHY.
 *
 * @param[in] port      Port identifier.
 * @param[in] phy_addr  5-bit PHY address.
 * @param[in] reg_addr  5-bit register address.
 * @param[in] value     16-bit value to drive into the PHY register.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre MPIC.PSMCS programmed for the chosen MDC clock.
 * @post MPSM transaction issued; MMIS1.PWACS cleared.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_mdio_c22_write(ra_rmac_port_t port, uint8_t phy_addr, uint8_t reg_addr, uint16_t value);

/**
 * @brief Trigger an MDIO Clause-45 register read.
 *
 * @param[in]  port     Port identifier.
 * @param[in]  phy_addr 5-bit PHY address.
 * @param[in]  dev_addr 5-bit MMD device address.
 * @param[in]  reg_addr 16-bit MMD register address.
 * @param[out] out_value 16-bit register value on success.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok            Read complete.
 * @retval k_ra_err_null_ptr  out_value is nullptr.
 * @retval k_ra_err_hw_timeout MMIS1 completion bit never asserted.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre MPIC.PSMHT configured for Clause-45 hold time.
 * @post Address frame followed by read frame both completed.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_mdio_c45_read(ra_rmac_port_t port,
                                             uint8_t        phy_addr,
                                             uint8_t        dev_addr,
                                             uint16_t       reg_addr,
                                             uint16_t*      out_value);

/**
 * @brief Trigger an MDIO Clause-45 register write.
 *
 * @param[in] port     Port identifier.
 * @param[in] phy_addr 5-bit PHY address.
 * @param[in] dev_addr 5-bit MMD device address.
 * @param[in] reg_addr 16-bit MMD register address.
 * @param[in] value    16-bit value to drive into the MMD register.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre MPIC.PSMHT configured for Clause-45 hold time.
 * @post Address frame followed by write frame both completed.
 *
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_mdio_c45_write(ra_rmac_port_t port,
                                              uint8_t        phy_addr,
                                              uint8_t        dev_addr,
                                              uint16_t       reg_addr,
                                              uint16_t       value);

/**
 * @brief Read a port's status snapshot (MEIS + MMIS0..2 + MPIM + MAC).
 *
 * @param[in]  port Port identifier.
 * @param[out] out  Destination for the status snapshot.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port has been brought up via ra_rmac_init.
 * @pre out is a writable pointer.
 * @post out->err_status reflects MEIS at the call site.
 * @post out->mon_status[i] reflect MMIS{i} for i in 0..2.
 *
 * @note Reads are non-destructive; status bits are cleared via
 *       ::ra_rmac_clear_status.
 * @see ra_rmac_clear_status
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_get_status(ra_rmac_port_t port, ra_rmac_status_t* out);

/**
 * @brief Clear bits in MEIS / MMIS0 / MMIS1 / MMIS2.
 *
 * @param[in] port      Port identifier.
 * @param[in] err_mask  Bits to clear in MEIS via MEID.
 * @param[in] mon0_mask Bits to clear in MMIS0 via MMID0.
 * @param[in] mon1_mask Bits to clear in MMIS1 via MMID1.
 * @param[in] mon2_mask Bits to clear in MMIS2 via MMID2.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre Caller has read the status registers via ::ra_rmac_get_status.
 * @post Bits in masks are cleared in their respective status registers.
 * @post Re-reads of the status registers exclude the cleared bits.
 *
 * @note IRQ-safe; the underlying writes are atomic on Cortex-M85.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_clear_status(ra_rmac_port_t port,
                                            uint32_t       err_mask,
                                            uint32_t       mon0_mask,
                                            uint32_t       mon1_mask,
                                            uint32_t       mon2_mask);

/**
 * @brief Read the full statistic counter snapshot (HUM Ch 33.4).
 *
 * @param[in]  port Port identifier.
 * @param[out] out  Destination for the counter snapshot.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port has been brought up via ra_rmac_init.
 * @pre out is a writable pointer.
 * @post Every member in out matches its register at read time.
 * @post Hardware counters keep accumulating; this is a read-only snap.
 *
 * @note Counters are read-only; clearing happens via deinit/init or
 *       a controlled reset of the ETHA mode.
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t ra_rmac_read_stats(ra_rmac_port_t port, ra_rmac_stats_t* out);

/**
 * @brief Attach a per-port IRQ handler for RMAC events.
 *
 * @param[in] port Port identifier.
 * @param[in] cb   Callback (nullptr to detach).
 * @param[in] ctx  Opaque cookie passed back to cb.
 *
 * @return ::ra_err_t Error code.
 *
 * @pre Port previously brought up via ra_rmac_init.
 * @pre Caller is single-threaded with respect to attach/dispatch.
 * @post Subsequent calls to ::ra_rmac_dispatch invoke cb.
 *
 * @see ra_rmac_dispatch
 * @since Version 0.4.0
 */
[[nodiscard]] ra_err_t
ra_rmac_attach_handler(ra_rmac_port_t port, ra_rmac_event_fn_t cb, void* ctx);

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
 * @pre Port previously brought up via ra_rmac_init.
 * @post All four status registers are cleared.
 * @post Callback (if any) has run with the snapshot.
 *
 * @see ra_rmac_attach_handler
 * @since Version 0.4.0
 */
void ra_rmac_dispatch(ra_rmac_port_t port);

#ifdef __cplusplus
}
#endif
