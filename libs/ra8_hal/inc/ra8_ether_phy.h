/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ether_phy.h
 * @brief Generic Ethernet PHY abstraction (MDIO Clause-22 + auto-neg)
 * @ingroup grp_hal_net
 *
 * @details
 * Mirrors the FSP `r_ether_phy` API shape (open / close / chipInit /
 * read / write / startAutoNegotiate / linkPartnerAbilityGet /
 * linkStatusGet). The driver is hardware-agnostic: it does not own
 * an MDIO bus -- instead, it borrows one through a `ra8_ether_phy_io_t`
 * function-pointer pair, which can be backed by either the RMAC
 * MDC/MDIO state machine, the ETHER MIIM block, or a fake bus
 * for unit tests.
 *
 * Reference: FSP `r_ether_phy` driver shape, IEEE 802.3 Clause 22.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_ether_phy_speed_t
 * @brief Negotiated PHY link speed / duplex.
 */
typedef enum : uint8_t {
  k_ra8_ether_phy_speed_no_link = 0U, /**< RA8 ether PHY speed no link. */
  k_ra8_ether_phy_speed_10h     = 1U, /**< RA8 ether PHY speed 10h.     */
  k_ra8_ether_phy_speed_10f     = 2U, /**< RA8 ether PHY speed 10f.     */
  k_ra8_ether_phy_speed_100h    = 3U, /**< RA8 ether PHY speed 100h.    */
  k_ra8_ether_phy_speed_100f    = 4U, /**< RA8 ether PHY speed 100f.    */
  k_ra8_ether_phy_speed_1000h   = 5U, /**< RA8 ether PHY speed 1000h.   */
  k_ra8_ether_phy_speed_1000f   = 6U, /**< RA8 ether PHY speed 1000f.   */
} ra8_ether_phy_speed_t;

/**
 * @enum ra8_ether_phy_mii_t
 * @brief Media-independent interface type seen on the MAC side.
 */
typedef enum : uint8_t {
  k_ra8_ether_phy_mii   = 0U, /**< RA8 ether PHY mii.   */
  k_ra8_ether_phy_rmii  = 1U, /**< RA8 ether PHY rmii.  */
  k_ra8_ether_phy_gmii  = 2U, /**< RA8 ether PHY gmii.  */
  k_ra8_ether_phy_rgmii = 3U, /**< RA8 ether PHY rgmii. */
} ra8_ether_phy_mii_t;

/**
 * @enum ra8_ether_phy_clause22_reg_t
 * @brief Standard IEEE 802.3 Clause-22 PHY register addresses.
 */
typedef enum : uint8_t {
  k_ra8_ether_phy_reg_control    = 0U, /**< BMCR.                   */
  k_ra8_ether_phy_reg_status     = 1U, /**< BMSR.                   */
  k_ra8_ether_phy_reg_id_low     = 2U, /**< PHY ID #1.              */
  k_ra8_ether_phy_reg_id_high    = 3U, /**< PHY ID #2.              */
  k_ra8_ether_phy_reg_an_advert  = 4U, /**< Auto-Neg Advertisement. */
  k_ra8_ether_phy_reg_an_partner = 5U, /**< Link Partner Ability.   */
  k_ra8_ether_phy_reg_an_expand  = 6U, /**< Auto-Neg Expansion.     */
} ra8_ether_phy_clause22_reg_t;

/**
 * @enum ra8_ether_phy_addr_limit_t
 * @brief Five-bit MDIO PHY address (0..31).
 */
typedef enum : uint8_t {
  k_ra8_ether_phy_addr_max = 31U, /**< RA8 ether PHY address maximum. */
} ra8_ether_phy_addr_limit_t;

/**
 * @struct ra8_ether_phy_io_t
 * @brief Pluggable MDIO bus interface.
 */
