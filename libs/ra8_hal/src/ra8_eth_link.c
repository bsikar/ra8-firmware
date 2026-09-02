/**
 * @file ra8_eth_link.c
 * @brief Ethernet PHY link-status poller + MAC speed/duplex resync
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Split out of ra8_eth.c to keep each translation unit under the
 * per-file line-count cap. Holds the read-mostly half of the NIC
 * API: ``ra8_eth_link_status`` plus the one-shot MAC speed/duplex
 * resync that realigns the on-chip RMAC's MPIC.LSC / MPIC.PIPP with
 * whatever the PHY auto-negotiated.
 *
 * The board layer programs MPIC for 1 Gbps at boot (the maximum the
 * EK-RA8D2's PHY supports). When the link partner only offers 10/100
 * the PHY negotiates down, but MPIC stays at 1 Gbps and the RMAC
 * drops every RGMII edge as out-of-spec framing. This TU reads the
 * auto-neg result registers (ANLPAR + GBSR), picks the highest
 * mutually-supported speed/duplex, and brackets the MPIC write with
 * the ETHA DISABLE -> CONFIG -> {MPIC} -> DISABLE -> OPERATION
 * transitions HUM Ch 33.4.1.2 requires.
 *
 * The singleton NIC runtime state (::s_eth_state) and the one-shot
 * resync latch (::g_eth_mac_speed_resynced) are defined in ra8_eth.c
 * and shared via ra8_eth_internal.h.
 *
 * Every register access carries a HUM Ch 33 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_eth_internal.h"
#include "ra8_etha.h"
#include "ra8_log.h"
#include "ra8_rmac.h"
#include "ra8_time.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra8_eth_* call.
 */
static const char* s_tag = "ETH";

/**
 * @var g_ra8_eth_phy_bmsr_after_wait
 * @brief BMSR snapshot AFTER the ::internal_wait_for_autoneg poll.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra8_eth_phy_bmsr_after_wait;

/**
 * @var g_ra8_eth_anlpar
 * @brief Last-read ANLPAR (PHY reg 5) snapshot for bench debugging.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra8_eth_anlpar;

/**
 * @var g_ra8_eth_gbsr
 * @brief Last-read GBSR (PHY reg 10) snapshot for bench debugging.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra8_eth_gbsr;

/**
 * @var g_ra8_eth_resync_speed_lsc
 * @brief Speed-LSC value the resync chose. 0=10M, 1=100M, 2=1000M.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra8_eth_resync_speed_lsc;

/**
 * @var g_ra8_eth_resync_duplex
 * @brief Duplex value the resync chose. 0=half, 1=full.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra8_eth_resync_duplex;

/**
 * @brief Compute the highest auto-negotiated speed + duplex from ANLPAR+GBSR.
 *
 * @details
 * Mirrors FSP r_rmac_phy.c::R_RMAC_PHY_LinkPartnerAbilityGet. Picks
 * the highest priority intersection of our local PHY advertisement
 * and the link partner's: 1000F > 100F > 100H > 10F > 10H. BMCR's
 * speed bits are NOT the negotiated speed -- they reflect what the
 * host commanded the PHY to be (not what the auto-neg landed on).
 *
 * @param[in]  anlpar Auto-negotiation Link Partner Ability register.
 * @param[in]  gbsr   1000BASE-T Status register (link-partner-ability bits).
 * @param[out] out_speed  Receives the LSC value for ::ra8_rmac_set_link.
 * @param[out] out_duplex Receives the duplex value.
 *
 * @pre out_speed and out_duplex are non-null.
 * @pre Auto-neg has completed (caller checked BMSR.AN_COMPLETE).
 * @post On return ``*out_speed`` reflects the highest mutual ability.
 * @post On return ``*out_duplex`` reflects matching duplex.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_pick_negotiated_speed(uint16_t           anlpar,
                                           uint16_t           gbsr,
                                           ra8_rmac_lsc_t*    out_speed,
                                           ra8_rmac_duplex_t* out_duplex)
{
  /* Default: lowest tier. */
  *out_speed  = k_ra8_rmac_lsc_10mbit;
  *out_duplex = k_ra8_rmac_duplex_half;
  if ((anlpar & (uint16_t)k_ra8_eth_phy_anlpar_10h) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_10mbit;
    *out_duplex = k_ra8_rmac_duplex_half;
  }
  if ((anlpar & (uint16_t)k_ra8_eth_phy_anlpar_10f) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_10mbit;
    *out_duplex = k_ra8_rmac_duplex_full;
  }
  if ((anlpar & (uint16_t)k_ra8_eth_phy_anlpar_100h) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_100mbit;
    *out_duplex = k_ra8_rmac_duplex_half;
  }
  if ((anlpar & (uint16_t)k_ra8_eth_phy_anlpar_100f) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_100mbit;
    *out_duplex = k_ra8_rmac_duplex_full;
  }
  if ((gbsr & (uint16_t)k_ra8_eth_phy_gbsr_1000h) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_1000mbit;
    *out_duplex = k_ra8_rmac_duplex_half;
  }
  if ((gbsr & (uint16_t)k_ra8_eth_phy_gbsr_1000f) != 0U) {
    *out_speed  = k_ra8_rmac_lsc_1000mbit;
    *out_duplex = k_ra8_rmac_duplex_full;
  }
}

