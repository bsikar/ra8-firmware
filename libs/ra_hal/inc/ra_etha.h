/**
 * @file ra_etha.h
 * @brief Per-port Ethernet Agent (ETHA) driver -- HUM Ch 32 (p 1627-1702)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 Ethernet Agent (ETHA) block. The
 * Ethernet pipeline on the RA8D2 is a small stack:
 *
 * @verbatim
 * +-----------------------+ top +-------------------------+
 * | ESWM (Ch 29) | ------> | Switch fabric |
 * | ra_eth.c | | |
 * +-----------------------+ +-------------------------+
 * | per-port descriptor / forwarding bus
 * v
 * +-----------------------+ ETHA +-------------------------+
 * | ETHA (Ch 32, here) | <-----> | per-port queue / FIFO |
 * | ra_etha.c (one per m) | | preemption, IPV remap |
 * +-----------------------+ +-------------------------+
 * | per-port MAC bus
 * v
 * +-----------------------+ RMAC +-------------------------+
 * | RMAC (Ch 33) | <-----> | MAC + PHY MDIO |
 * | ra_rmac.c | | |
 * +-----------------------+ +-------------------------+
 * | MII / GMII pins
 * v bottom
 * Off-chip PHY (LAN8740, KSZ9031,...)
 * @endverbatim
 *
 * Round-3 brings the driver to **full HUM Ch 32 coverage**:
 *
 * - 8 traffic-class TX queues (EATDQAC, EATDQDC, EATDQM, EATDQMLM)
 * - TX preemption (802.3br) via EATPEC
 * - Per-class max frame size programming (EATMFSCq), jumbo-frame OK
 * - IPV remap table (EAIRC) -- 8 PCP-to-internal-priority entries
 * - VLAN tag insertion (EAVCC.VIM + EAVTC C-/S-tags)
 * - VLAN tag stripping (EAVCC.VEM modes)
 * - RX tag filter (EARTFC) -- multicast group filter
 * - Cut-through queue (EACTQC + EACTDQDC) for low-latency forwarding
 * - Credit-based shaper (CBS) admin / oper registers (EACAEC, EACAIVCq,
 * EACAULCq, EACOEM, EACOIVMq, EACOULMq, EACGSM)
 * - Time-aware shaper (TAS / 802.1Qbv) gate-control programming
 * - Per-port full statistics (EAUSMFSECN, EATFECN, EAFSECN, EADQOECN,
 * EADQSECN) and MIB counter snapshot
 * - All three error-IRQ blocks (EAEIS0/E/D0, EAEIS1/E/D1, EAEIS2/E/D2)
 * -- TX-done, RX-frame, error and link-change events
 * - DMA-descriptor-ring management hooks for TX (queue-depth + monitor)
 * - Security configuration (EASCR) gating MRSL / TRSL / TGRSL etc.
 *
 * Per-port state is kept in a small fixed-size table; both port
 * instances (m = 0, 1) share this driver via the::ra_etha_port_t
 * argument. The shared MSTP gate ``k_ra_mstp_eswm`` is reference-
 * counted by ra_mstp, so init/deinit interleave safely with the
 * other ethernet sub-drivers (ra_eth, ra_eth_mfwd, ra_eth_coma,
 * ra_eth_gwca, ra_eth_gptp, ra_rmac).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_etha_regs.h"
#include "ra_err.h"
#include "ra_rmac.h"

/**
 * @struct ra_etha_config_t
 * @brief Per-port ETHA configuration knobs.
 *
 * @details
 * Passed by const-pointer so future additions never break the ABI.
 * The driver does NOT touch any per-class register from this struct;
 * call the dedicated configuration helpers (ra_etha_set_max_frame_size,
 * ra_etha_set_queue_depth,...) after init for that.
 */
typedef struct {
  ra_etha_opc_t initial_mode; /**< Mode to enter after init. */
  uint32_t      eaeie0_mask;  /**< Bits to OR into EAEIE0 on init. */
  uint32_t      eaeie1_mask;  /**< Bits to OR into EAEIE1 on init. */
  uint32_t      eaeie2_mask;  /**< Bits to OR into EAEIE2 on init. */
} ra_etha_config_t;

