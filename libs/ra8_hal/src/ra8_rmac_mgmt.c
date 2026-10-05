/**
 * @file ra8_rmac_mgmt.c
 * @brief RMAC status read/clear + statistics snapshot -- HUM Ch 33
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to `ra8_rmac.c`, split out so each file
 * stays well under the per-file line cap. This unit owns two cohesive
 * sub-responsibilities of the RA8D2 RMAC block (HUM Ch 33, p 1703-1786):
 *
 *   - IRQ status read / clear (HUM Ch 33.4 MEIS / MMIS0..2 / MEID /
 *     MMID0..2 p 1706, plus MPIM / MRMAC0 / MRMAC1 monitoring), exposed
 *     through ::ra8_rmac_get_status and ::ra8_rmac_clear_status.
 *   - The full statistic counter snapshot (HUM Ch 33.4 MMPFTCT ..
 *     MTXBCPL p 1706), exposed through ::ra8_rmac_read_stats and its three
 *     private snapshot helpers (pause/PFC/EEE, receive, transmit).
 *   - The IEEE 802.3 Clause-22 PHY helpers moved to Zig in RA8FW-744
 *     (libs/ra8_hal/src/rmac_phy_abi.zig, internal/rmac_phy.zig).
 *
 * Every register access carries a HUM Ch 33 citation. The driver-private
 * logger tag is a private read-only copy of the `ra8_rmac.c` tag so the two
 * units log under the same "RMAC" name without sharing a linker symbol.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_log.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra8_rmac_* call in this TU.
 */
static const char* const s_tag = "RMAC";

ra8_err_t ra8_rmac_get_status(ra8_rmac_port_t port, ra8_rmac_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "rmac_get_status: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_get_status: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile const r_rmac_regs_t* reg = ra8_rmac(port);
  /* HUM Ch 33.4 "MEIS : MAC Error Interrupt Status Register" p 1745 */
  out->err_status = reg->MEIS;
  /* HUM Ch 33.4 "MMIS0 : MAC Monitoring Interrupt Status Register 0" p 1756 */
  out->mon_status[0] = reg->MMIS0;
  /* HUM Ch 33.4 "MMIS1 : MAC Monitoring Interrupt Status Register 1" p 1758 */
  out->mon_status[1] = reg->MMIS1;
  /* HUM Ch 33.4 "MMIS2 : MAC Monitoring Interrupt Status Register 2" p 1761 */
  out->mon_status[2] = reg->MMIS2;
  /* HUM Ch 33.4 "MPIM : PHY Interfaces Monitoring Register" p 1710 */
  out->phy_monitor = reg->MPIM;
  /* HUM Ch 33.4 "MRMAC0 : MAC Reception MAC Address Configuration Register 0" p 1716 */
  out->mrmac0 = reg->MRMAC0;
  /* HUM Ch 33.4 "MRMAC1 : MAC Reception MAC Address Configuration Register 1" p 1717 */
  out->mrmac1 = reg->MRMAC1;
  return k_ra8_ok;
}

ra8_err_t ra8_rmac_clear_status(ra8_rmac_port_t port,
                                uint32_t        err_mask,
                                uint32_t        mon0_mask,
                                uint32_t        mon1_mask,
                                uint32_t        mon2_mask)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_clear_status: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  /* The disable registers act as the clear-on-write counterpart of
   * each status register; writing 1 to a bit clears the matching bit
   * in MEIS / MMIS{0,1,2}. The driver also writes the explicit
   * masked-out value so fake backings (which lack RW1C) end up
   * in the same observable state as real hardware. */
  /* HUM Ch 33.4 "MEID : MAC Error Interrupt Disable Register" p 1754 */
  reg->MEID = err_mask;
  /* HUM Ch 33.4 "MMID0 : MAC Monitoring Interrupt Disable Register 0" p 1758 */
  reg->MMID0 = mon0_mask;
  /* HUM Ch 33.4 "MMID1 : MAC Monitoring Interrupt Disable Register 1" p 1760 */
  reg->MMID1 = mon1_mask;
  /* HUM Ch 33.4 "MMID2 : MAC Monitoring Interrupt Disable Register 2" p 1763 */
  reg->MMID2 = mon2_mask;
  /* HUM Ch 33.4 "MEIS : MAC Error Interrupt Status Register" p 1745 */
  reg->MEIS = reg->MEIS & ~err_mask;
  /* HUM Ch 33.4 "MMIS0 : MAC Monitoring Interrupt Status Register 0" p 1756 */
  reg->MMIS0 = reg->MMIS0 & ~mon0_mask;
  /* HUM Ch 33.4 "MMIS1 : MAC Monitoring Interrupt Status Register 1" p 1758 */
  reg->MMIS1 = reg->MMIS1 & ~mon1_mask;
  /* HUM Ch 33.4 "MMIS2 : MAC Monitoring Interrupt Status Register 2" p 1761 */
  reg->MMIS2 = reg->MMIS2 & ~mon2_mask;
  return k_ra8_ok;
}