typedef struct {
  /**
   * @brief Read a 16-bit Clause-22 register.
   *
   * @param[in]  ctx       User context handed back unchanged.
   * @param[in]  phy_addr  PHY address (0..31).
   * @param[in]  reg_addr  Register index (0..31).
   * @param[out] out_data  Receives the 16-bit register value.
   */
  ra8_err_t (*read)(void* ctx, uint8_t phy_addr, uint8_t reg_addr, uint16_t* out_data);
  /**
   * @brief Write a 16-bit Clause-22 register.
   *
   * @param[in] ctx      User context handed back unchanged.
   * @param[in] phy_addr PHY address (0..31).
   * @param[in] reg_addr Register index (0..31).
   * @param[in] data     16-bit register value.
   */
  ra8_err_t (*write)(void* ctx, uint8_t phy_addr, uint8_t reg_addr, uint16_t data);
  /** Opaque context pointer forwarded to `read` / `write`. */
  void* ctx;
} ra8_ether_phy_io_t;

/**
 * @struct ra8_ether_phy_link_t
 * @brief Link-status snapshot.
 */
typedef struct {
  uint8_t               link_up;       /**< 1 if link is up.        */
  uint8_t               auto_neg_done; /**< 1 if AN completed.      */
  ra8_ether_phy_speed_t speed;         /**< Speed / duplex (if up). */
  uint16_t              bmsr;          /**< Last raw BMSR snapshot. */
} ra8_ether_phy_link_t;

/**
 * @struct ra8_ether_phy_cfg_t
 * @brief Configuration descriptor for `ra8_ether_phy_open`.
 */
typedef struct {
  ra8_ether_phy_io_t  io;            /**< Pluggable MDIO bus.     */
  uint8_t             phy_address;   /**< 5-bit MDIO PHY address. */
  ra8_ether_phy_mii_t mii_type;      /**< Interface type.         */
  uint16_t            reset_wait_us; /**< Wait after reset.       */
} ra8_ether_phy_cfg_t;

/**
 * @brief Open and reset a PHY.
 *
 * @details Issues a Clause-22 BMCR.RESET to the PHY at `cfg->phy_address`
 *          and waits until the bit self-clears (or `reset_wait_us`
 *          elapses, whichever is sooner). Mirrors `R_ETHER_PHY_Open`.
 *
 * @param[in] cfg Configuration. Must not be `nullptr`.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                  PHY responded to reset.
 * @retval k_ra8_err_null_ptr        `cfg`, or any required IO callback NULL.
 * @retval k_ra8_err_invalid_arg     `phy_address` > 31.
 * @retval k_ra8_err_exists          Already opened.
 * @retval k_ra8_err_hw_timeout      Reset bit never cleared.
 *
 * @pre Single-threaded init context.
 * @post Driver is in the open state.
 * @post `ra8_ether_phy_close` will succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_open(const ra8_ether_phy_cfg_t* cfg);

/**
 * @brief Close the driver.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                  Closed.
 * @retval k_ra8_err_invalid_state   Not opened.
 *
 * @pre Driver is open.
 * @post Subsequent ops return `k_ra8_err_not_initialized`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_close(void);

/**
 * @brief Read a Clause-22 PHY register.
 *
 * @param[in]  reg_addr Register index (0..31).
 * @param[out] out_data Receives the value.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Read completed.
 * @retval k_ra8_err_null_ptr         `out_data` was NULL.
 * @retval k_ra8_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre `ra8_ether_phy_open` succeeded.
 * @post `*out_data` is defined on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data);

/**
 * @brief Write a Clause-22 PHY register.
 *
 * @param[in] reg_addr Register index (0..31).
 * @param[in] data     16-bit value.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Write completed.
 * @retval k_ra8_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre `ra8_ether_phy_open` succeeded.
 * @post Register at `reg_addr` holds `data` (driver perspective).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_mdio_write(uint8_t reg_addr, uint16_t data);

/**
 * @brief Kick off auto-negotiation by re-arming BMCR.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Auto-neg restarted.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre `ra8_ether_phy_open` succeeded.
 * @post BMCR.AUTONEG_RESTART was written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_auto_negotiate_start(void);

/**
 * @brief Poll the BMSR for auto-negotiation completion + link-up.
 *
 * @param[out] out Receives the link snapshot. Must not be `nullptr`.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Snapshot copied.
 * @retval k_ra8_err_null_ptr         `out` was NULL.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Pointer references writable memory.
 * @post `*out` reflects the BMSR read at call time.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ether_phy_link_status_get(ra8_ether_phy_link_t* out);

#ifdef __cplusplus
}
#endif