/**
 * @struct ra_etha_status_t
 * @brief Snapshot of an ETHA port's runtime state.
 *
 * @details
 * Returned by::ra_etha_get_status. Combines the operating-mode flag
 * with the three error-IRQ status words plus the cycle-time monitor.
 */
typedef struct {
  ra_etha_ops_t ops;       /**< Current operating-mode flag. */
  uint32_t      eaeis0;    /**< Raw EAEIS0 (ECC + frame errors). */
  uint32_t      eaeis1;    /**< Raw EAEIS1 (CBS + TAS gate). */
  uint32_t      eaeis2;    /**< Raw EAEIS2 (queue overflow). */
  uint32_t      tas_cycle; /**< EATASCTM TAS cycle-time monitor. */
} ra_etha_status_t;

/**
 * @struct ra_etha_stats_t
 * @brief Per-port MIB counter snapshot.
 *
 * @details
 * Read by::ra_etha_read_stats from the five 16-bit error-counter
 * registers. Counters are zeroed on hardware reset and saturate
 * (do not wrap) per the HUM. Each call reads-and-zeros via the
 * dedicated clear path on the chip so consecutive calls observe
 * deltas, not totals.
 */
typedef struct {
  uint16_t switch_min_frame_err; /**< EAUSMFSECN.USMFSEN[15:0]. */
  uint16_t tag_filter_err;       /**< EATFECN.TFEN[15:0]. */
  uint16_t frame_size_err;       /**< EAFSECN.FSEN[15:0]. */
  uint16_t queue_overflow_err;   /**< EADQOECN.DQOEN[15:0]. */
  uint16_t queue_security_err;   /**< EADQSECN.DQSEN[15:0]. */
} ra_etha_stats_t;

/**
 * @struct ra_etha_ring_cfg_t
 * @brief Descriptor-ring configuration for ::ra_etha_descriptor_ring_init.
 */
typedef struct {
  uint16_t num_tx;      /**< Number of TX descriptors (1..4096).      */
  uint16_t num_rx;      /**< Number of RX descriptors (1..4096).      */
  uint16_t buffer_size; /**< Per-descriptor buffer size in bytes.     */
} ra_etha_ring_cfg_t;

/**
 * @struct ra_etha_port_stats_t
 * @brief Per-port runtime traffic counters maintained by ra_etha.
 *
 * @details
 * Distinct from ::ra_etha_stats_t (which holds the five MIB error
 * counters from EAUSMFSECN/EATFECN/EAFSECN/EADQOECN/EADQSECN). This
 * struct holds the software-maintained TX/RX OK/error totals updated
 * as descriptors complete. Counters are 32-bit and saturate at
 * 0xFFFFFFFF (do not wrap).
 */
typedef struct {
  uint32_t tx_ok;    /**< Frames transmitted successfully.            */
  uint32_t tx_err;   /**< Frames that failed to transmit.             */
  uint32_t rx_ok;    /**< Frames received successfully.               */
  uint32_t rx_err;   /**< Frames received with PHY/MAC/FCS error.     */
  uint32_t rx_drop;  /**< Frames dropped due to ring overflow.        */
  uint16_t ring_tx;  /**< Configured TX ring depth.                   */
  uint16_t ring_rx;  /**< Configured RX ring depth.                   */
  uint16_t ring_buf; /**< Configured per-descriptor buffer size.      */
  uint16_t reserved; /**< Reserved for alignment / future use.        */
} ra_etha_port_stats_t;

/**
 * @struct ra_etha_phy_open_t
 * @brief PHY auto-negotiation parameters for ::ra_etha_open.
 */
typedef struct {
  uint8_t  phy_addr;   /**< MDIO address of the off-chip PHY (0..31). */
  uint16_t advertise;  /**< OR of ::ra_rmac_phy_advert_t bits.        */
  uint32_t timeout_ms; /**< Auto-neg wait timeout (0 = internal cap). */
} ra_etha_phy_open_t;

/**
 * @struct ra_etha_vlan_tag_t
 * @brief 802.1Q VLAN tag descriptor (used by::ra_etha_set_vlan_tag).
 */
typedef struct {
  uint16_t vid; /**< VLAN identifier (12 bits). */
  uint8_t  pcp; /**< Priority code point (3 bits). */
  uint8_t  dei; /**< Drop eligible indicator (1 bit). */
} ra_etha_vlan_tag_t;

