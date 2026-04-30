/**
 * @file ra_ethercat_phy.c
 * @brief EtherCAT PHY driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * EtherCAT slave-side PHY driver. Differs from the generic
 * `ra_ether_phy` in three ways:
 *  1. AN is disabled and the link is forced to 100/full at open time.
 *  2. The driver tracks a logical station id (ETG.1020 STADR).
 *  3. Link readiness is decided by BMSR.LINK alone, not by AN_complete,
 *     because EtherCAT requires deterministic link bring-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_ethercat_phy.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ECPHY";

typedef enum : uint8_t {
  k_ra_ethercat_phy_reg_control = 0U,
  k_ra_ethercat_phy_reg_status  = 1U,
} ra_ethercat_phy_reg_t;

typedef enum : uint16_t {
  k_ra_ethercat_phy_bmcr_force_100full = 0x2100U,
  k_ra_ethercat_phy_bmsr_link_up       = 0x0004U,
  k_ra_ethercat_phy_link_poll_default  = 64U,
} ra_ethercat_phy_bits_t;

typedef struct {
  bool                    opened;
  uint8_t                 phy_address;
  uint8_t                 port;
  uint16_t                station_id;
  ra_ethercat_phy_io_t    io;
  ra_ethercat_phy_state_t state;
  uint8_t                 link_up;
  uint16_t                last_bmsr;
  uint32_t                rx_frames;
  uint32_t                tx_frames;
} ra_ethercat_phy_internal_t;

static ra_ethercat_phy_internal_t s_state = {};

ra_err_t ra_ethercat_phy_open(const ra_ethercat_phy_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->io.read, s_tag, "io.read required");
  RA_CHECK_NULL_PTR(cfg->io.write, s_tag, "io.write required");
  if (cfg->phy_address > k_ra_ethercat_phy_addr_max) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->port > k_ra_ethercat_phy_port_max) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened      = true;
  s_state.phy_address = cfg->phy_address;
  s_state.port        = cfg->port;
  s_state.station_id  = cfg->station_id;
  s_state.io          = cfg->io;
  s_state.state       = k_ra_ethercat_phy_state_uninit;
  s_state.link_up     = 0U;
  s_state.last_bmsr   = 0U;
  s_state.rx_frames   = 0U;
  s_state.tx_frames   = 0U;

  /* Force 100/full + AN disabled. */
  ra_err_t err = s_state.io.write(s_state.io.ctx,
                                  s_state.phy_address,
                                  k_ra_ethercat_phy_reg_control,
                                  k_ra_ethercat_phy_bmcr_force_100full);
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }

  /* Poll BMSR.LINK. */
  const uint16_t poll_max =
    (cfg->link_poll_max == 0U) ? k_ra_ethercat_phy_link_poll_default : cfg->link_poll_max;
  for (uint16_t i = 0U; i < poll_max; ++i) {
    uint16_t bmsr = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_ethercat_phy_reg_status, &bmsr);
    if (err != k_ra_ok) {
      s_state.state  = k_ra_ethercat_phy_state_error;
      s_state.opened = false;
      return err;
    }
    s_state.last_bmsr = bmsr;
    if ((bmsr & k_ra_ethercat_phy_bmsr_link_up) != 0U) {
      s_state.link_up = 1U;
      s_state.state   = k_ra_ethercat_phy_state_ready;
      ra_log_info_val(s_tag, "ecat link up port", (uint32_t)cfg->port);
      return k_ra_ok;
    }
  }
  /* Open succeeded but link is still down -- caller can retry via status. */
  s_state.state = k_ra_ethercat_phy_state_ready;
  return k_ra_ok;
}

ra_err_t ra_ethercat_phy_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  s_state.state  = k_ra_ethercat_phy_state_uninit;
  return k_ra_ok;
}

ra_err_t ra_ethercat_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_ethercat_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.read(s_state.io.ctx, s_state.phy_address, reg_addr, out_data);
}

ra_err_t ra_ethercat_phy_mdio_write(uint8_t reg_addr, uint16_t data)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_ethercat_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, reg_addr, data);
}

ra_err_t ra_ethercat_phy_station_id_set(uint16_t station_id)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.station_id = station_id;
  return k_ra_ok;
}

ra_err_t ra_ethercat_phy_counters_add(uint32_t rx_delta, uint32_t tx_delta)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.rx_frames = s_state.rx_frames + rx_delta;
  s_state.tx_frames = s_state.tx_frames + tx_delta;
  s_state.state     = k_ra_ethercat_phy_state_active;
  return k_ra_ok;
}

ra_err_t ra_ethercat_phy_status_get(ra_ethercat_phy_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  out->state      = s_state.state;
  out->link_up    = s_state.link_up;
  out->station_id = s_state.station_id;
  out->last_bmsr  = s_state.last_bmsr;
  out->rx_frames  = s_state.rx_frames;
  out->tx_frames  = s_state.tx_frames;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
