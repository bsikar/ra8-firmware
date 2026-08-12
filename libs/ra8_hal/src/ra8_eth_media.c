/**
 * @file ra8_eth_media.c
 * @brief ESWM media mux: select a port's MII/RGMII mode and release its block.
 *
 * @par Tag
 * [Ring 2 / HAL] {World: S}
 *
 * @details
 * The media half of the Ethernet HAL, kept apart from the frame-level NIC API
 * in ra8_eth.c. Choosing how a port talks to its PHY, and taking that port's
 * interface block out of reset, happens once during bring-up and has nothing
 * in common with opening a NIC or moving frames -- and ra8_eth.c had grown to
 * the 1000-line cap, which the file-size policy answers by splitting a
 * responsibility rather than by granting a waiver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_ether_regs.h"
#include "ra8_log.h"

ra8_err_t ra8_eth_rgmii_select(ra8_eth_mii_port_t port)
{
  /** @brief Log tag -- block scope: this is the only function here that logs. */
  static const char* const s_eth_media_tag = "ETH";

  RA8_CHECK_RANGE_TAG((uint32_t)port,
                      0U,
                      (uint32_t)k_ra8_eth_mii_port_1,
                      k_ra8_err_invalid_arg,
                      s_eth_media_tag);

  /* Port m -> MIICRm control register + MIIRR.RGRSTm enable bit. */
  volatile uint32_t* miicr = (port == k_ra8_eth_mii_port_0) ? ra8_eswm_miicr0() : ra8_eswm_miicr1();
  const uint32_t     rgrst = (port == k_ra8_eth_mii_port_0) ? (uint32_t)k_ra8_eswm_miirr_rgrst0
                                                            : (uint32_t)k_ra8_eswm_miirr_rgrst1;

  /* Write MIICR before enabling the per-port RGMII block so the pin mux is
   * in the correct mode the instant the data pins go live. */
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  *miicr = (uint32_t)k_ra8_eswm_miicr_txcide | (uint32_t)k_ra8_eswm_miicr_miisel_rgmii;

  /* HUM Ch 29.2.1.2 "MIIRR : Media-independent Interface Reset Register"
   * p 1289: RGRSTm is 0 = Reset, 1 = Enable -- set it to bring the RGMII
   * block out of reset so TXC is generated and the RMAC RX state machine is
   * clocked. */
  /* HUM Ch 29.2.1.2 "MIIRR : Media-independent Interface Reset Register" p 1289 */
  uint32_t miirr = *ra8_eswm_miirr();
  miirr |= rgrst;
  /* HUM Ch 29.2.1.2 "MIIRR : Media-independent Interface Reset Register" p 1289 */
  *ra8_eswm_miirr() = miirr;

  ra8_log_info(s_eth_media_tag, "rgmii_select");
  return k_ra8_ok;
}
