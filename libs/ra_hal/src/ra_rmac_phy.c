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
#include "ra_rmac_phy_internal.h"

/**
 * @brief Pure speed-detected predicate -- see header for full contract.
 * @details Promoted helper so the line-352/356 AND can be driven under MC/DC.
 * @param[in] err       Result of the prior MIIM read.
 * @param[in] reg_value Register value just read.
 * @param[in] mask      Speed-bit mask to test.
 * @return Boolean predicate.
 * @retval true  Speed bit set and read OK.
 * @retval false Otherwise.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_rmac_phy_internal_speed_ok(ra_err_t err, uint16_t reg_value, uint16_t mask)
{
  return (err == k_ra_ok) && ((reg_value & mask) != 0U);
}

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

/**
 * @brief Open the off-chip Ethernet PHY via Clause-22 MDIO.
 *
 * @details
 * Validates the injected MDIO IO callbacks, drives BMCR.RESET (IEEE
 * 802.3 Clause 22) and polls until the PHY clears it, then writes the
 * caller's auto-negotiation advertisement registers (LPA + optional
 * 1000T-CTRL). The MDIO transport itself is provided through the RMAC
 * MDIO interface (HUM Ch 41 "RMAC", p 1893 -- "Station Management").
 *
 * @param[in] cfg Non-NULL configuration block (PHY address, advertise
 *                masks and IO function pointers).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 PHY reset and advertised.
 * @retval k_ra_err_null_ptr       ``cfg`` or required IO callback NULL.
 * @retval k_ra_err_invalid_arg    ``phy_address`` or ``lsi_type`` bad.
 * @retval k_ra_err_exists         Driver already opened.
 * @retval k_ra_err_hw_timeout     BMCR.RESET did not self-clear.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre RMAC station-management clock has been brought up.
 *
 * @post Driver state holds the validated configuration on success.
 * @post PHY is in a known reset-then-advertised state.
 *
 * @note Not thread-safe; one PHY per driver instance.
 * @since 0.1.0
 */
/**
 * @brief Issue BMCR.RESET to the PHY and poll until it self-clears.
 *
 * @details
 * IEEE 802.3 Clause 22 6.3.5.2.5 requires BMCR bit 15 (RESET) to be
 * write-only; the PHY clears it once internal state has been
 * re-initialised. Polls up to ``poll_max`` MDIO reads before giving up.
 *
 * @param[in] poll_max Maximum read iterations before timing out.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 BMCR.RESET observed clear.
 * @retval k_ra_err_hw_timeout     BMCR.RESET still set after poll_max reads.
 *
 * @pre ``s_state.io.read`` and ``s_state.io.write`` non-NULL.
 * @pre ``s_state.phy_address`` is the open driver's PHY.
 * @post On success the PHY is back in its post-reset default state.
 * @post On error the PHY may be partway through reset; caller closes driver.
 *
 * @note Not thread-safe; called from open under IRQ-masked init.
 * @since 0.1.0
 */
