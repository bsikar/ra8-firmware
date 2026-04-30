/**
 * @file ra_ethercat_phy.h
 * @brief EtherCAT PHY abstraction (Clause-22 + ECAT extensions)
 *
 * @details
 * Mirrors the FSP `r_ethercat_phy` API shape. EtherCAT (IEC 61158-3-12)
 * runs on top of standard 100BASE-TX PHYs but disables auto-negotiation
 * and forces 100Mbit / full-duplex with deterministic timing
 * (see ETG.1000.4 "EtherCAT Slave Controller -- PHY Considerations").
 * The driver therefore wraps a plain MDIO bus and adds:
 *
 *  - Forced 100 Mbit / full duplex (BMCR = 0x2100, AN disabled).
 *  - PHY-port enable (some PHYs need a vendor-specific power-up bit).
 *  - Deterministic-link polling that does NOT wait on AN_complete.
 *  - Slave-station identification (16-bit logical, 16-bit physical).
 *
 * Reference: FSP `r_ethercat_phy` driver shape, ETG.1000.4.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_ethercat_phy_addr_limit_t
 * @brief Five-bit MDIO PHY address (0..31) range constraint.
 */
typedef enum : uint8_t {
  k_ra_ethercat_phy_addr_max = 31U,
  k_ra_ethercat_phy_reg_max  = 31U,
  k_ra_ethercat_phy_port_max = 3U, /**< Slave-controller TX/RX port. */
} ra_ethercat_phy_addr_limit_t;

/**
 * @enum ra_ethercat_phy_state_t
 * @brief Driver state.
 */
typedef enum : uint8_t {
  k_ra_ethercat_phy_state_uninit = 0U,
  k_ra_ethercat_phy_state_ready  = 1U,
  k_ra_ethercat_phy_state_active = 2U,
  k_ra_ethercat_phy_state_error  = 3U,
} ra_ethercat_phy_state_t;

/**
 * @struct ra_ethercat_phy_io_t
 * @brief Pluggable MDIO bus interface (same shape as ra_ether_phy).
 */
typedef struct {
  ra_err_t (*read)(void* ctx, uint8_t phy_addr, uint8_t reg_addr, uint16_t* out_data);
  ra_err_t (*write)(void* ctx, uint8_t phy_addr, uint8_t reg_addr, uint16_t data);
  void* ctx;
} ra_ethercat_phy_io_t;

/**
 * @struct ra_ethercat_phy_cfg_t
 * @brief Configuration for `ra_ethercat_phy_open`.
 */
typedef struct {
  ra_ethercat_phy_io_t io;            /**< MDIO bus.                   */
  uint8_t              phy_address;   /**< 5-bit MDIO PHY address.     */
  uint8_t              port;          /**< Slave-controller port (0..3). */
  uint16_t             station_id;    /**< Logical station address.    */
  uint16_t             link_poll_max; /**< Max BMSR polls in init.   */
} ra_ethercat_phy_cfg_t;

/**
 * @struct ra_ethercat_phy_status_t
 * @brief Snapshot of link + driver state.
 */
typedef struct {
  ra_ethercat_phy_state_t state;
  uint8_t                 link_up;
  uint16_t                station_id;
  uint16_t                last_bmsr;
  uint32_t                rx_frames;
  uint32_t                tx_frames;
} ra_ethercat_phy_status_t;

/**
 * @brief Open and reset an EtherCAT-attached PHY.
 *
 * @details Forces 100Mbit / full duplex with auto-negotiation
 *          disabled (BMCR = 0x2100) per ETG.1000.4 PHY guidance.
 *
 * @param[in] cfg Configuration. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                  PHY initialised.
 * @retval k_ra_err_null_ptr        `cfg` or required IO callback NULL.
 * @retval k_ra_err_invalid_arg     `phy_address` > 31 or `port` > 3.
 * @retval k_ra_err_exists          Already opened.
 * @retval k_ra_err_hw_timeout      PHY did not respond.
 *
 * @pre Single-threaded init context.
 * @post Driver in `k_ra_ethercat_phy_state_ready`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_open(const ra_ethercat_phy_cfg_t* cfg);

/**
 * @brief Close the driver.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                  Closed.
 * @retval k_ra_err_invalid_state   Not opened.
 *
 * @pre Driver is open.
 * @post Driver in `k_ra_ethercat_phy_state_uninit`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_close(void);

/**
 * @brief Read a Clause-22 register on the EtherCAT PHY.
 *
 * @param[in]  reg_addr Register index (0..31).
 * @param[out] out_data Receives the 16-bit register value.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Read completed.
 * @retval k_ra_err_null_ptr         `out_data` NULL.
 * @retval k_ra_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post `*out_data` is defined on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data);

/**
 * @brief Write a Clause-22 register on the EtherCAT PHY.
 *
 * @param[in] reg_addr Register index (0..31).
 * @param[in] data     16-bit value.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Write completed.
 * @retval k_ra_err_invalid_arg      `reg_addr` > 31.
 * @retval k_ra_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_mdio_write(uint8_t reg_addr, uint16_t data);

/**
 * @brief Set or update the slave logical station address.
 *
 * @param[in] station_id 16-bit station identifier (ETG.1020 STADR).
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Saved.
 * @retval k_ra_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post `ra_ethercat_phy_status_get` reports the new id.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_station_id_set(uint16_t station_id);

/**
 * @brief Update RX / TX frame counters (called by the MAC ISR).
 *
 * @param[in] rx_delta Frames received since last call.
 * @param[in] tx_delta Frames transmitted since last call.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Counters updated.
 * @retval k_ra_err_not_initialized  Not opened.
 *
 * @pre Driver is open.
 * @post Counters are advanced by the supplied deltas.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_counters_add(uint32_t rx_delta, uint32_t tx_delta);

/**
 * @brief Snapshot driver + link state.
 *
 * @param[out] out Receives the snapshot. Must not be `nullptr`.
 *
 * @return `ra_err_t`.
 * @retval k_ra_ok                   Snapshot copied.
 * @retval k_ra_err_null_ptr         `out` NULL.
 * @retval k_ra_err_not_initialized  Not opened.
 *
 * @pre Pointer references writable memory.
 * @post `*out` reflects current driver state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ethercat_phy_status_get(ra_ethercat_phy_status_t* out);

#ifdef __cplusplus
}
#endif