/**
 * @brief Snapshot the pause / PFC / EEE counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MMPFTCT / MAPFTCT / MPFRCT / MFCICT / MEEECT and the
 * MMPCFTCT / MAPCFTCT / MPCFRCT counter banks (p 1706).
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_snapshot_pause_pfc(volatile r_rmac_regs_t* reg, ra8_rmac_stats_t* out)
{
  out->pause_tx_manual = reg->MMPFTCT;
  out->pause_tx_auto   = reg->MAPFTCT;
  out->pause_rx        = reg->MPFRCT;
  out->false_carrier   = reg->MFCICT;
  out->eee_count       = reg->MEEECT;
  for (uint8_t i = 0; i < k_ra8_rmac_pfc_group_count; ++i) {
    out->pfc_tx_manual[i] = reg->MMPCFTCT[i];
    out->pfc_tx_auto[i]   = reg->MAPCFTCT[i];
  }
  for (uint8_t i = 0; i < k_ra8_rmac_pfc_rx_count; ++i) {
    out->pfc_rx[i] = reg->MPCFRCT[i];
  }
}

/**
 * @brief Snapshot the receive counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MROVFC ... MRXBCPL p 1706.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_snapshot_rx(volatile const r_rmac_regs_t* reg, ra8_rmac_stats_t* out)
{
  out->rx_overflow        = reg->MROVFC;
  out->rx_hdr_crc_err     = reg->MRHCRCEC;
  out->rx_good_e          = reg->MRGFCE;
  out->rx_good_p          = reg->MRGFCP;
  out->rx_broadcast       = reg->MRBFC;
  out->rx_multicast       = reg->MRMFC;
  out->rx_unicast         = reg->MRUFC;
  out->rx_phy_err         = reg->MRPEFC;
  out->rx_nibble_err      = reg->MRNEFC;
  out->rx_fcs_err         = reg->MRFMEFC;
  out->rx_final_frag_miss = reg->MRFFMEFC;
  out->rx_c_frag_err      = reg->MRCFCEFC;
  out->rx_frag_count_err  = reg->MRFCEFC;
  out->rx_filter_rejected = reg->MRRCFEFC;
  out->rx_total           = reg->MRFC;
  out->rx_good_undersize  = reg->MRGUEFC;
  out->rx_bad_undersize   = reg->MRBUEFC;
  out->rx_good_oversize   = reg->MRGOEFC;
  out->rx_bad_oversize    = reg->MRBOEFC;
  out->rx_bytes_e_upper   = reg->MRXBCEU;
  out->rx_bytes_e_lower   = reg->MRXBCEL;
  out->rx_bytes_p_upper   = reg->MRXBCPU;
  out->rx_bytes_p_lower   = reg->MRXBCPL;
}

/**
 * @brief Snapshot the transmit counters into ``out``.
 *
 * @details
 * HUM Ch 33.4 MTGFCE ... MTXBCPL p 1706.
 *
 * @param[in] reg See implementation.
 * @param[in] out See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_snapshot_tx(volatile const r_rmac_regs_t* reg, ra8_rmac_stats_t* out)
{
  out->tx_good_e        = reg->MTGFCE;
  out->tx_good_p        = reg->MTGFCP;
  out->tx_broadcast     = reg->MTBFC;
  out->tx_multicast     = reg->MTMFC;
  out->tx_unicast       = reg->MTUFC;
  out->tx_error         = reg->MTEFC;
  out->tx_bytes_e_upper = reg->MTXBCEU;
  out->tx_bytes_e_lower = reg->MTXBCEL;
  out->tx_bytes_p_upper = reg->MTXBCPU;
  out->tx_bytes_p_lower = reg->MTXBCPL;
}

ra8_err_t ra8_rmac_read_stats(ra8_rmac_port_t port, ra8_rmac_stats_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "read_stats: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "read_stats: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
  internal_snapshot_pause_pfc(reg, out);
  internal_snapshot_rx(reg, out);
  internal_snapshot_tx(reg, out);
  return k_ra8_ok;
}
