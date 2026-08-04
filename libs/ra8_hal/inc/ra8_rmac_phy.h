/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rmac_phy.h
 * @brief Reduced-MAC (RMAC) PHY driver -- off-chip PHY for the GMAC-FPI
 * @ingroup grp_hal_net
 *
 * @details
 * Mirrors the FSP `r_rmac_phy` API shape. The RMAC peripheral sits
 * inside the RA8D2 (HUM Ch 33) and talks to an off-chip PHY over
 * MII / RMII / GMII / RGMII. This driver is the per-PHY companion
 * to `ra8_rmac.c` and handles:
 *
 *  - PHY-LSI identification table (KSZ8041 / KSZ8091 / DP83620 /
 *    ICS1894 / GPY111 / VSC8541, plus a CUSTOM slot).
 *  - Clause-22 register access through the same pluggable bus
 *    interface used by `ra8_ether_phy`.
 *  - Auto-negotiation start / poll / read partner ability with
 *    1000Mbit support (Clause-22 register 9 controller/peripheral, per IEEE 802.3 spec).
 *  - RGMII rx/tx clock-skew tuning (vendor-specific PHY register).
 *  - Link-status and per-PHY callback hooks for the MAC ISR.
 *
 * Reference: FSP `r_rmac_phy` driver shape, IEEE 802.3 Clause 22 /
 *            Clause 28A (auto-neg), IEEE 802.3-2018 Annex 28B.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_rmac_phy_lsi_t
 * @brief Supported PHY-LSI identifiers.
 */
typedef enum : uint8_t {
  k_ra8_rmac_phy_lsi_default    = 0U, /**< RA8 rmac PHY lsi default.    */
  k_ra8_rmac_phy_lsi_ksz8091rnb = 1U, /**< RA8 rmac PHY lsi ksz8091rnb. */
  k_ra8_rmac_phy_lsi_ksz8041    = 2U, /**< RA8 rmac PHY lsi ksz8041.    */
  k_ra8_rmac_phy_lsi_dp83620    = 3U, /**< RA8 rmac PHY lsi dp83620.    */
  k_ra8_rmac_phy_lsi_ics1894    = 4U, /**< RA8 rmac PHY lsi ics1894.    */
  k_ra8_rmac_phy_lsi_gpy111     = 5U, /**< RA8 rmac PHY lsi gpy111.     */
  k_ra8_rmac_phy_lsi_vsc8541    = 6U, /**< RA8 rmac PHY lsi vsc8541.    */
  k_ra8_rmac_phy_lsi_custom     = 7U, /**< RA8 rmac PHY lsi custom.     */
  k_ra8_rmac_phy_lsi_count      = 8U, /**< RA8 rmac PHY lsi count.      */
} ra8_rmac_phy_lsi_t;

/**
 * @enum ra8_rmac_phy_speed_t
 * @brief Negotiated link speed.
 */
typedef enum : uint8_t {
  k_ra8_rmac_phy_speed_no_link = 0U, /**< RA8 rmac PHY speed no link. */
  k_ra8_rmac_phy_speed_10h     = 1U, /**< RA8 rmac PHY speed 10h.     */
  k_ra8_rmac_phy_speed_10f     = 2U, /**< RA8 rmac PHY speed 10f.     */
  k_ra8_rmac_phy_speed_100h    = 3U, /**< RA8 rmac PHY speed 100h.    */
  k_ra8_rmac_phy_speed_100f    = 4U, /**< RA8 rmac PHY speed 100f.    */
  k_ra8_rmac_phy_speed_1000h   = 5U, /**< RA8 rmac PHY speed 1000h.   */
  k_ra8_rmac_phy_speed_1000f   = 6U, /**< RA8 rmac PHY speed 1000f.   */
} ra8_rmac_phy_speed_t;

/**
 * @enum ra8_rmac_phy_addr_limit_t
 * @brief MDIO address constraints.
 */
typedef enum : uint8_t {
  k_ra8_rmac_phy_addr_max = 31U, /**< RA8 rmac PHY address maximum.  */
  k_ra8_rmac_phy_reg_max  = 31U, /**< RA8 rmac PHY register maximum. */
} ra8_rmac_phy_addr_limit_t;

/**
 * @struct ra8_rmac_phy_io_t
 * @brief Pluggable MDIO bus.
 */