static ra_err_t internal_reset_and_wait(uint16_t poll_max)
{
  ra_err_t err = s_state.io.write(s_state.io.ctx,
                                  s_state.phy_address,
                                  k_ra_rmac_phy_reg_control,
                                  k_ra_rmac_phy_bmcr_reset);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint16_t i = 0U; i < poll_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    uint16_t reg = 0U;
    err = s_state.io.read(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_control, &reg);
    if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
      return err;
    }
    if ((reg & k_ra_rmac_phy_bmcr_reset) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Program the auto-negotiation advertisement registers.
 *
 * @details
 * Writes the cached local-advertise word into Clause-22 register 4
 * (AN_ADVERT) and -- if non-zero -- the gbit-advertise word into
 * register 9 (1000BASE-T control). Skipping the second write keeps
 * 10/100-only PHYs from rejecting an unsupported register access.
 *
 * @return ``ra_err_t`` error code propagated from the MDIO callback.
 * @retval k_ra_ok                  Advertisement programmed.
 *
 * @pre Caller has populated ``s_state.local_advertise`` / ``gbit_advertise``.
 * @pre Driver still holds the opened-but-tentative state from open().
 * @post Advertised abilities reflect the cached configuration on success.
 * @post Driver state is unchanged on error.
 *
 * @note Not thread-safe; called only from open().
 * @since 0.1.0
 */
static ra_err_t internal_program_advertise(void)
{
  ra_err_t err = s_state.io.write(s_state.io.ctx,
                                  s_state.phy_address,
                                  k_ra_rmac_phy_reg_an_advert,
                                  s_state.local_advertise);
  if (err != k_ra_ok) {
    return err;
  }
  if (s_state.gbit_advertise != 0U) {
    err = s_state.io.write(s_state.io.ctx,
                           s_state.phy_address,
                           k_ra_rmac_phy_reg_1000t_ctrl,
                           s_state.gbit_advertise);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Open the RMAC PHY driver and bring the off-chip PHY out of reset.
 *
 * @details
 * Validates the caller-supplied configuration, captures the MDIO IO seam
 * (read/write function pointers) into module-private state, issues a soft
 * reset over MDIO and polls BMCR.RESET to clear, then programs the
 * advertised abilities into ANAR (and 1000BASE-T control where the LSI
 * supports gigabit). Subsequent ``ra_rmac_phy_*`` calls operate against
 * the cached state; only one open instance is permitted at a time.
 *
 * Algorithm:
 *   1. Null-pointer + range validation on cfg / cfg->io / phy_address.
 *   2. Copy cfg fields into the module-private ``s_state``.
 *   3. internal_reset_and_wait() drives BMCR.RESET=1 and polls clear.
 *   4. internal_program_advertise() writes ANAR (and 1000T_CTRL if used).
 *
 * @param[in] cfg PHY configuration: IO seam, MDIO address (0..31),
 *                LSI family, advertised abilities, and reset poll budget.
 *                Must not be nullptr; ``cfg->io.read`` and
 *                ``cfg->io.write`` must be non-null.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  PHY reset and advertise programmed.
 * @retval k_ra_err_invalid_arg     cfg/io pointers null, phy_address out
 *                                  of range, or unknown lsi_type.
 * @retval k_ra_err_exists          Driver already opened; close first.
 * @retval k_ra_err_io              MDIO write to BMCR/ANAR failed.
 * @retval k_ra_err_timeout         BMCR.RESET did not clear within budget.
 *
 * @pre Caller is in single-threaded init context with the MDIO bus idle.
 * @pre RMAC controller MDIO is clocked (caller has enabled the MAC).
 * @post On k_ra_ok the driver is in the opened state with cached config.
 * @post On failure the driver remains closed (s_state.opened == false).
 *
 * @note Not thread-safe; the module owns a single static state instance.
 * @since 0.1.0
 */
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

  const uint16_t poll_max =
    (cfg->reset_poll_max == 0U) ? k_ra_rmac_phy_reset_poll_max : cfg->reset_poll_max;
  ra_err_t err = internal_reset_and_wait(poll_max);
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }

  err = internal_program_advertise();
  if (err != k_ra_ok) {
    s_state.opened = false;
    return err;
  }
  ra_log_info_val(s_tag, "rmac phy lsi", (uint32_t)cfg->lsi_type);
  return k_ra_ok;
}

/**
 * @brief Close the PHY driver and forget all cached state.
 *
 * @details
 * Marks the driver as closed so subsequent IO calls return
 * ``k_ra_err_not_initialized``. The PHY itself is left in its
 * current state (the caller may issue another reset via
 * ``ra_rmac_phy_open`` later).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Driver state cleared.
 * @retval k_ra_err_invalid_state   Driver was not open.
 *
 * @pre IRQs masked or single-threaded shutdown context.
 * @pre ``ra_rmac_phy_open`` previously succeeded.
 *
 * @post Driver state is marked closed.
 * @post Off-chip PHY register state is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_rmac_phy_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  return k_ra_ok;
}

/**
 * @brief Read a 16-bit Clause-22 PHY register.
 *
 * @details
 * Forwards the read to the injected ``io.read`` callback so the
 * caller controls the actual MDIO transport. ``reg_addr`` must be in
 * 0..31 (Clause-22 5-bit register space).
 *
 * @param[in]  reg_addr Clause-22 register index 0..31.
 * @param[out] out_data Receives the 16-bit register value.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Read completed.
 * @retval k_ra_err_null_ptr        ``out_data`` was NULL.
 * @retval k_ra_err_not_initialized ``ra_rmac_phy_open`` not called.
 * @retval k_ra_err_invalid_arg     ``reg_addr`` > 31.
 *
 * @pre ``out_data`` non-NULL.
 * @pre Driver previously opened.
 *
 * @post ``*out_data`` reflects the live PHY register value.
 * @post No PHY register has been written.
 *
 * @note Re-entrancy is governed by the injected IO callback.
 * @since 0.1.0
 */
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

/**
 * @brief Write a 16-bit Clause-22 PHY register.
 *
 * @details
 * Forwards the write to the injected ``io.write`` callback. The PHY
 * may treat the write as set-and-forget or as a self-clearing strobe
 * depending on the register (e.g. BMCR.RESET).
 *
 * @param[in] reg_addr Clause-22 register index 0..31.
 * @param[in] data     Value to write.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Write completed.
 * @retval k_ra_err_not_initialized ``ra_rmac_phy_open`` not called.
 * @retval k_ra_err_invalid_arg     ``reg_addr`` > 31.
 *
 * @pre Driver previously opened.
 * @pre Caller has serialised access to MDIO.
 *
 * @post Selected PHY register reflects ``data`` per IEEE 802.3 Clause 22.
 * @post Driver state is unchanged.
 *
 * @note Re-entrancy is governed by the injected IO callback.
 * @since 0.1.0
 */
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

/**
 * @brief Restart auto-negotiation by writing BMCR.{ANE | ANR}.
 *
 * @details
 * Sets the AN-enable + AN-restart bits in the IEEE 802.3 Clause-22
 * BMCR (register 0). Useful after the link partner changes or after
 * the local advertisement masks were updated.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  Restart command issued.
 * @retval k_ra_err_not_initialized ``ra_rmac_phy_open`` not called.
 *
 * @pre Driver previously opened.
 * @pre Local + gigabit advertisement registers have been programmed.
 *
 * @post BMCR.ANE and BMCR.ANR are set.
 * @post Auto-negotiation FSM restarts on the link.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_rmac_phy_auto_negotiate_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  const uint16_t bmcr = (uint16_t)(k_ra_rmac_phy_bmcr_an_enable | k_ra_rmac_phy_bmcr_an_restart);
  return s_state.io.write(s_state.io.ctx, s_state.phy_address, k_ra_rmac_phy_reg_control, bmcr);
}

/**
 * @brief Read BMSR / 1000T-status / LPA and decode the link state.
 *
 * @details
 * Reads BMSR (IEEE 802.3 register 1), then -- if AN is complete and
 * the link is up -- consults the gigabit controller/peripheral status (MSR,
 * register 10) and the partner ability (LPA, register 5) to map the
 * advertised speeds to ::ra_rmac_phy_speed_t.
 *
 * @param[out] out Receives the decoded link snapshot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  ``*out`` populated.
 * @retval k_ra_err_null_ptr        ``out`` was NULL.
 * @retval k_ra_err_not_initialized Driver not open.
 *
 * @pre ``out`` non-NULL.
 * @pre Driver previously opened.
 *
 * @post ``*out`` reflects the live PHY state.
 * @post ``s_state.last_bmsr`` caches the raw BMSR for diagnostics.
 *
 * @note Not thread-safe; pair with caller-side mutex if shared.
 * @since 0.1.0
 */
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
      if (ra_rmac_phy_internal_speed_ok(err, msr, (uint16_t)k_ra_rmac_phy_msr_1000full)) {
        out->speed = k_ra_rmac_phy_speed_1000f;
        return k_ra_ok;
      }
      if (ra_rmac_phy_internal_speed_ok(err, msr, (uint16_t)k_ra_rmac_phy_msr_1000half)) {
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

/**
 * @brief Return the LSI tag the caller passed to ``ra_rmac_phy_open``.
 *
 * @details
 * Echoes back the cached ::ra_rmac_phy_lsi_t so higher-level stacks
 * can branch on the on-board PHY model without re-querying MDIO.
 *
 * @param[out] out Receives the cached LSI tag.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                  ``*out`` populated.
 * @retval k_ra_err_null_ptr        ``out`` was NULL.
 * @retval k_ra_err_not_initialized Driver not open.
 *
 * @pre ``out`` non-NULL.
 * @pre Driver previously opened.
 *
 * @post ``*out`` equals the value passed in ``cfg->lsi_type``.
 * @post Driver state is unchanged.
 *
 * @note Read-only; safe under simple races.
 * @since 0.1.0
 */
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
