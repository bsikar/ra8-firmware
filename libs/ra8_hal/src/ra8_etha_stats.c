/**
 * @file ra8_etha_stats.c
 * @brief ETHA per-port statistics, descriptor-ring sizing, and PHY open -- HUM Ch 32
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Split-out half of the RA8D2 ETHA driver (HUM Ch 32, p 1627-1702). This
 * translation unit holds the host-side bookkeeping and bring-up paths that
 * sit alongside the lifecycle / IRQ / traffic-shaping path in ra8_etha.c:
 *  - Descriptor-ring sizing (::ra8_etha_descriptor_ring_init), which clamps
 *    every per-class TX queue depth to the host ring depth;
 *  - Per-port software traffic counters (::ra8_etha_get_stats,
 *    ::ra8_etha_account_traffic) with saturating accumulation;
 *  - PHY bring-up (::ra8_etha_open), driving ETHA into OPERATION and then
 *    sequencing the RMAC PHY auto-negotiation handshake.
 *
 * The shared logger tag, per-port slot table, and port range-check
 * predicate live in ra8_etha_internal.h and are defined in ra8_etha.c.
 *
 * Every register access carries a HUM Ch 32 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_internal.h"
#include "ra8_etha_regs.h"
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra8_etha_* call in this TU.
 *
 * @details TU-local read-only logger tag. The lifecycle TU (ra8_etha.c)
 * keeps its own identical copy; the string is immutable so duplicating
 * it avoids cross-TU external linkage.
 * @note Read-only after init; treat as immutable.
 * @since 0.1.0
 */
static const char* s_tag = "ETHA";

/**
 * @enum ra8_etha_ring_limits_t
 * @brief Bound checks for ::ra8_etha_descriptor_ring_init arguments.
 */
typedef enum : uint16_t {
  k_ra8_etha_ring_count_min = 1U,     /**< Minimum descriptor ring depth. */
  k_ra8_etha_ring_count_max = 4096U,  /**< Maximum descriptor ring depth. */
  k_ra8_etha_ring_buf_min   = 64U,    /**< IEEE 802.3 minimum frame.      */
  k_ra8_etha_ring_buf_max   = 16383U, /**< 14-bit frame-size field cap.   */
} ra8_etha_ring_limits_t;