/**
 * @struct ra_etha_cbs_param_t
 * @brief Credit-based shaper parameters per traffic class.
 */
typedef struct {
  uint32_t increment; /**< CIV credit-add per byte (20 bits). */
  uint32_t upper_lim; /**< CUL upper credit limit (31 bits). */
} ra_etha_cbs_param_t;

/**
 * @struct ra_etha_tas_gate_t
 * @brief TAS (802.1Qbv) gate control list entry.
 *
 * @details
 * One entry programs ``time_units`` of bus time during which the
 * eight per-class gates take the bit pattern ``gate_state``. Bit 0
 * of ``gate_state`` is class 0; bit 7 is class 7; bit 8 is the
 * cut-through gate (TASCTGS). One entry maps to a write to
 * EATASGL0 + EATASGL1 followed by a learn pulse.
 */
typedef struct {
  uint8_t  gate_state;  /**< 8-bit gate vector + cut-through bit (bit 8). */
  uint32_t time_units;  /**< Duration of this entry (28 bits). */
  uint8_t  cut_through; /**< Non-zero -> set TASGSL bit. */
} ra_etha_tas_gate_t;

/**
 * @typedef ra_etha_event_fn_t
 * @brief ETHA per-port event callback.
 *
 * @param[in] ctx Caller-supplied opaque cookie.
 * @param[in] port Port that raised the event.
 * @param[in] eaeis0 Snapshot of EAEIS0 at dispatch time.
 * @param[in] eaeis1 Snapshot of EAEIS1 at dispatch time.
 * @param[in] eaeis2 Snapshot of EAEIS2 at dispatch time.
 */
typedef void (*ra_etha_event_fn_t)(void*          ctx,
                                   ra_etha_port_t port,
                                   uint32_t       eaeis0,
                                   uint32_t       eaeis1,
                                   uint32_t       eaeis2);

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
 * @return None.
 * @retval None Function returns ``void``; out-of-range port is silently ignored.
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

