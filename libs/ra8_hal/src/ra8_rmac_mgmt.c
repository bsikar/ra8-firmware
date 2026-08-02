/**
 * @file ra8_rmac_mgmt.c
 * @brief RMAC status read/clear + statistics snapshot + Clause-22 PHY -- HUM Ch 33
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to `ra8_rmac.c`, split out so each file
 * stays well under the per-file line cap. This unit owns three cohesive
 * sub-responsibilities of the RA8D2 RMAC block (HUM Ch 33, p 1703-1786):
 *
 *   - IRQ status read / clear (HUM Ch 33.4 MEIS / MMIS0..2 / MEID /
 *     MMID0..2 p 1706, plus MPIM / MRMAC0 / MRMAC1 monitoring), exposed
 *     through ::ra8_rmac_get_status and ::ra8_rmac_clear_status.
 *   - The full statistic counter snapshot (HUM Ch 33.4 MMPFTCT ..
 *     MTXBCPL p 1706), exposed through ::ra8_rmac_read_stats and its three
 *     private snapshot helpers (pause/PFC/EEE, receive, transmit).
 *   - The IEEE 802.3 Clause-22 PHY management glue layered on the MDIO
 *     primitives in `ra8_rmac.c`: PHY soft-reset, advertisement program,
 *     auto-negotiation start / wait, and link-status read. These call the
 *     public ::ra8_rmac_mdio_c22_read / ::ra8_rmac_mdio_c22_write API.
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
static const char* s_tag = "RMAC";

/**
 * @enum ra8_rmac_phy_poll_t
 * @brief PHY-management bounded poll budgets.
 */
typedef enum : uint32_t {
  k_ra8_rmac_phy_reset_iter_cap      = 4096UL,  /**< BMCR.RESET self-clear cap. */
  k_ra8_rmac_phy_anwait_iter_cap     = 65536UL, /**< Auto-neg internal cap.     */
  k_ra8_rmac_phy_anwait_iters_per_ms = 100UL,   /**< 10us per spin -> 100/ms.   */
} ra8_rmac_phy_poll_t;

/**
 * @enum ra8_rmac_phy_reg_addr_t
 * @brief IEEE 802.3 Clause 22 register addresses.
 *
 * @details
 * Reference: IEEE Std 802.3-2018 Clause 22 sec 22.2.4 "Management
 * register set". These five registers are the only ones the RMAC
 * PHY-control glue touches; everything else is vendor-specific.
 */
typedef enum : uint8_t {
  k_ra8_rmac_phy_reg_bmcr   = 0U,  /**< IEEE 802.3 Clause 22 sec 22.2.4.1 BMCR. */
  k_ra8_rmac_phy_reg_bmsr   = 1U,  /**< IEEE 802.3 Clause 22 sec 22.2.4.2 BMSR. */
  k_ra8_rmac_phy_reg_anar   = 4U,  /**< IEEE 802.3 Clause 28.2.4 ANAR.          */
  k_ra8_rmac_phy_reg_anlpar = 5U,  /**< IEEE 802.3 Clause 28.2.4 ANLPAR.        */
  k_ra8_rmac_phy_addr_max   = 31U, /**< 5-bit MDIO PHY address ceiling.         */
} ra8_rmac_phy_reg_addr_t;

/**
 * @enum ra8_rmac_phy_bit_t
 * @brief IEEE 802.3 Clause 22 BMCR / BMSR / ANxR field bits.
 */