/**
 * @brief Validate descriptor-ring sizing.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] num_tx See header declaration for direction and constraints.
 * @param[in] num_rx See header declaration for direction and constraints.
 * @param[in] buffer_size See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline bool internal_ring_args_ok(uint16_t num_tx, uint16_t num_rx, uint16_t buffer_size)
{
  return (num_tx >= k_ra8_etha_ring_count_min) && (num_tx <= k_ra8_etha_ring_count_max) &&
         (num_rx >= k_ra8_etha_ring_count_min) && (num_rx <= k_ra8_etha_ring_count_max) &&
         (buffer_size >= k_ra8_etha_ring_buf_min) && (buffer_size <= k_ra8_etha_ring_buf_max);
}

/**
 * @brief Saturating-add helper for the per-port traffic counters.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] base See header declaration for direction and constraints.
 * @param[in] inc See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_sat_add_u32(uint32_t base, uint32_t inc)
{
  if (inc > (UINT32_MAX - base)) {
    return UINT32_MAX;
  }
  return base + inc;
}

ra8_err_t ra8_etha_descriptor_ring_init(ra8_etha_port_t channel,
                                        uint16_t        num_tx,
                                        uint16_t        num_rx,
                                        uint16_t        buffer_size)
{
  if (!internal_port_ok(channel)) {
    ra8_log_error(s_tag, "etha_descriptor_ring_init: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  if (!internal_ring_args_ok(num_tx, num_rx, buffer_size)) {
    ra8_log_error(s_tag, "etha_descriptor_ring_init: ring args out of range");
    return k_ra8_err_invalid_arg;
  }
  s_etha_slots[(uint8_t)channel].stats.ring_tx  = num_tx;
  s_etha_slots[(uint8_t)channel].stats.ring_rx  = num_rx;
  s_etha_slots[(uint8_t)channel].stats.ring_buf = buffer_size;

  /* Clamp every per-class TX queue depth to num_tx so the hardware
   * never schedules a class deeper than the host-side ring can hold.
   * EATDQDC is 11 bits (k_ra8_etha_mask_dqd = 0x7FF = 2047). */
  uint16_t depth = num_tx;
  if ((uint32_t)depth > k_ra8_etha_mask_dqd) {
    depth = (uint16_t)k_ra8_etha_mask_dqd;
  }
  volatile r_etha_regs_t* reg = ra8_etha(channel);
  for (uint8_t i = 0U; i < k_ra8_etha_tc_count; ++i) {
    /* HUM Ch 32.3.2.7 "EATDQDCq" p 1636 */
    reg->EATDQDC[i] = (uint32_t)depth & k_ra8_etha_mask_dqd;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_etha_get_stats(ra8_etha_port_t channel, ra8_etha_port_stats_t* out_stats)
{
  RA8_CHECK_NULL_PTR(out_stats, s_tag, "etha_get_stats: out_stats null");
  if (!internal_port_ok(channel)) {
    ra8_log_error(s_tag, "etha_get_stats: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  *out_stats = s_etha_slots[(uint8_t)channel].stats;
  return k_ra8_ok;
}

ra8_err_t ra8_etha_account_traffic(ra8_etha_port_t channel,
                                   uint32_t        tx_ok,
                                   uint32_t        tx_err,
                                   uint32_t        rx_ok,
                                   uint32_t        rx_err,
                                   uint32_t        rx_drop)
{
  if (!internal_port_ok(channel)) {
    ra8_log_error(s_tag, "etha_account_traffic: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  ra8_etha_port_stats_t* st = &s_etha_slots[(uint8_t)channel].stats;
  st->tx_ok                 = internal_sat_add_u32(st->tx_ok, tx_ok);
  st->tx_err                = internal_sat_add_u32(st->tx_err, tx_err);
  st->rx_ok                 = internal_sat_add_u32(st->rx_ok, rx_ok);
  st->rx_err                = internal_sat_add_u32(st->rx_err, rx_err);
  st->rx_drop               = internal_sat_add_u32(st->rx_drop, rx_drop);
  return k_ra8_ok;
}

/**
 * @brief Transition ETHA @p channel into OPERATION and wait for EAMS.
 *
 * @details Writes EAMC = OPERATION and polls EAMS.OPS until it matches
 * (or the bounded budget elapses). Without this wait a TX/RX kick that
 * follows can fire before the chip has actually entered OPERATION and
 * silently drop frames.
 *
 * @param[in] channel ETHA port (0 or 1).
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok            EAMS observed in OPERATION.
 * @retval k_ra8_err_hw_timeout EAMS never reached OPERATION.
 *
 * @pre Caller has validated @p channel.
 * @pre ETHA block is powered and out of RESET.
 * @post On success EAMC=EAMS=OPERATION.
 * @post On failure ETHA may be in any state.
 * @note Not thread-safe; serialise ETHA mode changes.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_etha_to_operation(ra8_etha_port_t channel)
{
  enum : uint32_t { k_ra8_etha_mode_spin = 200000U /**< RA8 etha mode spin. */ };
  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 +
   * HUM Ch 32.3.1.2 "EAMS : Mode Status Register" p 1631 */
  volatile r_etha_regs_t* reg = ra8_etha(channel);
  reg->EAMC                   = (uint32_t)k_ra8_etha_opc_operation;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_etha_mode_spin; ++i) {
    if ((reg->EAMS & k_ra8_etha_mask_ops) == (uint32_t)k_ra8_etha_opc_operation) {
      return k_ra8_ok;
    }
  }
  ra8_log_error(s_tag, "etha_to_operation: EAMS never reached OPERATION");
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_etha_open(ra8_etha_port_t            channel,
                        const ra8_etha_phy_open_t* phy,
                        ra8_rmac_phy_link_t*       out_link)
{
  RA8_CHECK_NULL_PTR(phy, s_tag, "etha_open: phy null");
  RA8_CHECK_NULL_PTR(out_link, s_tag, "etha_open: out_link null");
  if (!internal_port_ok(channel)) {
    ra8_log_error(s_tag, "etha_open: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  /* Map ETHA channel index 1:1 onto the corresponding RMAC port. */
  const ra8_rmac_port_t rmac_port = (ra8_rmac_port_t)(uint8_t)channel;

  const ra8_err_t op_err = internal_etha_to_operation(channel);
  if (op_err != k_ra8_ok) {
    return op_err;
  }

  /* Each PHY step below is an MDIO transaction pair (drain + post-wait)
   * polling the RMAC MPSM register. On the host unit-test build those
   * waits consult the ra8_fake_mmio seam, so each error leg is reached by
   * failing the matching MPSM wait-loop (fail_wait / fail_nth_wait). */
  ra8_err_t err = ra8_rmac_phy_reset(rmac_port, phy->phy_addr);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_open: phy_reset");
    return err;
  }
  err = ra8_rmac_phy_set_advertise(rmac_port, phy->phy_addr, phy->advertise);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_open: set_advertise");
    return err;
  }
  err = ra8_rmac_phy_auto_neg_start(rmac_port, phy->phy_addr);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_open: auto_neg_start");
    return err;
  }
  return ra8_rmac_phy_auto_neg_wait(rmac_port, phy->phy_addr, phy->timeout_ms, out_link);
}