typedef struct {
  ra8_err_t (*read)(void*     ctx,
                    uint8_t   phy_addr,
                    uint8_t   reg_addr,
                    uint16_t* out_data);                                            /**< Read.  */
  ra8_err_t (*write)(void* ctx, uint8_t phy_addr, uint8_t reg_addr, uint16_t data); /**< Write. */
  void* ctx;                                                                        /**< Ctx.   */
} ra8_rmac_phy_io_t;

/**
 * @struct ra8_rmac_phy_link_t
 * @brief Snapshot of resolved link parameters.
 */
typedef struct {
  uint8_t              link_up;         /**< Link up.         */
  uint8_t              auto_neg_done;   /**< Auto neg done.   */
  ra8_rmac_phy_speed_t speed;           /**< Speed.           */
  uint16_t             bmsr;            /**< Bmsr.            */
  uint16_t             partner_ability; /**< Partner ability. */
} ra8_rmac_phy_link_t;

/**
 * @struct ra8_rmac_phy_cfg_t
 * @brief Configuration descriptor.
 */
typedef struct {
  ra8_rmac_phy_io_t  io;              /**< Io.                         */
  ra8_rmac_phy_lsi_t lsi_type;        /**< Lsi type.                   */
  uint8_t            phy_address;     /**< PHY address.                */
  uint16_t           reset_poll_max;  /**< Reset poll maximum.         */
  uint16_t           local_advertise; /**< Clause-22 reg 4 advertised. */
  uint16_t           gbit_advertise;  /**< Clause-22 reg 9 (1000T).    */
} ra8_rmac_phy_cfg_t;

/**
 * @brief Open the PHY and run a soft-reset sequence.
 *
 * @details Mirrors `R_RMAC_PHY_Open`. Issues BMCR.RESET, polls until
 *          self-clear, then writes the local advertisement register
 *          and (if `gbit_advertise != 0`) the 1000BASE-T control
 *          register so the next auto-neg round picks them up.
 *
 * @param[in] cfg Configuration. Must not be `nullptr`.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                  PHY initialized.
 * @retval k_ra8_err_null_ptr        `cfg` or required IO callback NULL.
 * @retval k_ra8_err_invalid_arg     `phy_address` > 31 or `lsi_type` invalid.
 * @retval k_ra8_err_exists          Already opened.
 * @retval k_ra8_err_hw_timeout      BMCR.RESET never cleared.
 *
 * @pre Single-threaded init context.
 * @post Driver is in the open state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_open(const ra8_rmac_phy_cfg_t* cfg);

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
[[nodiscard]] ra8_err_t ra8_rmac_phy_close(void);

/**
 * @brief Read a Clause-22 register on the off-chip PHY.
 *
 * @param[in]  reg_addr Register index (0..31).
 * @param[out] out_data Receives the 16-bit register value.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Read completed.
 * @retval k_ra8_err_null_ptr         `out_data` NULL.
 * @retval k_ra8_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post `*out_data` is defined on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data);

/**
 * @brief Write a Clause-22 register on the off-chip PHY.
 *
 * @param[in] reg_addr Register index (0..31).
 * @param[in] data     16-bit value.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Write completed.
 * @retval k_ra8_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_mdio_write(uint8_t reg_addr, uint16_t data);

/**
 * @brief Restart auto-negotiation on the PHY.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   AN restarted.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post BMCR.AN_ENABLE | AN_RESTART was written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_auto_negotiate_start(void);

/**
 * @brief Read BMSR + partner ability and resolve speed/duplex.
 *
 * @param[out] out Receives the snapshot. Must not be `nullptr`.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Snapshot copied.
 * @retval k_ra8_err_null_ptr         `out` NULL.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Pointer references writable memory.
 * @post `*out` reflects the BMSR / LPA / 1000T_STATUS read at call time.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_link_status_get(ra8_rmac_phy_link_t* out);

/**
 * @brief Get the currently-bound PHY-LSI identifier.
 *
 * @param[out] out Receives the LSI id. Must not be `nullptr`.
 *
 * @return `ra8_err_t`.
 * @retval k_ra8_ok                   Returned.
 * @retval k_ra8_err_null_ptr         `out` NULL.
 * @retval k_ra8_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post `*out` matches `cfg->lsi_type` from the original `ra8_rmac_phy_open`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rmac_phy_lsi_get(ra8_rmac_phy_lsi_t* out);

#ifdef __cplusplus
}
#endif