typedef enum : uint16_t {
  k_ra8_rmac_phy_bmcr_an_restart = 0x0200U, /**< BMCR bit 9.           */
  k_ra8_rmac_phy_bmcr_an_enable  = 0x1000U, /**< BMCR bit 12.          */
  k_ra8_rmac_phy_bmcr_reset      = 0x8000U, /**< BMCR bit 15.          */
  k_ra8_rmac_phy_bmsr_link_up    = 0x0004U, /**< BMSR bit 2.           */
  k_ra8_rmac_phy_bmsr_an_done    = 0x0020U, /**< BMSR bit 5.           */
  k_ra8_rmac_phy_anar_selector   = 0x0001U, /**< ANAR bit 0 (802.3).   */
  k_ra8_rmac_phy_anlpar_10_hd    = 0x0020U, /**< ANLPAR 10BASE-T HD.   */
  k_ra8_rmac_phy_anlpar_10_fd    = 0x0040U, /**< ANLPAR 10BASE-T FD.   */
  k_ra8_rmac_phy_anlpar_100_hd   = 0x0080U, /**< ANLPAR 100BASE-TX HD. */
  k_ra8_rmac_phy_anlpar_100_fd   = 0x0100U, /**< ANLPAR 100BASE-TX FD. */
} ra8_rmac_phy_bit_t;

ra8_err_t ra8_rmac_get_status(ra8_rmac_port_t port, ra8_rmac_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "rmac_get_status: out must not be nullptr");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "rmac_get_status: port out of range");
    return k_ra8_err_invalid_arg;
  }

  volatile r_rmac_regs_t* reg = ra8_rmac(port);
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
static void internal_snapshot_rx(volatile r_rmac_regs_t* reg, ra8_rmac_stats_t* out)
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
static void internal_snapshot_tx(volatile r_rmac_regs_t* reg, ra8_rmac_stats_t* out)
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

/**
 * @brief Validate the (port, phy_addr) tuple shared by every PHY helper.
 *
 * @param[in] port     Port to validate.
 * @param[in] phy_addr 5-bit PHY address to validate.
 * @return true iff both arguments are in range.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline bool internal_phy_args_ok(ra8_rmac_port_t port, uint8_t phy_addr)
{
  return internal_port_ok(port) && (phy_addr <= (uint8_t)k_ra8_rmac_phy_addr_max);
}

/**
 * @brief Decode an ANLPAR snapshot into the highest-priority capability.
 *
 * @details
 * IEEE 802.3 Clause 28.2.4.4 "Link Partner Ability Register" defines
 * the priority order for resolved capabilities.
 *
 * @param[in] anlpar Raw ANLPAR contents read over MDIO.
 * @return Resolved ::ra8_rmac_phy_speed_t value.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_rmac_phy_speed_t internal_decode_anlpar(uint16_t anlpar)
{
  if ((anlpar & (uint16_t)k_ra8_rmac_phy_anlpar_100_fd) != 0U) {
    return k_ra8_rmac_phy_speed_100_fd;
  }
  if ((anlpar & (uint16_t)k_ra8_rmac_phy_anlpar_100_hd) != 0U) {
    return k_ra8_rmac_phy_speed_100_hd;
  }
  if ((anlpar & (uint16_t)k_ra8_rmac_phy_anlpar_10_fd) != 0U) {
    return k_ra8_rmac_phy_speed_10_fd;
  }
  if ((anlpar & (uint16_t)k_ra8_rmac_phy_anlpar_10_hd) != 0U) {
    return k_ra8_rmac_phy_speed_10_hd;
  }
  return k_ra8_rmac_phy_speed_unknown;
}

ra8_err_t ra8_rmac_phy_reset(ra8_rmac_port_t port, uint8_t phy_addr)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra8_log_error(s_tag, "phy_reset: bad args");
    return k_ra8_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 22 sec 22.2.4.1.1 "BMCR.RESET" -- self-clearing.
   * HUM Ch 33.4.1.1 "MPSM" p 1707 carries the MDIO write. */
  const ra8_err_t w = ra8_rmac_mdio_c22_write(port,
                                              phy_addr,
                                              (uint8_t)k_ra8_rmac_phy_reg_bmcr,
                                              (uint16_t)k_ra8_rmac_phy_bmcr_reset);
  if (w != k_ra8_ok) {
    /* Reached on host by arming the ra8_fake_mmio seam on MPSM so the
     * MDIO write's drain or post-wait times out. */
    ra8_log_error(s_tag, "phy_reset: bmcr write");
    return w;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rmac_phy_reset_iter_cap; ++i) {
    uint16_t        bmcr = 0U;
    const ra8_err_t r =
      ra8_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_bmcr, &bmcr);
    if (r != k_ra8_ok) {
      /* Reached on host via ra8_fake_mmio_fail_nth_wait on MPSM: the
       * write's two MPSM wait-loops succeed, the read's drain fails. */
      ra8_log_error(s_tag, "phy_reset: bmcr read");
      return r;
    }
    if ((bmcr & (uint16_t)k_ra8_rmac_phy_bmcr_reset) == 0U) {
      return k_ra8_ok;
    }
    /* GCOVR_EXCL_START
     * Off-target MDIO returns BMCR = 0 on every read (ra8_rmac.c
     * internal_mpsm_issue writes PRD = 0 for reads and the pure-RAM
     * MPSM readback returns that word), so BMCR.RESET always reads
     * clear and the loop returns above on the first iteration. The
     * natural loop exit and this timeout leg require a PHY that holds
     * BMCR.RESET asserted -- read DATA the ra8_fake_mmio wait seam
     * cannot synthesize. */
  }
  ra8_log_error(s_tag, "phy_reset: bmcr.reset never cleared");
  return k_ra8_err_hw_timeout;
  /* GCOVR_EXCL_STOP */
}

