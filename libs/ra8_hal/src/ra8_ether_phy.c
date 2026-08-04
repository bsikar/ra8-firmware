/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ether_phy.c
 * @brief Generic Ethernet PHY abstraction (MDIO Clause-22)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hardware-agnostic PHY driver. The MDIO bus itself is plugged in
 * via `ra8_ether_phy_io_t` callbacks so the same code can drive the
 * RA8D2 RMAC MIIM block, the legacy ETHER MIIM, an external
 * SMI-over-GPIO bit-banger, or a host fake. State is kept in
 * a single static control block; only one PHY is tracked at a time
 * because the underlying MAC ports each use their own driver
 * instance (ra8_ethercat_phy / ra8_rmac_phy).
 */

#include "ra8_ether_phy.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* s_tag = "EPHY";

typedef enum : uint8_t {
  k_ra8_ether_phy_reg_max        = 31U, /**< RA8 ether PHY register maximum.   */
  k_ra8_ether_phy_reset_poll_max = 32U, /**< RA8 ether PHY reset poll maximum. */
} ra8_ether_phy_priv_t;

typedef enum : uint16_t {
  k_ra8_ether_phy_bmcr_reset       = 0x8000U, /**< RA8 ether PHY bmcr reset.       */
  k_ra8_ether_phy_bmcr_an_enable   = 0x1000U, /**< RA8 ether PHY bmcr an enable.   */
  k_ra8_ether_phy_bmcr_an_restart  = 0x0200U, /**< RA8 ether PHY bmcr an restart.  */
  k_ra8_ether_phy_bmsr_link_up     = 0x0004U, /**< RA8 ether PHY bmsr link up.     */
  k_ra8_ether_phy_bmsr_an_complete = 0x0020U, /**< RA8 ether PHY bmsr an complete. */
  k_ra8_ether_phy_anar_100full     = 0x0100U, /**< RA8 ether PHY anar 100full.     */
  k_ra8_ether_phy_anar_100half     = 0x0080U, /**< RA8 ether PHY anar 100half.     */
  k_ra8_ether_phy_anar_10full      = 0x0040U, /**< RA8 ether PHY anar 10full.      */
  k_ra8_ether_phy_anar_10half      = 0x0020U, /**< RA8 ether PHY anar 10half.      */
} ra8_ether_phy_bits_t;

typedef struct {
  bool                opened;        /**< Opened.        */
  uint8_t             phy_address;   /**< PHY address.   */
  ra8_ether_phy_mii_t mii_type;      /**< Mii type.      */
  uint16_t            reset_wait_us; /**< Reset wait us. */
  ra8_ether_phy_io_t  io;            /**< Io.            */
  uint16_t            last_bmsr;     /**< Last bmsr.     */
} ra8_ether_phy_internal_t;

static ra8_ether_phy_internal_t s_state = {};

/**
 * @brief Issue BMCR.RESET to the PHY and poll until it self-clears.
 *
 * @details
 * IEEE 802.3 Clause 22 requires BMCR bit 15 (RESET) to self-clear once
 * the PHY has re-initialized its internal state. Polls up to
 * ``k_ra8_ether_phy_reset_poll_max`` MDIO reads before giving up.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             BMCR.RESET observed clear.
 * @retval k_ra8_err_hw_timeout BMCR.RESET still set after the poll budget.
 *
 * @pre ``s_state.io.read`` and ``s_state.io.write`` are non-NULL.
 * @pre ``s_state.phy_address`` names the PHY being opened.
 * @post On success the PHY is back in its post-reset default state.
 * @post On error the PHY may be partway through reset; the caller
 *       rolls the driver state back to closed.
 *
 * @note Not thread-safe; called from open under single-threaded init.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_reset_and_wait(void)
{
  ra8_err_t err = s_state.io.write(s_state.io.ctx,
                                   s_state.phy_address,
                                   k_ra8_ether_phy_reg_control,
                                   k_ra8_ether_phy_bmcr_reset);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < k_ra8_ether_phy_reset_poll_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    uint16_t reg = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra8_ether_phy_reg_control, &reg);
    if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
      return err;
    }
    if ((reg & k_ra8_ether_phy_bmcr_reset) == 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_ether_phy_open(const ra8_ether_phy_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->io.read, s_tag, "io.read required");
  RA8_CHECK_NULL_PTR(cfg->io.write, s_tag, "io.write required");
  if (cfg->phy_address > k_ra8_ether_phy_addr_max) {
    return k_ra8_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra8_err_exists;
  }
  s_state.opened        = true;
  s_state.phy_address   = cfg->phy_address;
  s_state.mii_type      = cfg->mii_type;
  s_state.reset_wait_us = cfg->reset_wait_us;
  s_state.io            = cfg->io;
  s_state.last_bmsr     = 0U;

  /* Issue BMCR reset and poll for self-clear. */
  const ra8_err_t err = internal_reset_and_wait();
  if (err != k_ra8_ok) {
    s_state.opened = false;
    return err;
  }
  ra8_log_info_val(s_tag, "phy ready addr", (uint32_t)cfg->phy_address);
  return k_ra8_ok;
}