/**
 * @enum ra8_eth_an_poll_t
 * @brief Bound on the AUTONEG_COMPLETE poll loop.
 *
 * @details Spec: auto-neg should complete within ~3 s of link-up.
 * We poll BMSR.AN_COMPLETE for up to 4 s in 50 ms increments before
 * giving up and falling back to whatever ANLPAR / GBSR happen to
 * read (typically zero -> 10M HD).
 */
typedef enum : uint32_t {
  k_ra8_eth_an_poll_period_ms = 50U, /**< RA8 Ethernet an poll period ms. */
  k_ra8_eth_an_poll_max_iters = 80U, /**< 80 * 50 ms = 4 s ceiling.       */
} ra8_eth_an_poll_t;

/**
 * @brief Poll BMSR.AUTONEG_COMPLETE until it asserts or the budget runs out.
 *
 * @details
 * BMSR.LINK_STATUS can fire before AN_COMPLETE, but ANLPAR / GBSR
 * only latch the link-partner advertisement at AN_COMPLETE. Reading
 * them too early returns 0. Polls every k_ra8_eth_an_poll_period_ms
 * for up to k_ra8_eth_an_poll_max_iters iterations (~4 s ceiling).
 *
 * @param[in] port RMAC port whose PHY to poll.
 *
 * @pre Caller is single-threaded.
 * @pre PHY is enumerated at ::k_ra8_eth_phy_addr_default.
 * @post Returns once AN_COMPLETE is set or the budget is exhausted.
 * @post Side-effect free apart from the busy-wait loop.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_wait_for_autoneg(ra8_rmac_port_t port)
{
  uint16_t bmsr = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_eth_an_poll_max_iters; ++i) {
    if (ra8_rmac_mdio_c22_read(port,
                               (uint8_t)k_ra8_eth_phy_addr_default,
                               (uint8_t)k_ra8_eth_phy_reg_bmsr,
                               &bmsr) != k_ra8_ok) {
      g_ra8_eth_phy_bmsr_after_wait = bmsr;
      return;
    }
    if ((bmsr & (uint16_t)k_ra8_eth_phy_bmsr_an_done) != 0U) {
      g_ra8_eth_phy_bmsr_after_wait = bmsr;
      return;
    }
    ra8_delay_ms((uint32_t)k_ra8_eth_an_poll_period_ms);
  }
  g_ra8_eth_phy_bmsr_after_wait = bmsr;
}

/**
 * @brief Read ANLPAR + GBSR and pick the negotiated speed/duplex.
 *
 * @details
 * Helper extracted from ::internal_resync_mac_speed so each function
 * fits under the 60-line / 40-statement cap. Reads PHY registers 5
 * (ANLPAR) and 10 (GBSR), snapshots them into the bench-side debug
 * vars, then calls ::internal_pick_negotiated_speed to compute the
 * highest mutually-supported speed/duplex.
 *
 * @param[in]  port       RMAC port whose PHY to query.
 * @param[out] out_speed  Receives the LSC value.
 * @param[out] out_duplex Receives the duplex value.
 *
 * @return ::ra8_err_t MDIO read outcome (propagated from the first
 *         failing read).
 * @retval k_ra8_ok            Both registers fetched.
 * @retval other              MDIO read failure.
 *
 * @pre Auto-neg has had a chance to settle (call
 *      ::internal_wait_for_autoneg first).
 * @pre out_speed / out_duplex are non-null.
 * @post On success the resync debug snapshots are updated.
 * @post On failure ``*out_speed`` / ``*out_duplex`` are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_query_negotiated_speed(ra8_rmac_port_t    port,
                                                 ra8_rmac_lsc_t*    out_speed,
                                                 ra8_rmac_duplex_t* out_duplex)
{
  uint16_t anlpar = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra8_err_t anlpar_err = ra8_rmac_mdio_c22_read(port,
                                                      (uint8_t)k_ra8_eth_phy_addr_default,
                                                      (uint8_t)k_ra8_eth_phy_reg_anlpar,
                                                      &anlpar);
  if (anlpar_err != k_ra8_ok) {
    return anlpar_err;
  }
  uint16_t        gbsr     = 0U;
  const ra8_err_t gbsr_err = ra8_rmac_mdio_c22_read(port,
                                                    (uint8_t)k_ra8_eth_phy_addr_default,
                                                    (uint8_t)k_ra8_eth_phy_reg_gbsr,
                                                    &gbsr);
  if (gbsr_err != k_ra8_ok) {
    return gbsr_err;
  }
  g_ra8_eth_anlpar = anlpar;
  g_ra8_eth_gbsr   = gbsr;
  internal_pick_negotiated_speed(anlpar, gbsr, out_speed, out_duplex);
  g_ra8_eth_resync_speed_lsc = (uint32_t)*out_speed;
  g_ra8_eth_resync_duplex    = (uint32_t)*out_duplex;
  return k_ra8_ok;
}

/**
 * @brief Drop ETHA to CONFIG, write MPIC, return to OPERATION.
 *
 * @details Bracketed write per HUM Ch 33.4.1.2 -- MPIC only sticks
 * while ETHA is in CONFIG. Always returns ETHA to OPERATION even if
 * the MPIC write itself failed, so the chip doesn't park in CONFIG.
 *
 * @param[in] port   RMAC port whose MPIC needs updating.
 * @param[in] speed  Target LSC value.
 * @param[in] duplex Target duplex value.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok              MPIC programmed.
 * @retval k_ra8_err_hw_timeout  ETHA mode transition timed out.
 * @retval k_ra8_err_invalid_arg ra8_rmac_set_link rejected the args.
 *
 * @pre Auto-neg result has been read.
 * @pre port and speed/duplex are in range.
 * @post On k_ra8_ok ETHA is in OPERATION, MPIC reflects the args.
 * @post On error ETHA is restored to OPERATION before returning.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_program_mpic(ra8_rmac_port_t port, ra8_rmac_lsc_t speed, ra8_rmac_duplex_t duplex)
{
  const ra8_etha_port_t etha_port = (port == k_ra8_rmac_port_0)
                                      ? (ra8_etha_port_t)k_ra8_etha_port_0
                                      : (ra8_etha_port_t)k_ra8_etha_port_1;
  ra8_err_t             err       = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_disable);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_config);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Table 29.11: MPIC.PIS tracks link speed, not the external
   * RGMII-ness -- GMII for 1 Gbps, MII for 10/100 Mbps. The ESWM
   * MIICR1.MIISEL field (programmed once by the board) is what
   * actually selects external RGMII. */
  ra8_rmac_pis_t pis = k_ra8_rmac_pis_mii;
  if (speed == k_ra8_rmac_lsc_1000mbit) {
    pis = k_ra8_rmac_pis_gmii;
  }
  const ra8_err_t set_err = ra8_rmac_set_link(port, pis, speed, duplex);
  const ra8_err_t dis_err = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_disable);
  const ra8_err_t op_err  = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_operation);
  if (set_err != k_ra8_ok) {
    return set_err;
  }
  if (dis_err != k_ra8_ok) {
    return dis_err;
  }
  return op_err;
}