ra8_err_t ra8_rmac_phy_set_advertise(ra8_rmac_port_t port, uint8_t phy_addr, uint16_t capabilities)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra8_log_error(s_tag, "phy_set_advertise: bad args");
    return k_ra8_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 28.2.4 "ANAR" -- selector field is bits [4:0],
   * value 00001 = IEEE 802.3 (the only selector RMAC supports). */
  const uint16_t anar = (uint16_t)(capabilities | (uint16_t)k_ra8_rmac_phy_anar_selector);
  return ra8_rmac_mdio_c22_write(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_anar, anar);
}

ra8_err_t ra8_rmac_phy_auto_neg_start(ra8_rmac_port_t port, uint8_t phy_addr)
{
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra8_log_error(s_tag, "phy_auto_neg_start: bad args");
    return k_ra8_err_invalid_arg;
  }
  /* IEEE 802.3 Clause 22 sec 22.2.4.1 "BMCR" -- AN_ENABLE (bit 12) +
   * AN_RESTART (bit 9). Writing both in one transaction is the
   * canonical "kick" sequence. */
  const uint16_t bmcr =
    (uint16_t)((uint16_t)k_ra8_rmac_phy_bmcr_an_enable | (uint16_t)k_ra8_rmac_phy_bmcr_an_restart);
  return ra8_rmac_mdio_c22_write(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_bmcr, bmcr);
}