/**
 * @brief Configure VLAN tag insertion / extraction for the port.
 *
 * @param[in] port Port identifier.
 * @param[in] vim Insertion mode (::ra_etha_vim_t).
 * @param[in] vem Extraction mode (::ra_etha_vem_t).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EAVCC updated.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre vem fits in 3 bits.
 * @post EAVCC.VIM = vim and EAVCC.VEM = vem.
 * @post Subsequent TX frames are tagged or stripped per the new mode.
 *
 * @note Tag values to insert come from::ra_etha_set_vlan_tag.
 * @see ra_etha_set_vlan_tag
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_set_vlan_mode(ra_etha_port_t port, ra_etha_vim_t vim, ra_etha_vem_t vem);

/**
 * @brief Programme the C-VLAN and S-VLAN tag values used on TX insertion.
 *
 * @param[in] port Port identifier.
 * @param[in] c_tag C-VLAN tag (inner). Pointer to descriptor.
 * @param[in] s_tag S-VLAN tag (outer). Pointer to descriptor.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EAVTC packed and written.
 * @retval k_ra_err_null_ptr c_tag or s_tag is nullptr.
 * @retval k_ra_err_invalid_arg port out of range or vid/pcp out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre c_tag->vid <= 0xFFF and c_tag->pcp <= 7 (same for s_tag).
 * @post EAVTC has both tags packed: CTV/CTP/CTD then STV/STP/STD.
 * @post No partial write on validation failure.
 *
 * @note Only effective when EAVCC.VIM =::k_ra_etha_vim_enabled.
 * @see ra_etha_set_vlan_mode
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_vlan_tag(ra_etha_port_t            port,
                                            const ra_etha_vlan_tag_t* c_tag,
                                            const ra_etha_vlan_tag_t* s_tag);

/**
 * @brief Configure the RX tag filter (multicast group + VLAN-tag filter).
 *
 * @param[in] port Port identifier.
 * @param[in] mask Bit mask written verbatim to EARTFC. Bit positions
 * follow HUM Ch 32 EARTFC: NT, RT, CST, CSRT, CT,
 * CRT, SCT, SCRT, UT.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EARTFC = mask.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre Caller has used the EARTFC bit names (NT/RT/CST/...) when building mask.
 * @post EARTFC = mask & 0x1FF.
 * @post Frames matching the active filter survive; others are dropped.
 *
 * @note This is the multicast group filter for tagged traffic.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_rx_tag_filter(ra_etha_port_t port, uint32_t mask);

/**
 * @brief Configure the cut-through TX queue (low-latency forwarding).
 *
 * @param[in] port Port identifier.
 * @param[in] qd 16-bit cut-through queue depth in bytes (CTQD).
 * @param[in] dqd 4-bit cut-through descriptor queue depth (CTDQD).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EACTQC + EACTDQDC updated.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre dqd <= 15.
 * @post EACTQC.CTQD = qd; EACTDQDC.CTDQD = dqd.
 * @post Cut-through path may begin forwarding short frames sooner.
 *
 * @note CTDQD = 0 effectively disables the cut-through path.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_configure_cut_through(ra_etha_port_t port, uint16_t qd, uint8_t dqd);

/**
 * @brief Configure the credit-based shaper (CBS) for one traffic class.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[in] enable Non-zero -> set EACAEC.CEtc and EACC.CCtc.
 * @param[in] param Pointer to CBS parameters (increment + upper-limit).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok CBS programmed.
 * @retval k_ra_err_null_ptr param is nullptr (when enable != 0).
 * @retval k_ra_err_invalid_arg port, tc, or values out of range.
 *
 * @pre Port is in CONFIG mode.
 * @pre param->increment fits in 20 bits, upper_lim in 31 bits.
 * @post EACAIVC[tc] = increment; EACAULC[tc] = upper_lim.
 * @post EACAEC.CEtc = enable!=0; EACC.CCtc = enable!=0.
 *
 * @note CBS is the AVB credit shaper (802.1Qav).
 * @see ra_etha_get_cbs_state
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_configure_cbs(ra_etha_port_t             port,
                                             ra_etha_tc_t               tc,
                                             uint8_t                    enable,
                                             const ra_etha_cbs_param_t* param);

/**
 * @brief Read the operational CBS gate-state vector + per-class oper params.
 *
 * @param[in] port Port identifier.
 * @param[in] tc Traffic class (0..7).
 * @param[out] enabled Non-zero if oper-enable mirror bit is set.
 * @param[out] gate_open Non-zero if EACGSM gate-state bit is asserted.
 * @param[out] oper_param Live oper-side increment + upper-limit values.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr any out pointer is nullptr.
 * @retval k_ra_err_invalid_arg port or tc out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre All output pointers are writable.
 * @post *enabled reflects EACOEM.CEtc.
 * @post *gate_open reflects EACGSM.CGStc.
 *
 * @note The oper-side mirror lags the admin-side until next CBS update.
 * @see ra_etha_configure_cbs
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_get_cbs_state(ra_etha_port_t       port,
                                             ra_etha_tc_t         tc,
                                             uint8_t*             enabled,
                                             uint8_t*             gate_open,
                                             ra_etha_cbs_param_t* oper_param);

/**
 * @brief Programme the time-aware shaper (TAS / 802.1Qbv) gate list.
 *
 * @param[in] port Port identifier.
 * @param[in] gate_list Array of gate-control entries.
 * @param[in] entry_count Number of entries (1..256).
 * @param[in] cycle_units Total cycle-time in clock units (32-bit).
 * @param[in] start_time Absolute cycle-start time (64-bit, lo+hi).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok TAS programmed and committed.
 * @retval k_ra_err_null_ptr gate_list is nullptr (and entry_count > 0).
 * @retval k_ra_err_invalid_arg port out of range or entry_count > 256.
 *
 * @pre Port is in CONFIG mode.
 * @pre PTP timer (ra_eth_gptp) provides the time reference.
 * @post EATASCTC = cycle_units.
 * @post EATASCSTC0/1 = start_time low/high words.
 * @post EATASENC entries set; EATASGL writes feed each entry; EATASC.TASCC
 * commits the new schedule.
 *
 * @note Gate-control entries each program EATASGL0/EATASGL1 in turn.
 * @par State Machine
 * @startuml
 * [*] --> Idle
 * Idle --> Loading: ra_etha_set_tas_schedule
 * Loading --> Committing: EATASC.TASCC=1
 * Committing --> Active: EATASRIRM.TASRR observed cleared
 * Active --> Idle: EATASC.TASE=0
 * @enduml
 * @see ra_etha_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_tas_schedule(ra_etha_port_t            port,
                                                const ra_etha_tas_gate_t* gate_list,
                                                uint16_t                  entry_count,
                                                uint32_t                  cycle_units,
                                                uint64_t                  start_time);

/**
 * @brief Enable or disable the TAS scheduler (master switch).
 *
 * @param[in] port Port identifier.
 * @param[in] enable Non-zero -> set EATASC.TASE; zero -> clear.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EATASC.TASE updated.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre TAS schedule programmed via ra_etha_set_tas_schedule.
 * @post EATASC.TASE = enable!=0.
 * @post Other EATASC bits unchanged.
 *
 * @note When disabled, all gates are forced open.
 * @see ra_etha_set_tas_schedule
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_enable_tas(ra_etha_port_t port, uint8_t enable);

/**
 * @brief Read the per-port MIB error counters.
 *
 * @param[in] port Port identifier.
 * @param[out] out Snapshot of all five counter registers.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok Snapshot returned.
 * @retval k_ra_err_null_ptr out is nullptr.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre out is a writable pointer.
 * @post out is populated with the live counter values.
 * @post Counters in hardware are NOT cleared; use ra_etha_clear_stats.
 *
 * @note Each counter is 16 bits and saturates rather than wrapping.
 * @see ra_etha_clear_stats
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_read_stats(ra_etha_port_t port, ra_etha_stats_t* out);

/**
 * @brief Clear the per-port MIB error counters in hardware.
 *
 * @param[in] port Port identifier.
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok All five counters zeroed.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre Caller has snapshotted via ra_etha_read_stats first if deltas matter.
 * @post EAUSMFSECN = EATFECN = EAFSECN = EADQOECN = EADQSECN = 0.
 * @post No state outside the five counter registers is touched.
 *
 * @note Provided so applications can reset the MIB at any time.
 * @see ra_etha_read_stats
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_clear_stats(ra_etha_port_t port);

/**
 * @brief Configure the per-port security gate (EASCR).
 *
 * @param[in] port Port identifier.
 * @param[in] mask Verbatim EASCR write (bit fields MRSL/TRSL/...).
 *
 * @return::ra_err_t Error code.
 * @retval k_ra_ok EASCR = mask.
 * @retval k_ra_err_invalid_arg port out of range.
 *
 * @pre Port previously brought up via ra_etha_init.
 * @pre Caller composed mask using EASCR field positions.
 * @post EASCR = mask.
 * @post Subsequent secure-only register writes are gated per the mask.
 *
 * @note Used to lock the agent to TrustZone-S accesses only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_set_security(ra_etha_port_t port, uint32_t mask);

/**
 * @brief Configure the per-port descriptor-ring sizing.
 *
 * @param[in] channel     Port identifier (0..1, mapped to ::ra_etha_port_t).
 * @param[in] num_tx      Number of TX descriptors (1..4096).
 * @param[in] num_rx      Number of RX descriptors (1..4096).
 * @param[in] buffer_size Bytes per descriptor buffer (>= 64, <= 16383).
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Ring config captured.
 * @retval k_ra_err_invalid_arg  Any argument out of range.
 *
 * @pre Port previously brought up via ::ra_etha_init.
 * @pre Caller has reserved descriptor storage matching num_tx + num_rx.
 * @post Per-port stats reflect ring_tx / ring_rx / ring_buf.
 * @post Per-class TX queue depths are clamped to num_tx.
 *
 * @note Per-port; safe to call concurrently for distinct ports.
 * @see ra_etha_get_stats
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_descriptor_ring_init(ra_etha_port_t channel,
                                                    uint16_t       num_tx,
                                                    uint16_t       num_rx,
                                                    uint16_t       buffer_size);

/**
 * @brief Snapshot the per-port software-maintained traffic counters.
 *
 * @param[in]  channel    Port identifier.
 * @param[out] out_stats  Destination for the snapshot.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Snapshot returned.
 * @retval k_ra_err_null_ptr     out_stats is nullptr.
 * @retval k_ra_err_invalid_arg  channel out of range.
 *
 * @pre Port previously brought up via ::ra_etha_init.
 * @pre out_stats is a writable pointer.
 * @post out_stats is populated with the live per-port counters.
 *
 * @note Used by ethernet_tcp_echo / threadx_netx_tcp_echo for liveness.
 * @see ra_etha_descriptor_ring_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_get_stats(ra_etha_port_t channel, ra_etha_port_stats_t* out_stats);

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
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Counters updated (or saturated).
 * @retval k_ra_err_invalid_arg  channel out of range.
 *
 * @pre Port previously brought up via ::ra_etha_init.
 * @pre Caller serialises against concurrent ::ra_etha_get_stats.
 * @post Each counter += the matching argument, saturating at UINT32_MAX.
 *
 * @note IRQ-context safe; performs no allocation, no locks.
 * @see ra_etha_get_stats
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_account_traffic(ra_etha_port_t channel,
                                               uint32_t       tx_ok,
                                               uint32_t       tx_err,
                                               uint32_t       rx_ok,
                                               uint32_t       rx_err,
                                               uint32_t       rx_drop);

/**
 * @brief Bring an ETHA port + its off-chip PHY up to OPERATION mode.
 *
 * @details
 * One-shot helper that wraps the per-port bring-up sequence the
 * ethernet_tcp_echo and threadx_netx_tcp_echo apps need. Steps:
 *   1. EAMC = OPERATION
 *   2. ::ra_rmac_phy_reset(channel, phy->phy_addr)
 *   3. ::ra_rmac_phy_set_advertise(channel, ..., phy->advertise)
 *   4. ::ra_rmac_phy_auto_neg_start(channel, ...)
 *   5. ::ra_rmac_phy_auto_neg_wait(channel, ..., phy->timeout_ms, out_link)
 *
 * @param[in]  channel   Port identifier.
 * @param[in]  phy       PHY bring-up parameters (must not be nullptr).
 * @param[out] out_link  Resolved link state on success.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Port + PHY up; out_link populated.
 * @retval k_ra_err_null_ptr     phy or out_link is nullptr.
 * @retval k_ra_err_invalid_arg  channel out of range or phy fields bad.
 * @retval k_ra_err_hw_timeout   PHY reset / auto-neg never completed.
 *
 * @pre Port previously brought up via ::ra_etha_init.
 * @pre RMAC port previously brought up via ::ra_rmac_init.
 * @pre Off-chip PHY visible on the MDIO bus at phy->phy_addr.
 * @post EAMC = OPERATION.
 * @post Auto-neg complete; *out_link reflects negotiated capability.
 *
 * @see ra_etha_descriptor_ring_init
 * @see ra_rmac_phy_auto_neg_wait
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_etha_open(ra_etha_port_t channel, const ra_etha_phy_open_t* phy, ra_rmac_phy_link_t* out_link);

#ifdef RA_SIMULATOR_MODE
/**
 * @brief Inject a raw RX frame for fuzz / unit-test ingestion.
 *
 * @details
 * Test-only veneer compiled when ``RA_SIMULATOR_MODE`` is defined.
 * Performs the minimum sanity check the descriptor ISR would do
 * (length >= Ethernet header) and decodes the destination MAC,
 * source MAC, and EtherType into the supplied output struct. Returns
 * an error if the frame is malformed. Used by the libFuzzer harness
 * in ``tests/fuzz/fuzz_ra_etha.c``.
 *
 * @param[in]  frame      Raw bytes (untrusted).
 * @param[in]  frame_len  Length in bytes of @p frame.
 * @param[out] out_dst    6-byte destination MAC, populated on success.
 * @param[out] out_src    6-byte source MAC, populated on success.
 * @param[out] out_etype  16-bit big-endian EtherType, populated on success.
 *
 * @retval k_ra_ok               Frame parsed.
 * @retval k_ra_err_null_ptr     Any required output is NULL.
 * @retval k_ra_err_invalid_arg  Frame too short to be a legal Ethernet frame.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_etha_test_inject_rx(const uint8_t* frame,
                                              uint32_t       frame_len,
                                              uint8_t        out_dst[6],
                                              uint8_t        out_src[6],
                                              uint16_t*      out_etype);
#endif /* RA_SIMULATOR_MODE */

#ifdef __cplusplus
}
#endif
