/**
 * @file ra_etha_ops.h
 * @brief Per-port ETHA lifecycle / IRQ / mode / queue API -- HUM Ch 32 (p 1627-1702)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Lifecycle (init / deinit / reset), status snapshot, error-IRQ block
 * management, event dispatch, low-power stop, explicit mode-change, and
 * per-traffic-class TX-queue arbitration / depth / preemption / max
 * frame-size / IPV-remap prototypes. Split out of the umbrella ra_etha.h
 * to keep that header under the per-file line budget; this is a pure move
 * of the original declarations. The data types these functions take live
 * in ra_etha_types.h, and the register enums in ra8d2_etha_regs.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8d2_etha_regs.h"
#include "ra_err.h"
#include "ra_etha_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise an ETHA port (MSTP gate + reset registers + initial mode).
 *
 * @param[in] port Port identifier (::k_ra_etha_port_0 or _1).
 * @param[in] cfg Per-port configuration. Must not be nullptr.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Port brought up in the requested mode.
 * @retval k_ra_err_null_ptr cfg is nullptr.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Caller is single-threaded with respect to this port.
 * @pre Ethernet subsystem MSTP gate may already be enabled by ra_eth.
 * @post EAMC is loaded with cfg->initial_mode.
 * @post EAEIE0/1/2 are loaded with cfg->eaeie0/1/2_mask.
 *
 * @note Per-port; safe to call concurrently for distinct ports.
 * @see ra_etha_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_init(ra_etha_port_t port, const ra_etha_config_t* cfg);

/**
 * @brief Tear down an ETHA port (drop into RESET, mask all IRQs).
 *
 * @param[in] port Port identifier.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Port returned to RESET mode.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre No outstanding DMA on this port.
 * @post EAMC =::k_ra_etha_opc_reset.
 * @post EAEIE0 = EAEIE1 = EAEIE2 = 0.
 *
 * @note See ra_eth_deinit for the shared MSTP gate.
 * @see ra_etha_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_deinit(ra_etha_port_t port);

/**
 * @brief Read a port's status snapshot (mode + 3x error IRQ status + TAS).
 *
 * @param[in] port Port identifier.
 * @param[out] out Destination for the status snapshot.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Snapshot populated.
 * @retval k_ra_err_null_ptr out is nullptr.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port has been brought up via ra_etha_init.
 * @pre out is a writable pointer.
 * @post out fields reflect the live registers at the call site.
 * @post No register state is mutated.
 *
 * @note Reads are non-destructive; status bits are cleared via
 *::ra_etha_clear_status.
 * @see ra_etha_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_get_status(ra_etha_port_t port, ra_etha_status_t* out);

/**
 * @brief Clear bits in EAEIS0 / EAEIS1 / EAEIS2 via EAEIDx.
 *
 * @param[in] port Port identifier.
 * @param[in] block Which error-interrupt block (0/1/2).
 * @param[in] mask Bit mask to write to EAEIDx.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Bits cleared.
 * @retval k_ra_err_invalid_arg port or block out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre Caller has read the corresponding EAEISx via::ra_etha_get_status.
 * @post Bits in mask are cleared in EAEISx.
 * @post EAEISx read-back excludes the cleared bits.
 *
 * @note IRQ-safe; the underlying write is atomic on Cortex-M85.
 * @see ra_etha_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_clear_status(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask);

/**
 * @brief Atomically enable specific error IRQ bits in one of the three blocks.
 *
 * @param[in] port Port identifier.
 * @param[in] block Which IRQ block.
 * @param[in] mask Bits to OR into the corresponding EAEIEx.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok IRQ bits enabled.
 * @retval k_ra_err_invalid_arg port or block out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre IRQ at the NVIC is disabled while this is called if reentrancy is undesirable.
 * @post Bits in mask are set in EAEIEx.
 * @post Other bits in EAEIEx are unchanged.
 *
 * @note Hardware uses a separate "set" register so this is atomic.
 * @see ra_etha_disable_irq
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_enable_irq(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask);

/**
 * @brief Atomically disable specific error IRQ bits in one of the three blocks.
 *
 * @param[in] port Port identifier.
 * @param[in] block Which IRQ block.
 * @param[in] mask Bits to OR into the corresponding EAEIDx.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok IRQ bits disabled.
 * @retval k_ra_err_invalid_arg port or block out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre IRQ at the NVIC is masked or this caller can tolerate a missed event.
 * @post Bits in mask are cleared in EAEIEx.
 * @post Other bits in EAEIEx are unchanged.
 *
 * @note Hardware uses a separate "clear" register so this is atomic.
 * @see ra_etha_enable_irq
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_disable_irq(ra_etha_port_t port, ra_etha_irq_class_t block, uint32_t mask);

/**
 * @brief Attach a per-port event handler.
 *
 * @param[in] port Port identifier.
 * @param[in] cb Callback to fire from::ra_etha_dispatch (may be nullptr to detach).
 * @param[in] ctx Opaque cookie passed back to cb.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Handler attached / detached.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre Caller owns the lifetime of ctx.
 * @post Subsequent dispatches for this port invoke cb (or nothing if cb == nullptr).
 * @post Previous handler for this port is no longer reachable.
 *
 * @note Not thread-safe; install handlers during init / before IRQ unmask.
 * @see ra_etha_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_attach_handler(ra_etha_port_t port, ra_etha_event_fn_t cb, void* ctx);

/**
 * @brief Snapshot all three EAEISx, clear them, then fire the per-port handler.
 *
 * @details
 * Reads the three Ethernet-A error/event status registers EAEIS0/1/2
 * (HUM Ch 32 "ETHA error / event status registers EAEISn", p ~1408)
 * for the given port, atomically writes back the captured bits to
 * acknowledge them, then invokes the handler installed via
 * ``ra_etha_attach_handler()`` with the snapshot mask. Out-of-range
 * ports and missing handlers are silently ignored.
 *
 * @param[in] port Port identifier. Out-of-range silently ignored.
 *
 * @pre Port previously brought up via ra_etha_init for the call to do work.
 * @pre Caller has masked the ETHA IRQ at the NVIC if reentrancy is undesirable.
 * @post All three EAEISx are cleared (matching what was passed to the handler).
 * @post Per-port callback (if any) has been invoked exactly once.
 *
 * @note IRQ-context safe; performs no allocation, no locks.
 * @see ra_etha_attach_handler
 * @since 0.1.0
 */