ra8_err_t ra8_ether_phy_close(void)
{
  if (!s_state.opened) {
    return k_ra8_err_invalid_state;
  }
  s_state.opened = false;
  return k_ra8_ok;
}

ra8_err_t ra8_ether_phy_mdio_read(uint8_t reg_addr, uint16_t* out_data)
{
  RA8_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  if (reg_addr > k_ra8_ether_phy_reg_max) {
    return k_ra8_err_invalid_arg;
  }
  return s_state.io.read(s_state.io.ctx, s_state.phy_address, reg_addr, out_data);
}

ra8_err_t ra8_ether_phy_mdio_write(uint8_t reg_addr, uint16_t data)
{
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  if (reg_addr > k_ra8_ether_phy_reg_max) {
    return k_ra8_err_invalid_arg;
  }
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, reg_addr, data);
}

ra8_err_t ra8_ether_phy_auto_negotiate_start(void)
{
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  /* Re-arm AN: enable + restart. */
  const uint16_t bmcr =
    (uint16_t)(k_ra8_ether_phy_bmcr_an_enable | k_ra8_ether_phy_bmcr_an_restart);
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, k_ra8_ether_phy_reg_control, bmcr);
}

ra8_err_t ra8_ether_phy_link_status_get(ra8_ether_phy_link_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  uint16_t  bmsr = 0U;
  ra8_err_t err =
    s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra8_ether_phy_reg_status, &bmsr);
  if (err != k_ra8_ok) {
    return err;
  }
  s_state.last_bmsr  = bmsr;
  out->bmsr          = bmsr;
  out->link_up       = (uint8_t)(((bmsr & k_ra8_ether_phy_bmsr_link_up) != 0U) ? 1U : 0U);
  out->auto_neg_done = (uint8_t)(((bmsr & k_ra8_ether_phy_bmsr_an_complete) != 0U) ? 1U : 0U);
  out->speed         = k_ra8_ether_phy_speed_no_link;
  if ((out->link_up != 0U) && (out->auto_neg_done != 0U)) {
    /* Inspect partner ability for resolved speed/duplex. */
    uint16_t lpa = 0U;
    err =
      s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra8_ether_phy_reg_an_partner, &lpa);
    if (err == k_ra8_ok) {
      if ((lpa & k_ra8_ether_phy_anar_100full) != 0U) {
        out->speed = k_ra8_ether_phy_speed_100f;
      } else if ((lpa & k_ra8_ether_phy_anar_100half) != 0U) {
        out->speed = k_ra8_ether_phy_speed_100h;
      } else if ((lpa & k_ra8_ether_phy_anar_10full) != 0U) {
        out->speed = k_ra8_ether_phy_speed_10f;
      } else if ((lpa & k_ra8_ether_phy_anar_10half) != 0U) {
        out->speed = k_ra8_ether_phy_speed_10h;
      }
    }
  }
  return k_ra8_ok;
}
