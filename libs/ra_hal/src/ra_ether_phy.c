/**
 * @file ra_ether_phy.c
 * @brief Generic Ethernet PHY abstraction (MDIO Clause-22)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hardware-agnostic PHY driver. The MDIO bus itself is plugged in
 * via `ra_ether_phy_io_t` callbacks so the same code can drive the
 * RA8D2 RMAC MIIM block, the legacy ETHER MIIM, an external
 * SMI-over-GPIO bit-banger, or a host simulator. State is kept in
 * a single static control block; only one PHY is tracked at a time
 * because the underlying MAC ports each use their own driver
 * instance (ra_ethercat_phy / ra_rmac_phy).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_ether_phy.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Module log tag. */
static const char* s_tag = "EPHY";

typedef enum : uint8_t {
  k_ra_ether_phy_reg_max        = 31U,
  k_ra_ether_phy_reset_poll_max = 32U,
} ra_ether_phy_priv_t;

typedef enum : uint16_t {
  k_ra_ether_phy_bmcr_reset       = 0x8000U,
  k_ra_ether_phy_bmcr_an_enable   = 0x1000U,
  k_ra_ether_phy_bmcr_an_restart  = 0x0200U,
  k_ra_ether_phy_bmsr_link_up     = 0x0004U,
  k_ra_ether_phy_bmsr_an_complete = 0x0020U,
  k_ra_ether_phy_anar_100full     = 0x0100U,
  k_ra_ether_phy_anar_100half     = 0x0080U,
  k_ra_ether_phy_anar_10full      = 0x0040U,
  k_ra_ether_phy_anar_10half      = 0x0020U,
} ra_ether_phy_bits_t;

typedef struct {
  bool               opened;
  uint8_t            phy_address;
  ra_ether_phy_mii_t mii_type;
  uint16_t           reset_wait_us;
  ra_ether_phy_io_t  io;
  uint16_t           last_bmsr;
} ra_ether_phy_internal_t;

static ra_ether_phy_internal_t s_state = {};

/* Implementation of ra_ether_phy_open (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_open(const ra_ether_phy_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->io.read, s_tag, "io.read required");
  RA_CHECK_NULL_PTR(cfg->io.write, s_tag, "io.write required");
  if (cfg->phy_address > k_ra_ether_phy_addr_max) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened        = true;
  s_state.phy_address   = cfg->phy_address;
  s_state.mii_type      = cfg->mii_type;
  s_state.reset_wait_us = cfg->reset_wait_us;
  s_state.io            = cfg->io;
  s_state.last_bmsr     = 0U;

  /* Issue BMCR reset and poll for self-clear. */
  ra_err_t err = s_state.io.write(s_state.io.ctx,
                                  s_state.phy_address,
                                  k_ra_ether_phy_reg_control,
                                  k_ra_ether_phy_bmcr_reset);
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }
  for (uint8_t i = 0U; i < k_ra_ether_phy_reset_poll_max; ++i) {
    uint16_t reg = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_ether_phy_reg_control, &reg);
    if (err != k_ra_ok) {
      s_state.opened = false;
      return err;
    }
    if ((reg & k_ra_ether_phy_bmcr_reset) == 0U) {
      ra_log_info_val(s_tag, "phy ready addr", (uint32_t)cfg->phy_address);
      return k_ra_ok;
    }
  }
  s_state.opened = false;
  return k_ra_err_hw_timeout;
}

/* Implementation of ra_ether_phy_close (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  return k_ra_ok;
}

/* Implementation of ra_ether_phy_mdio_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_ether_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.read(s_state.io.ctx, s_state.phy_address, reg_addr, out_data);
}

/* Implementation of ra_ether_phy_mdio_write (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_mdio_write(uint8_t reg_addr, uint16_t data)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_ether_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, reg_addr, data);
}

/* Implementation of ra_ether_phy_auto_negotiate_start (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_auto_negotiate_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  /* Re-arm AN: enable + restart. */
  const uint16_t bmcr = (uint16_t)(k_ra_ether_phy_bmcr_an_enable | k_ra_ether_phy_bmcr_an_restart);
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, k_ra_ether_phy_reg_control, bmcr);
}

/* Implementation of ra_ether_phy_link_status_get (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_ether_phy_link_status_get(ra_ether_phy_link_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  uint16_t bmsr = 0U;
  ra_err_t err =
    s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_ether_phy_reg_status, &bmsr);
  if (err != k_ra_ok) {
    return err;
  }
  s_state.last_bmsr  = bmsr;
  out->bmsr          = bmsr;
  out->link_up       = (uint8_t)(((bmsr & k_ra_ether_phy_bmsr_link_up) != 0U) ? 1U : 0U);
  out->auto_neg_done = (uint8_t)(((bmsr & k_ra_ether_phy_bmsr_an_complete) != 0U) ? 1U : 0U);
  out->speed         = k_ra_ether_phy_speed_no_link;
  if ((out->link_up != 0U) && (out->auto_neg_done != 0U)) {
    /* Inspect partner ability for resolved speed/duplex. */
    uint16_t lpa = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_ether_phy_reg_an_partner, &lpa);
    if (err == k_ra_ok) {
      if ((lpa & k_ra_ether_phy_anar_100full) != 0U) {
        out->speed = k_ra_ether_phy_speed_100f;
      } else if ((lpa & k_ra_ether_phy_anar_100half) != 0U) {
        out->speed = k_ra_ether_phy_speed_100h;
      } else if ((lpa & k_ra_ether_phy_anar_10full) != 0U) {
        out->speed = k_ra_ether_phy_speed_10f;
      } else if ((lpa & k_ra_ether_phy_anar_10half) != 0U) {
        out->speed = k_ra_ether_phy_speed_10h;
      }
    }
  }
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