void ra_etha_dispatch(ra_etha_port_t port);

/**
 * @brief Drop an ETHA port into low-power stop (DISABLE mode).
 *
 * @param[in] port Port identifier.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Port placed in DISABLE mode.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre No outstanding DMA on this port.
 * @post EAMC =::k_ra_etha_opc_disable.
 * @post Subsequent get_status reports OPS =::k_ra_etha_ops_disable
 * (assuming hardware ack is observed).
 *
 * @note The shared ESWM MSTP gate is NOT dropped here.
 * @see ra_etha_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_enter_stop(ra_etha_port_t port);

/**
 * @brief Bring an ETHA port back from low-power stop (OPERATION mode).
 *
 * @param[in] port Port identifier.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Port returned to OPERATION mode.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously placed in stop via ra_etha_enter_stop.
 * @pre Ethernet subsystem MSTP gate is live (ra_eth_init was called).
 * @post EAMC =::k_ra_etha_opc_operation.
 * @post Subsequent get_status reports OPS =::k_ra_etha_ops_operation
 * once the hardware ack is observed.
 *
 * @see ra_etha_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_exit_stop(ra_etha_port_t port);

/**
 * @brief Software-reset the port: drop to RESET, then re-enter CONFIG.
 *
 * @param[in] port Port identifier.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Reset sequence issued.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre Caller is prepared to re-program per-class state after the reset.
 * @post EAMC traversed RESET -> CONFIG.
 * @post All transient queue / counter state is cleared by hardware.
 *
 * @note Used by error-recovery callers; does not touch the MSTP gate.
 * @see ra_etha_set_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_reset(ra_etha_port_t port);

/**
 * @brief Issue an explicit EAMC mode-change command.
 *
 * @param[in] port Port identifier.
 * @param[in] mode Target mode (one of::ra_etha_opc_t).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Command written.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre mode is a legal transition from the current state per HUM Ch 32.2.
 * @post EAMC = mode.
 * @post Caller must poll get_status until OPS == ops-of-mode for ack.
 *
 * @note State machine: RESET -> CONFIG -> OPERATION; OPERATION -> DISABLE.
 * @par State Machine
 * @startuml
 * [*] --> RESET
 * RESET --> CONFIG: EAMC.OPC=2
 * CONFIG --> OPERATION: EAMC.OPC=3
 * OPERATION --> DISABLE: EAMC.OPC=1
 * DISABLE --> OPERATION: EAMC.OPC=3
 * CONFIG --> RESET: EAMC.OPC=0
 * OPERATION --> RESET: EAMC.OPC=0
 * DISABLE --> RESET: EAMC.OPC=0
 * @enduml
 * @see ra_etha_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_mode(ra_etha_port_t port, ra_etha_opc_t mode);

/**
 * @brief Configure the per-traffic-class TX queue arbitration weight.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[in] arb Arbitration weight (4-bit field, 0=lowest, 15=highest).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Field updated.
 * @retval k_ra_err_invalid_arg port, tc, or arb out of range.
 *
 * @pre Port is in CONFIG mode (ra_etha_set_mode k_ra_etha_opc_config).
 * @pre arb fits in 4 bits.
 * @post EATDQAC.TDQAtc = arb.
 * @post Other class weights are unchanged.
 *
 * @note Only takes effect after a transition back to OPERATION.
 * @see ra_etha_set_queue_depth
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_queue_arb(ra_etha_port_t port, ra_etha_tc_t tc, uint8_t arb);

/**
 * @brief Set the TX descriptor queue depth for one traffic class.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[in] depth Queue depth in descriptors (11-bit field, 1..2047).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EATDQDC[tc] updated.
 * @retval k_ra_err_invalid_arg port, tc, or depth out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre depth <= 2047.
 * @post EATDQDC[tc].DQD = depth.
 * @post Other classes unchanged.
 *
 * @note Backs the per-traffic-class descriptor ring.
 * @see ra_etha_get_queue_level
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_set_queue_depth(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t depth);

/**
 * @brief Read the current and high-water-mark queue level for one class.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[out] cur_level Current pending-descriptor count (DNQ).
 * @param[out] peak Peak pending-descriptor count since last clear (DMLQ).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr cur_level or peak is nullptr.
 * @retval k_ra_err_invalid_arg port or tc out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre cur_level and peak are writable pointers.
 * @post *cur_level = EATDQM[tc].DNQ.
 * @post *peak = EATDQMLM[tc].DMLQ.
 *
 * @note Read-only; does not clear the high-water mark.
 * @see ra_etha_set_queue_depth
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_get_queue_level(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t* cur_level, uint16_t* peak);

/**
 * @brief Configure 802.3br TX preemption: which classes are pre-emptable.
 *
 * @param[in] port Port identifier.
 * @param[in] preempt Bit-mask of pre-emptable classes (bit q -> class q).
 * @param[in] cut_thru Non-zero -> set TTQ8 (cut-through path is pre-emptable).
 * @param[in] afs Additional fragment size policy (::ra_etha_afs_t).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EATPEC updated.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre afs is a valid::ra_etha_afs_t.
 * @post EATPEC.TTQ7..TTQ0 = preempt&0xFF; TTQ8 = cut_thru!=0; AFS = afs.
 * @post 802.3br express/preemptable selector is live for next frame.
 *
 * @note 802.3br requires both ends to support preamble preemption.
 * @see ra_etha_set_max_frame_size
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_set_preemption(ra_etha_port_t port, uint8_t preempt, uint8_t cut_thru, ra_etha_afs_t afs);

/**
 * @brief Set the maximum frame size for one traffic class (jumbo OK).
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[in] max_bytes Frame size limit in bytes (16-bit, 64..16383 typical).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EATMFSC[tc] updated.
 * @retval k_ra_err_invalid_arg port or tc out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre max_bytes >= 64 to satisfy IEEE 802.3 minimum.
 * @post EATMFSC[tc].MFS = max_bytes.
 * @post Frames larger than max_bytes are counted in EAFSECN.
 *
 * @note Jumbo frames (>1500) supported up to a 16-bit limit.
 * @see ra_etha_read_stats
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_set_max_frame_size(ra_etha_port_t port, ra_etha_tc_t tc, uint16_t max_bytes);

/**
 * @brief Programme the IPV (PCP-to-internal-priority) remap table.
 *
 * @param[in] port Port identifier.
 * @param[in] map 8-entry array, each value 0..7. ipv_map[i] is the
 * internal priority for incoming PCP=i.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EAIRC packed and written.
 * @retval k_ra_err_null_ptr map is nullptr.
 * @retval k_ra_err_invalid_arg port out of range or any map[i] > 7.
 *
 * @pre Port is in CONFIG mode.
 * @pre map points to 8 valid entries.
 * @post EAIRC packs eight 3-bit fields, IPVR0..IPVR7, from map[].
 * @post Out-of-range values are rejected without partial writes.
 *
 * @note Used to map 802.1p PCP -> RA8D2 internal traffic class.
 * @see ra_etha_set_queue_arb
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_ipv_remap(ra_etha_port_t port, const uint8_t* map);

#ifdef __cplusplus
}
#endif