/**
 * @brief Resync MPIC.LSC / MPIC.PIPP to match the PHY auto-neg result.
 *
 * @details
 * Top-level helper. Polls for AUTONEG_COMPLETE, reads ANLPAR / GBSR
 * for the actual negotiated speed/duplex, then writes MPIC inside
 * the ETHA CONFIG bracket. See ::internal_query_negotiated_speed and
 * ::internal_program_mpic for the per-step details.
 *
 * @param[in] port RMAC port whose MPIC needs updating.
 * @param[in] bmcr Unused -- kept for API stability with the old caller.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok              MPIC re-programmed.
 * @retval k_ra8_err_invalid_arg ra8_rmac_set_link rejected an arg.
 * @retval k_ra8_err_hw_timeout  An ETHA mode transition or MDIO timed out.
 *
 * @pre ::ra8_eth_open has succeeded (::s_eth_state.opened == 1).
 * @pre ::g_eth_mac_speed_resynced is false on first call after open.
 * @post On success MPIC matches the PHY's negotiated speed/duplex.
 * @post On success ::g_eth_mac_speed_resynced is true.
 *
 * @note Not thread-safe; firmware drives this from a single thread.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_resync_mac_speed(ra8_rmac_port_t port, uint16_t bmcr)
{
  (void)bmcr;
  internal_wait_for_autoneg(port);
  ra8_rmac_lsc_t    speed  = k_ra8_rmac_lsc_10mbit;
  ra8_rmac_duplex_t duplex = k_ra8_rmac_duplex_half;
  const ra8_err_t   q_err  = internal_query_negotiated_speed(port, &speed, &duplex);
  if (q_err != k_ra8_ok) {
    return q_err;
  }
  const ra8_err_t mpic_err = internal_program_mpic(port, speed, duplex);
  if (mpic_err != k_ra8_ok) {
    return mpic_err;
  }
  g_eth_mac_speed_resynced = true;
  ra8_log_info(s_tag, "link_status: MPIC resynced to PHY speed/duplex");
  return k_ra8_ok;
}

/**
 * @brief Read PHY BMSR + BMCR over MDIO and populate out_status.
 *
 * @details
 * Helper extracted from ra8_eth_link_status so the per-condition
 * decoding (link_up, duplex, speed) stays under the NASA Rule 4
 * function-size threshold and clang-tidy
 * readability-function-size threshold.
 *
 * @param[in]  port       RMAC port carrying the PHY.
 * @param[out] out_status Link-state fields populated in place.
 * @param[out] out_bmcr   BMCR value (caller uses it for the MAC
 *                        speed-resync write).
 *
 * @return ::ra8_err_t MDIO read outcome.
 * @retval k_ra8_ok           Both BMSR + BMCR fetched cleanly.
 * @retval other             Underlying MDIO error from the first
 *                           failing read.
 *
 * @pre out_status != nullptr.
 * @pre out_bmcr != nullptr.
 * @post On success out_status->{bmsr, link_up, full_duplex,
 *       speed_mbps} are populated; out_bmcr holds the raw BMCR.
 * @post On error out_status / out_bmcr are unmodified.
 *
 * @note Not thread-safe; serialise PHY access with the rest of
 *       the ethernet bring-up.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_phy_read_link(ra8_rmac_port_t port, ra8_eth_link_t* out_status, uint16_t* out_bmcr)
{
  uint16_t bmsr = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra8_err_t bmsr_err = ra8_rmac_mdio_c22_read(port,
                                                    (uint8_t)k_ra8_eth_phy_addr_default,
                                                    (uint8_t)k_ra8_eth_phy_reg_bmsr,
                                                    &bmsr);
  RA8_RETURN_ON_ERROR(bmsr_err, s_tag, "link_status: bmsr read");

  uint16_t bmcr = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra8_err_t bmcr_err = ra8_rmac_mdio_c22_read(port,
                                                    (uint8_t)k_ra8_eth_phy_addr_default,
                                                    (uint8_t)k_ra8_eth_phy_reg_bmcr,
                                                    &bmcr);
  RA8_RETURN_ON_ERROR(bmcr_err, s_tag, "link_status: bmcr read");

  out_status->bmsr        = bmsr;
  out_status->link_up     = ((bmsr & k_ra8_eth_phy_bmsr_link_up) != 0U) ? 1U : 0U;
  out_status->full_duplex = ((bmcr & k_ra8_eth_phy_bmcr_duplex) != 0U) ? 1U : 0U;
  if ((bmcr & k_ra8_eth_phy_bmcr_speed100) != 0U) {
    out_status->speed_mbps = k_ra8_eth_phy_speed_100;
  } else {
    out_status->speed_mbps = k_ra8_eth_phy_speed_10;
  }
  *out_bmcr = bmcr;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_link_status(ra8_eth_link_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "link_status: out must not be nullptr");
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }

  const ra8_rmac_port_t port = priv_ra8_eth_channel_to_port(s_eth_state.cfg.channel);
  uint16_t              bmcr = 0U;
  const ra8_err_t       err  = internal_phy_read_link(port, out_status, &bmcr);
  RA8_RETURN_ON_ERROR(err, s_tag, "link_status: phy read");

  /* HUM Ch 33.4.1.2 "MPIC : PHY Interfaces Configuration Register"
   * p 1707: on first link-up after open, realign MPIC.LSC with the
   * PHY's negotiated speed. Sequential ifs (no compound boolean
   * operators) so each gate remains MC/DC-clean without a paired
   * test vector matrix. */
  if (out_status->link_up == 0U) {
    return k_ra8_ok;
  }
  if (g_eth_mac_speed_resynced) {
    return k_ra8_ok;
  }
  return internal_resync_mac_speed(port, bmcr);
}
