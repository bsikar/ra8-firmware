/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_eth_internal.h
 * @brief Cross-TU shared surface for the ra8_eth driver split.
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The ra8_eth driver is split across two translation units to stay
 * under the per-file line-count cap: ra8_eth.c holds the ESWM block,
 * the GWCA-backed NIC frame path (open / close / write / read /
 * get_stats), and the test-inject hook; ra8_eth_link.c holds the PHY
 * link-status poller plus the MAC speed/duplex resync that realigns
 * MPIC with the PHY's auto-negotiation result. This src/-local header
 * carries the handful of symbols both TUs share: the singleton NIC
 * runtime state, the one-shot MAC-resync latch, and the
 * channel-to-port mapping predicate. It is NOT part of the public ABI.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_eth.h"
#include "ra8_rmac.h"

/**
 * @enum ra8_eth_phy_t
 * @brief PHY-side MIIM constants shared by both ra8_eth TUs: the ra8_eth.c
 *        open path checks PHY readiness and ra8_eth_link.c polls link status.
 */
typedef enum : uint16_t {
  k_ra8_eth_phy_addr_default   = 0U,      /**< EK-RA8D2 PHY MDC address.       */
  k_ra8_eth_phy_reg_bmcr       = 0U,      /**< BMCR (basic mode control).      */
  k_ra8_eth_phy_reg_bmsr       = 1U,      /**< BMSR (basic mode status).       */
  k_ra8_eth_phy_reg_anlpar     = 5U,      /**< ANLPAR (10/100 link partner).   */
  k_ra8_eth_phy_reg_gbsr       = 10U,     /**< GBSR (1G link-partner ability). */
  k_ra8_eth_phy_bmsr_link_up   = 0x0004U, /**< BMSR.LINK_STATUS bit 2.         */
  k_ra8_eth_phy_bmsr_an_done   = 0x0020U, /**< BMSR.AUTONEG_COMPLETE bit 5.    */
  k_ra8_eth_phy_bmcr_speed100  = 0x2000U, /**< BMCR.SPEED_SELECT bit 13.       */
  k_ra8_eth_phy_bmcr_speed1000 = 0x0040U, /**< BMCR.SPEED_MS bit 6 (1Gb).      */
  k_ra8_eth_phy_bmcr_duplex    = 0x0100U, /**< BMCR.DUPLEX_MODE bit 8.         */
  k_ra8_eth_phy_anlpar_10h     = 0x0020U, /**< ANLPAR bit 5: 10BASE-T.         */
  k_ra8_eth_phy_anlpar_10f     = 0x0040U, /**< ANLPAR bit 6: 10BASE-T FD.      */
  k_ra8_eth_phy_anlpar_100h    = 0x0080U, /**< ANLPAR bit 7: 100BASE-TX.       */
  k_ra8_eth_phy_anlpar_100f    = 0x0100U, /**< ANLPAR bit 8: 100BASE-TX FD.    */
  k_ra8_eth_phy_gbsr_1000h     = 0x0400U, /**< GBSR bit 10: 1000BASE-T HD.     */
  k_ra8_eth_phy_gbsr_1000f     = 0x0800U, /**< GBSR bit 11: 1000BASE-T FD.     */
  k_ra8_eth_phy_speed_10       = 10U,     /**< 10 Mbps.                        */
  k_ra8_eth_phy_speed_100      = 100U,    /**< 100 Mbps.                       */
  k_ra8_eth_phy_speed_1000     = 1000U,   /**< 1 Gbps.                         */
} ra8_eth_phy_t;

/**
 * @var g_ra8_eth_phy_bmsr_after_wait
 * @brief BMSR snapshot AFTER the auto-neg wait. Defined once in
 *        ra8_eth_link.c; referenced from ra8_eth.c via this declaration.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
extern volatile uint16_t g_ra8_eth_phy_bmsr_after_wait;

/**
 * @struct ra8_eth_state_t
 * @brief Driver-private NIC state.
 *
 * @details
 * Holds the cached configuration, the cumulative software counters,
 * and the "open" flag. The per-queue head/tail cursors live inside
 * the GWCA default-state block (managed by the GWCA default-state
 * API).
 */
typedef struct {
  uint8_t         opened; /**< 1 once ::ra8_eth_open succeeds. */
  ra8_eth_cfg_t   cfg;    /**< Captured configuration.         */
  ra8_eth_stats_t stats;  /**< Cumulative counters.            */
} ra8_eth_state_t;

/**
 * @var s_eth_state
 * @brief Singleton NIC runtime state.
 *
 * @details
 * Defined once in ra8_eth.c and referenced from the split-out
 * link-status TU (ra8_eth_link.c) via this declaration. Holds the
 * cached configuration, the cumulative software counters, and the
 * "open" flag.
 *
 * @warning Not safe to mutate outside the ra8_eth driver path.
 * @since 0.1.0
 */
extern ra8_eth_state_t s_eth_state;

/**
 * @var s_eth_mac_speed_resynced
 * @brief Latch -- true once ::ra8_eth_link_status has re-programmed
 *        MPIC.LSC / MPIC.PIPP to match the PHY's negotiated link.
 *
 * @details
 * Reset to false in ::ra8_eth_open / ::ra8_eth_close. The MAC speed
 * resync only needs to fire once per link bring-up; further calls to
 * ::ra8_eth_link_status skip the MPIC write so they remain read-only
 * status pollers. Defined once in ra8_eth.c and updated from
 * ra8_eth_link.c via this declaration.
 *
 * @warning File-scope state, not thread-safe.
 * @since 0.1.0
 */
extern bool s_eth_mac_speed_resynced;

/**
 * @brief Map a logical channel index to an RMAC port identifier.
 *
 * @details
 * Shared mapping used by both ra8_eth translation units: the NIC
 * frame path (ra8_eth.c) selects the RMAC port for teardown, and the
 * link-status TU (ra8_eth_link.c) selects the port whose PHY it
 * polls. Pure; no side effects.
 *
 * @param[in] channel Channel id from ::ra8_eth_cfg_t (0 or 1).
 * @return Matching ::ra8_rmac_port_t.
 *
 * @retval k_ra8_rmac_port_0 channel == 0.
 * @retval k_ra8_rmac_port_1 channel != 0.
 * @pre channel was already range-checked by the caller.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post No global state is mutated.
 * @note Thread safety: pure function, safe from any context.
 * @since 0.1.0
 */
RA8_PRIV ra8_rmac_port_t ra8_eth_channel_to_port(uint8_t channel);

#ifdef __cplusplus
}
#endif