ra8_err_t ra8_rmac_phy_auto_neg_wait(ra8_rmac_port_t      port,
                                     uint8_t              phy_addr,
                                     uint32_t             timeout_ms,
                                     ra8_rmac_phy_link_t* out_link)
{
  RA8_CHECK_NULL_PTR(out_link, s_tag, "phy_auto_neg_wait: out_link null");
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra8_log_error(s_tag, "phy_auto_neg_wait: bad args");
    return k_ra8_err_invalid_arg;
  }
  out_link->up    = false;
  out_link->speed = k_ra8_rmac_phy_speed_unknown;

  /* 100 iters per ms = 10us per spin (k_ra8_rmac_phy_anwait_us_per_iter). */
  uint32_t cap = (timeout_ms == 0U) ? (uint32_t)k_ra8_rmac_phy_anwait_iter_cap
                                    : (timeout_ms * (uint32_t)k_ra8_rmac_phy_anwait_iters_per_ms);
  if (cap == 0U) {
    cap = 1U;
  }

  for (uint32_t i = 0U; i < cap; ++i) {
    uint16_t bmsr = 0U;
    /* IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR" -- AN_COMPLETE bit 5,
     * LINK_STATUS bit 2. Need both set for a "ready to forward" link. */
    const ra8_err_t r =
      ra8_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_bmsr, &bmsr);
    if (r != k_ra8_ok) {
      /* Reached on host by arming the ra8_fake_mmio seam on MPSM. */
      return r;
    }
    const uint16_t need =
      (uint16_t)((uint16_t)k_ra8_rmac_phy_bmsr_an_done | (uint16_t)k_ra8_rmac_phy_bmsr_link_up);
    if ((bmsr & need) == need) {
      /* GCOVR_EXCL_START
       * Off-target MDIO delivers BMSR = 0 on every read (ra8_rmac.c
       * internal_mpsm_issue writes PRD = 0 for reads -- read DATA the
       * ra8_fake_mmio wait seam cannot synthesize), so (bmsr & need)
       * is never equal to need and this link-up read-back body -- the
       * ANLPAR fetch, its MDIO-error return, and the resolved-speed
       * decode -- is unreachable from the host. */
      uint16_t anlpar = 0U;
      /* IEEE 802.3 Clause 28.2.4.4 "ANLPAR" -- resolved capability. */
      const ra8_err_t lp =
        ra8_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_anlpar, &anlpar);
      if (lp != k_ra8_ok) {
        return lp;
      }
      out_link->up    = true;
      out_link->speed = internal_decode_anlpar(anlpar);
      return k_ra8_ok;
      /* GCOVR_EXCL_STOP */
    }
  }
  ra8_log_error(s_tag, "phy_auto_neg_wait: timeout");
  return k_ra8_err_hw_timeout;
}

ra8_err_t
ra8_rmac_phy_link_status(ra8_rmac_port_t port, uint8_t phy_addr, ra8_rmac_phy_link_t* out_link)
{
  RA8_CHECK_NULL_PTR(out_link, s_tag, "phy_link_status: out_link null");
  if (!internal_phy_args_ok(port, phy_addr)) {
    ra8_log_error(s_tag, "phy_link_status: bad args");
    return k_ra8_err_invalid_arg;
  }
  out_link->up    = false;
  out_link->speed = k_ra8_rmac_phy_speed_unknown;

  uint16_t bmsr = 0U;
  /* IEEE 802.3 Clause 22 sec 22.2.4.2 "BMSR.LINK_STATUS" (bit 2). */
  const ra8_err_t r =
    ra8_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_bmsr, &bmsr);
  if (r != k_ra8_ok) {
    /* Reached on host by arming the ra8_fake_mmio seam on MPSM. */
    return r;
  }
  out_link->up = (bmsr & (uint16_t)k_ra8_rmac_phy_bmsr_link_up) != 0U;
  // mcdc-deactivated: ra8_rmac_phy_auto_neg_start link-up + an-done gate; both bits come from the same BMSR read; PHY hardware sets BMSR.AN_DONE only after BMSR.LINK_STATUS asserts (IEEE 802.3 Clause 22 22.2.4.2 ordering) -- the second condition cannot be true while the first is false on any conformant PHY.
  if (out_link->up && ((bmsr & (uint16_t)k_ra8_rmac_phy_bmsr_an_done) != 0U)) {
    /* GCOVR_EXCL_START
     * Same off-target MDIO limitation: BMSR reads 0, so out_link->up is
     * always false and this AN-resolved-speed read-back body (ANLPAR
     * fetch, MDIO-error return, and decode) is unreachable from the
     * host. */
    uint16_t        anlpar = 0U;
    const ra8_err_t lp =
      ra8_rmac_mdio_c22_read(port, phy_addr, (uint8_t)k_ra8_rmac_phy_reg_anlpar, &anlpar);
    if (lp != k_ra8_ok) {
      return lp;
    }
    out_link->speed = internal_decode_anlpar(anlpar);
  }
  /* GCOVR_EXCL_STOP */
  return k_ra8_ok;
}
