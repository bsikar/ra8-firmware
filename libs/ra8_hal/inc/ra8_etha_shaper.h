/**
 * @file ra8_etha_shaper.h
 * @brief Per-port ETHA VLAN / shaper / stats / ring / PHY API -- HUM Ch 32 (p 1627-1702)
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * VLAN tag insertion / extraction, RX tag filter, cut-through queue,
 * credit-based shaper (CBS / 802.1Qav), time-aware shaper (TAS /
 * 802.1Qbv), per-port MIB counters, security gate (EASCR), descriptor
 * ring sizing, software traffic accounting, and the one-shot PHY bring-up
 * helper. Split out of the umbrella ra8_etha.h to keep that header under
 * the per-file line budget; this is a pure move of the original
 * declarations (the tests-side Ethernet header parser that once lived
 * here moved to tests/eth_frame_fixture.h under issue #238). The data
 * types these functions take live in ra8_etha_types.h, the register enums
 * in ra8_etha_regs.h, and the PHY link type in ra8_rmac.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_etha_regs.h"
#include "ra8_etha_types.h"
#include "ra8_rmac.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure VLAN tag insertion / extraction for the port.
 *
 * @param[in] port Port identifier.
 * @param[in] vim Insertion mode (::ra8_etha_vim_t).
 * @param[in] vem Extraction mode (::ra8_etha_vem_t).
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EAVCC updated.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre vem fits in 3 bits.
 * @post EAVCC.VIM = vim and EAVCC.VEM = vem.
 * @post Subsequent TX frames are tagged or stripped per the new mode.
 *
 * @note Tag values to insert come from::ra8_etha_set_vlan_tag.
 * @see ra8_etha_set_vlan_tag
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_etha_set_vlan_mode(ra8_etha_port_t port, ra8_etha_vim_t vim, ra8_etha_vem_t vem);

/**
 * @brief Programme the C-VLAN and S-VLAN tag values used on TX insertion.
 *
 * @param[in] port Port identifier.
 * @param[in] c_tag C-VLAN tag (inner). Pointer to descriptor.
 * @param[in] s_tag_in S-VLAN tag (outer). Pointer to descriptor.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EAVTC packed and written.
 * @retval k_ra8_err_null_ptr c_tag or s_tag is nullptr.
 * @retval k_ra8_err_invalid_arg port out of range or vid/pcp out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre c_tag->vid <= 0xFFF and c_tag->pcp <= 7 (same for s_tag).
 * @post EAVTC has both tags packed: CTV/CTP/CTD then STV/STP/STD.
 * @post No partial write on validation failure.
 *
 * @note Only effective when EAVCC.VIM =::k_ra8_etha_vim_enabled.
 * @see ra8_etha_set_vlan_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_set_vlan_tag(ra8_etha_port_t            port,
                                              const ra8_etha_vlan_tag_t* c_tag,
                                              const ra8_etha_vlan_tag_t* s_tag_in);

/**
 * @brief Configure the RX tag filter (multicast group + VLAN-tag filter).
 *
 * @param[in] port Port identifier.
 * @param[in] mask Bit mask written verbatim to EARTFC. Bit positions
 * follow HUM Ch 32 EARTFC: NT, RT, CST, CSRT, CT,
 * CRT, SCT, SCRT, UT.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EARTFC = mask.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre Caller has used the EARTFC bit names (NT/RT/CST/...) when building mask.
 * @post EARTFC = mask & 0x1FF.
 * @post Frames matching the active filter survive; others are dropped.
 *
 * @note This is the multicast group filter for tagged traffic.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_set_rx_tag_filter(ra8_etha_port_t port, uint32_t mask);

/**
 * @brief Configure the cut-through TX queue (low-latency forwarding).
 *
 * @param[in] port Port identifier.
 * @param[in] qd 16-bit cut-through queue depth in bytes (CTQD).
 * @param[in] dqd 4-bit cut-through descriptor queue depth (CTDQD).
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EACTQC + EACTDQDC updated.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre dqd <= 15.
 * @post EACTQC.CTQD = qd; EACTDQDC.CTDQD = dqd.
 * @post Cut-through path may begin forwarding short frames sooner.
 *
 * @note CTDQD = 0 effectively disables the cut-through path.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_etha_configure_cut_through(ra8_etha_port_t port, uint16_t qd, uint8_t dqd);

/**
 * @brief Configure the credit-based shaper (CBS) for one traffic class.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[in] enable Non-zero -> set EACAEC.CEtc and EACC.CCtc.
 * @param[in] param Pointer to CBS parameters (increment + upper-limit).
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok CBS programmed.
 * @retval k_ra8_err_null_ptr param is nullptr (when enable != 0).
 * @retval k_ra8_err_invalid_arg port, tc, or values out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre param->increment fits in 20 bits, upper_lim in 31 bits.
 * @post EACAIVC[tc] = increment; EACAULC[tc] = upper_lim.
 * @post EACAEC.CEtc = enable!=0; EACC.CCtc = enable!=0.
 *
 * @note CBS is the AVB credit shaper (802.1Qav).
 * @see ra8_etha_get_cbs_state
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_configure_cbs(ra8_etha_port_t             port,
                                               ra8_etha_tc_t               tc,
                                               uint8_t                     enable,
                                               const ra8_etha_cbs_param_t* param);

/**
 * @brief Read the operational CBS gate-state vector + per-class oper params.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[out] enabled Non-zero if oper-enable mirror bit is set.
 * @param[out] gate_open Non-zero if EACGSM gate-state bit is asserted.
 * @param[out] oper_param Live oper-side increment + upper-limit values.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr any out pointer is nullptr.
 * @retval k_ra8_err_invalid_arg port or tc out of range.
 *
 * @pre Port previously brought up via ra8_etha_init.
 * @pre All output pointers are writable.
 * @post *enabled reflects EACOEM.CEtc.
 * @post *gate_open reflects EACGSM.CGStc.
 *
 * @note The oper-side mirror lags the admin-side until next CBS update.
 * @see ra8_etha_configure_cbs
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_get_cbs_state(ra8_etha_port_t       port,
                                               ra8_etha_tc_t         tc,
                                               uint8_t*              enabled,
                                               uint8_t*              gate_open,
                                               ra8_etha_cbs_param_t* oper_param);

/**
 * @brief Programme the time-aware shaper (TAS / 802.1Qbv) gate list.
 *
 * @param[in] port Port identifier.
 * @param[in] gate_list Array of gate-control entries.
 * @param[in] entry_count Number of entries (1..256).
 * @param[in] cycle_units Total cycle-time in clock units (32-bit).
 * @param[in] start_time Absolute cycle-start time (64-bit, lo+hi).
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok TAS programmed and committed.
 * @retval k_ra8_err_null_ptr gate_list is nullptr (and entry_count > 0).
 * @retval k_ra8_err_invalid_arg port out of range or entry_count > 256.
 *
 * @pre Port is in CONFIG mode.
 * @pre PTP timer (ra8_eth_gptp) provides the time reference.
 * @post EATASCTC = cycle_units.
 * @post EATASCSTC0/1 = start_time low/high words.
 * @post EATASENC entries set; EATASGL writes feed each entry; EATASC.TASCC
 * commits the new schedule.
 *
 * @note Gate-control entries each program EATASGL0/EATASGL1 in turn.
 * @par State Machine
 * @dot
 * digraph ra8_etha_shaper_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Idle [label="Idle"];
 *   Loading [label="Loading"];
 *   Committing [label="Committing"];
 *   Active [label="Active"];
 *
 *   __start -> Idle;
 *   Idle -> Loading [label="ra8_etha_set_tas_schedule"];
 *   Loading -> Committing [label="EATASC.TASCC=1"];
 *   Committing -> Active [label="EATASRIRM.TASRR observed\\ncleared"];
 *   Active -> Idle [label="EATASC.TASE=0"];
 * }
 * @enddot
 * @see ra8_etha_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_set_tas_schedule(ra8_etha_port_t            port,
                                                  const ra8_etha_tas_gate_t* gate_list,
                                                  uint16_t                   entry_count,
                                                  uint32_t                   cycle_units,
                                                  uint64_t                   start_time);

/**
 * @brief Enable or disable the TAS scheduler (main switch).
 *
 * @param[in] port Port identifier.
 * @param[in] enable Non-zero -> set EATASC.TASE; zero -> clear.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EATASC.TASE updated.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra8_etha_init.
 * @pre TAS schedule programmed via ra8_etha_set_tas_schedule.
 * @post EATASC.TASE = enable!=0.
 * @post Other EATASC bits unchanged.
 *
 * @note When disabled, all gates are forced open.
 * @see ra8_etha_set_tas_schedule
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_enable_tas(ra8_etha_port_t port, uint8_t enable);

/**
 * @brief Read the per-port MIB error counters.
 *
 * @param[in] port Port identifier.
 * @param[out] out Snapshot of all five counter registers.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok Snapshot returned.
 * @retval k_ra8_err_null_ptr out is nullptr.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra8_etha_init.
 * @pre out is a writable pointer.
 * @post out is populated with the live counter values.
 * @post Counters in hardware are NOT cleared; use ra8_etha_clear_stats.
 *
 * @note Each counter is 16 bits and saturates rather than wrapping.
 * @see ra8_etha_clear_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_read_stats(ra8_etha_port_t port, ra8_etha_stats_t* out);

/**
 * @brief Clear the per-port MIB error counters in hardware.
 *
 * @param[in] port Port identifier.
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok All five counters zeroed.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra8_etha_init.
 * @pre Caller has snapshotted via ra8_etha_read_stats first if deltas matter.
 * @post EAUSMFSECN = EATFECN = EAFSECN = EADQOECN = EADQSECN = 0.
 * @post No state outside the five counter registers is touched.
 *
 * @note Provided so applications can reset the MIB at any time.
 * @see ra8_etha_read_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_clear_stats(ra8_etha_port_t port);

/**
 * @brief Configure the per-port security gate (EASCR).
 *
 * @param[in] port Port identifier.
 * @param[in] mask Verbatim EASCR write (bit fields MRSL/TRSL/...).
 *
 * @return::ra8_err_t Error code.
 * @retval k_ra8_ok EASCR = mask.
 * @retval k_ra8_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra8_etha_init.
 * @pre Caller composed mask using EASCR field positions.
 * @post EASCR = mask.
 * @post Subsequent secure-only register writes are gated per the mask.
 *
 * @note Used to lock the agent to TrustZone-S accesses only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_set_security(ra8_etha_port_t port, uint32_t mask);

/**
 * @brief Configure the per-port descriptor-ring sizing.
 *
 * @param[in] channel     Port identifier (0..1, mapped to ::ra8_etha_port_t).
 * @param[in] num_tx      Number of TX descriptors (1..4096).
 * @param[in] num_rx      Number of RX descriptors (1..4096).
 * @param[in] buffer_size Bytes per descriptor buffer (>= 64, <= 16383).
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Ring config captured.
 * @retval k_ra8_err_invalid_arg  Any argument out of range.
 *
 * @pre Port previously brought up via ::ra8_etha_init.
 * @pre Caller has reserved descriptor storage matching num_tx + num_rx.
 * @post Per-port stats reflect ring_tx / ring_rx / ring_buf.
 * @post Per-class TX queue depths are clamped to num_tx.
 *
 * @note Per-port; safe to call concurrently for distinct ports.
 * @see ra8_etha_get_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_descriptor_ring_init(ra8_etha_port_t channel,
                                                      uint16_t        num_tx,
                                                      uint16_t        num_rx,
                                                      uint16_t        buffer_size);

/**
 * @brief Snapshot the per-port software-maintained traffic counters.
 *
 * @param[in]  channel    Port identifier.
 * @param[out] out_stats  Destination for the snapshot.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Snapshot returned.
 * @retval k_ra8_err_null_ptr     out_stats is nullptr.
 * @retval k_ra8_err_invalid_arg  channel out of range.
 *
 * @pre Port previously brought up via ::ra8_etha_init.
 * @pre out_stats is a writable pointer.
 * @post out_stats is populated with the live per-port counters.
 *
 * @note Used by threadx_netx_tcp_echo for liveness.
 * @see ra8_etha_descriptor_ring_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_get_stats(ra8_etha_port_t        channel,
                                           ra8_etha_port_stats_t* out_stats);

/**
 * @brief Increment the per-port TX OK / TX err / RX OK / RX err / drop counters.
 *
 * @param[in] channel  Port identifier.
 * @param[in] tx_ok    Frames to add to tx_ok.
 * @param[in] tx_err   Frames to add to tx_err.
 * @param[in] rx_ok    Frames to add to rx_ok.
 * @param[in] rx_err   Frames to add to rx_err.
 * @param[in] rx_drop  Frames to add to rx_drop.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Counters updated (or saturated).
 * @retval k_ra8_err_invalid_arg  channel out of range.
 *
 * @pre Port previously brought up via ::ra8_etha_init.
 * @pre Caller serialises against concurrent ::ra8_etha_get_stats.
 * @post Each counter += the matching argument, saturating at UINT32_MAX.
 *
 * @note IRQ-context safe; performs no allocation, no locks.
 * @see ra8_etha_get_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_account_traffic(ra8_etha_port_t channel,
                                                 uint32_t        tx_ok,
                                                 uint32_t        tx_err,
                                                 uint32_t        rx_ok,
                                                 uint32_t        rx_err,
                                                 uint32_t        rx_drop);

/**
 * @brief Bring an ETHA port + its off-chip PHY up to OPERATION mode.
 *
 * @details
 * One-shot helper that wraps the per-port bring-up sequence the
 * threadx_netx_tcp_echo app needs. Steps:
 *   1. EAMC = OPERATION
 *   2. ::ra8_rmac_phy_reset(channel, phy->phy_addr)
 *   3. ::ra8_rmac_phy_set_advertise(channel, ..., phy->advertise)
 *   4. ::ra8_rmac_phy_auto_neg_start(channel, ...)
 *   5. ::ra8_rmac_phy_auto_neg_wait(channel, ..., phy->timeout_ms, out_link)
 *
 * @param[in]  channel   Port identifier.
 * @param[in]  phy       PHY bring-up parameters (must not be nullptr).
 * @param[out] out_link  Resolved link state on success.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Port + PHY up; out_link populated.
 * @retval k_ra8_err_null_ptr     phy or out_link is nullptr.
 * @retval k_ra8_err_invalid_arg  channel out of range or phy fields bad.
 * @retval k_ra8_err_hw_timeout   PHY reset / auto-neg never completed.
 *
 * @pre Port previously brought up via ::ra8_etha_init.
 * @pre RMAC port previously brought up via ::ra8_rmac_init.
 * @pre Off-chip PHY visible on the MDIO bus at phy->phy_addr.
 * @post EAMC = OPERATION.
 * @post Auto-neg complete; *out_link reflects negotiated capability.
 *
 * @see ra8_etha_descriptor_ring_init
 * @see ra8_rmac_phy_auto_neg_wait
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_etha_open(ra8_etha_port_t            channel,
                                      const ra8_etha_phy_open_t* phy,
                                      ra8_rmac_phy_link_t*       out_link);

#ifdef __cplusplus
}
#endif
