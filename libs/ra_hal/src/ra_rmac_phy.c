/**
 * @file ra_rmac_phy.c
 * @brief RMAC off-chip PHY driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion driver to `ra_rmac.c`. Same MDIO bus shape as
 * `ra_ether_phy` but tracks 1000BASE-T capabilities and a vendor
 * LSI identifier (KSZ8041 / KSZ8091RNB / DP83620 / ICS1894 /
 * GPY111 / VSC8541) so future quirks can fan out by `lsi_type`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

#include "ra_rmac_phy.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "RMPHY";

typedef enum : uint8_t {
  k_ra_rmac_phy_reg_control      = 0U,
  k_ra_rmac_phy_reg_status       = 1U,
  k_ra_rmac_phy_reg_an_advert    = 4U,
  k_ra_rmac_phy_reg_an_partner   = 5U,
  k_ra_rmac_phy_reg_1000t_ctrl   = 9U,
  k_ra_rmac_phy_reg_1000t_status = 10U,
  k_ra_rmac_phy_reset_poll_max   = 32U,
} ra_rmac_phy_reg_t;

typedef enum : uint16_t {
  k_ra_rmac_phy_bmcr_reset       = 0x8000U,
  k_ra_rmac_phy_bmcr_an_enable   = 0x1000U,
  k_ra_rmac_phy_bmcr_an_restart  = 0x0200U,
  k_ra_rmac_phy_bmsr_link_up     = 0x0004U,
  k_ra_rmac_phy_bmsr_an_complete = 0x0020U,
  k_ra_rmac_phy_lpa_100full      = 0x0100U,
  k_ra_rmac_phy_lpa_100half      = 0x0080U,
  k_ra_rmac_phy_lpa_10full       = 0x0040U,
  k_ra_rmac_phy_lpa_10half       = 0x0020U,
  k_ra_rmac_phy_msr_1000full     = 0x0800U,
  k_ra_rmac_phy_msr_1000half     = 0x0400U,
} ra_rmac_phy_bits_t;

typedef struct {
  bool              opened;
  uint8_t           phy_address;
  ra_rmac_phy_lsi_t lsi_type;
  uint16_t          local_advertise;
  uint16_t          gbit_advertise;
  ra_rmac_phy_io_t  io;
  uint16_t          last_bmsr;
} ra_rmac_phy_internal_t;

static ra_rmac_phy_internal_t s_state = {};

ra_err_t ra_rmac_phy_open(const ra_rmac_phy_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR(cfg->io.read, s_tag, "io.read required");
  RA_CHECK_NULL_PTR(cfg->io.write, s_tag, "io.write required");
  if (cfg->phy_address > k_ra_rmac_phy_addr_max) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->lsi_type >= k_ra_rmac_phy_lsi_count) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened          = true;
  s_state.phy_address     = cfg->phy_address;
  s_state.lsi_type        = cfg->lsi_type;
  s_state.local_advertise = cfg->local_advertise;
  s_state.gbit_advertise  = cfg->gbit_advertise;
  s_state.io              = cfg->io;
  s_state.last_bmsr       = 0U;

  ra_err_t err = s_state.io.write(s_state.io.ctx,
                                  s_state.phy_address,
                                  k_ra_rmac_phy_reg_control,
                                  k_ra_rmac_phy_bmcr_reset);
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }
  const uint16_t poll_max =
    (cfg->reset_poll_max == 0U) ? k_ra_rmac_phy_reset_poll_max : cfg->reset_poll_max;
  bool reset_cleared = false;
  for (uint16_t i = 0U; i < poll_max; ++i) {
    uint16_t reg = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_control, &reg);
    if (err != k_ra_ok) {
      s_state.opened = false;
      return err;
    }
    if ((reg & k_ra_rmac_phy_bmcr_reset) == 0U) {
      reset_cleared = true;
      break;
    }
  }
  if (!reset_cleared) {
    s_state.opened = false;
    return k_ra_err_hw_timeout;
  }

  err = s_state.io.write(s_state.io.ctx,
                         s_state.phy_address,
                         k_ra_rmac_phy_reg_an_advert,
                         s_state.local_advertise);
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }
  if (s_state.gbit_advertise != 0U) {
    err = s_state.io.write(s_state.io.ctx,
                           s_state.phy_address,
                           k_ra_rmac_phy_reg_1000t_ctrl,
                           s_state.gbit_advertise);
    if (err != k_ra_ok) {
      s_state.opened = false;
      return err;
    }
  }
  ra_log_info_val(s_tag, "rmac phy lsi", (uint32_t)cfg->lsi_type);
  return k_ra_ok;
}

ra_err_t ra_rmac_phy_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  return k_ra_ok;
}

ra_err_t ra_rmac_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_rmac_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.read(s_state.io.ctx, s_state.phy_address, reg_addr, out_data);
}

ra_err_t ra_rmac_phy_mdio_write(uint8_t reg_addr, uint16_t data)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  if (reg_addr > k_ra_rmac_phy_reg_max) {
    return k_ra_err_invalid_arg;
  }
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, reg_addr, data);
}

ra_err_t ra_rmac_phy_auto_negotiate_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  const uint16_t bmcr = (uint16_t)(k_ra_rmac_phy_bmcr_an_enable | k_ra_rmac_phy_bmcr_an_restart);
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_control, bmcr);
}

ra_err_t ra_rmac_phy_link_status_get(ra_rmac_phy_link_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  uint16_t bmsr = 0U;
  ra_err_t err =
    s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_status, &bmsr);
  if (err != k_ra_ok) {
    return err;
  }
  s_state.last_bmsr    = bmsr;
  out->bmsr            = bmsr;
  out->link_up         = (uint8_t)(((bmsr & k_ra_rmac_phy_bmsr_link_up) != 0U) ? 1U : 0U);
  out->auto_neg_done   = (uint8_t)(((bmsr & k_ra_rmac_phy_bmsr_an_complete) != 0U) ? 1U : 0U);
  out->speed           = k_ra_rmac_phy_speed_no_link;
  out->partner_ability = 0U;

  if ((out->link_up != 0U) && (out->auto_neg_done != 0U)) {
    /* 1000T status first; if no gbit, fall back to LPA. */
    uint16_t msr = 0U;
    if (s_state.gbit_advertise != 0U) {
      err =
        s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_1000t_status, &msr);
      if ((err == k_ra_ok) && ((msr & k_ra_rmac_phy_msr_1000full) != 0U)) {
        out->speed = k_ra_rmac_phy_speed_1000f;
        return k_ra_ok;
      }
      if ((err == k_ra_ok) && ((msr & k_ra_rmac_phy_msr_1000half) != 0U)) {
        out->speed = k_ra_rmac_phy_speed_1000h;
        return k_ra_ok;
      }
    }
    uint16_t lpa = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_an_partner, &lpa);
    if (err == k_ra_ok) {
      out->partner_ability = lpa;
      if ((lpa & k_ra_rmac_phy_lpa_100full) != 0U) {
        out->speed = k_ra_rmac_phy_speed_100f;
      } else if ((lpa & k_ra_rmac_phy_lpa_100half) != 0U) {
        out->speed = k_ra_rmac_phy_speed_100h;
      } else if ((lpa & k_ra_rmac_phy_lpa_10full) != 0U) {
        out->speed = k_ra_rmac_phy_speed_10f;
      } else if ((lpa & k_ra_rmac_phy_lpa_10half) != 0U) {
        out->speed = k_ra_rmac_phy_speed_10h;
      }
    }
  }
  return k_ra_ok;
}

ra_err_t ra_rmac_phy_lsi_get(ra_rmac_phy_lsi_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  *out = s_state.lsi_type;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
