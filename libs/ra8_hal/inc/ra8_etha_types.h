/**
 * @file ra8_etha_types.h
 * @brief Per-port Ethernet Agent (ETHA) data types -- HUM Ch 32 (p 1627-1702)
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Configuration / status / statistics / descriptor structures and the
 * event-callback typedef shared by every ETHA entry point. Split out of
 * the umbrella ra8_etha.h so that header stays under the per-file line
 * budget; this is a pure move of the original declarations. The register
 * enums these structs reference (::ra8_etha_opc_t, ::ra8_etha_ops_t,
 * ::ra8_etha_port_t,...) live in ra8_etha_regs.h, and the PHY link /
 * advertise types come from ra8_rmac.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_etha_regs.h"
#include "ra8_rmac.h"

/**
 * @struct ra8_etha_config_t
 * @brief Per-port ETHA configuration knobs.
 *
 * @details
 * Passed by const-pointer so future additions never break the ABI.
 * The driver does NOT touch any per-class register from this struct;
 * call the dedicated configuration helpers (ra8_etha_set_max_frame_size,
 * ra8_etha_set_queue_depth,...) after init for that.
 */
typedef struct {
  ra8_etha_opc_t initial_mode; /**< Mode to enter after init.       */
  uint32_t       eaeie0_mask;  /**< Bits to OR into EAEIE0 on init. */
  uint32_t       eaeie1_mask;  /**< Bits to OR into EAEIE1 on init. */
  uint32_t       eaeie2_mask;  /**< Bits to OR into EAEIE2 on init. */
} ra8_etha_config_t;

/**
 * @struct ra8_etha_status_t
 * @brief Snapshot of an ETHA port's runtime state.
 *
 * @details
 * Returned by::ra8_etha_get_status. Combines the operating-mode flag
 * with the three error-IRQ status words plus the cycle-time monitor.
 */
typedef struct {
  ra8_etha_ops_t ops;       /**< Current operating-mode flag.     */
  uint32_t       eaeis0;    /**< Raw EAEIS0 (ECC + frame errors). */
  uint32_t       eaeis1;    /**< Raw EAEIS1 (CBS + TAS gate).     */
  uint32_t       eaeis2;    /**< Raw EAEIS2 (queue overflow).     */
  uint32_t       tas_cycle; /**< EATASCTM TAS cycle-time monitor. */
} ra8_etha_status_t;

/**
 * @struct ra8_etha_stats_t
 * @brief Per-port MIB counter snapshot.
 *
 * @details
 * Read by::ra8_etha_read_stats from the five 16-bit error-counter
 * registers. Counters are zeroed on hardware reset and saturate
 * (do not wrap) per the HUM. Each call reads-and-zeros via the
 * dedicated clear path on the chip so consecutive calls observe
 * deltas, not totals.
 */
typedef struct {
  uint16_t switch_min_frame_err; /**< EAUSMFSECN.USMFSEN[15:0]. */
  uint16_t tag_filter_err;       /**< EATFECN.TFEN[15:0].       */
  uint16_t frame_size_err;       /**< EAFSECN.FSEN[15:0].       */
  uint16_t queue_overflow_err;   /**< EADQOECN.DQOEN[15:0].     */
  uint16_t queue_security_err;   /**< EADQSECN.DQSEN[15:0].     */
} ra8_etha_stats_t;

/**
 * @struct ra8_etha_ring_cfg_t
 * @brief Descriptor-ring configuration for ::ra8_etha_descriptor_ring_init.
 */
typedef struct {
  uint16_t num_tx;      /**< Number of TX descriptors (1..4096).  */
  uint16_t num_rx;      /**< Number of RX descriptors (1..4096).  */
  uint16_t buffer_size; /**< Per-descriptor buffer size in bytes. */
} ra8_etha_ring_cfg_t;

/**
 * @struct ra8_etha_port_stats_t
 * @brief Per-port runtime traffic counters maintained by ra8_etha.
 *
 * @details
 * Distinct from ::ra8_etha_stats_t (which holds the five MIB error
 * counters from EAUSMFSECN/EATFECN/EAFSECN/EADQOECN/EADQSECN). This
 * struct holds the software-maintained TX/RX OK/error totals updated
 * as descriptors complete. Counters are 32-bit and saturate at
 * 0xFFFFFFFF (do not wrap).
 */
typedef struct {
  uint32_t tx_ok;    /**< Frames transmitted successfully.        */
  uint32_t tx_err;   /**< Frames that failed to transmit.         */
  uint32_t rx_ok;    /**< Frames received successfully.           */
  uint32_t rx_err;   /**< Frames received with PHY/MAC/FCS error. */
  uint32_t rx_drop;  /**< Frames dropped due to ring overflow.    */
  uint16_t ring_tx;  /**< Configured TX ring depth.               */
  uint16_t ring_rx;  /**< Configured RX ring depth.               */
  uint16_t ring_buf; /**< Configured per-descriptor buffer size.  */
  uint16_t reserved; /**< Reserved for alignment / future use.    */
} ra8_etha_port_stats_t;

/**
 * @struct ra8_etha_phy_open_t
 * @brief PHY auto-negotiation parameters for ::ra8_etha_open.
 */
typedef struct {
  uint8_t  phy_addr;   /**< MDIO address of the off-chip PHY (0..31). */
  uint16_t advertise;  /**< OR of ::ra8_rmac_phy_advert_t bits.       */
  uint32_t timeout_ms; /**< Auto-neg wait timeout (0 = internal cap). */
} ra8_etha_phy_open_t;

/**
 * @struct ra8_etha_vlan_tag_t
 * @brief 802.1Q VLAN tag descriptor (used by::ra8_etha_set_vlan_tag).
 */
typedef struct {
  uint16_t vid; /**< VLAN identifier (12 bits).       */
  uint8_t  pcp; /**< Priority code point (3 bits).    */
  uint8_t  dei; /**< Drop eligible indicator (1 bit). */
} ra8_etha_vlan_tag_t;

/**
 * @struct ra8_etha_cbs_param_t
 * @brief Credit-based shaper parameters per traffic class.
 */
typedef struct {
  uint32_t increment; /**< CIV credit-add per byte (20 bits). */
  uint32_t upper_lim; /**< CUL upper credit limit (31 bits).  */
} ra8_etha_cbs_param_t;

/**
 * @struct ra8_etha_tas_entry_t
 * @brief One TAS (802.1Qbv) RAM entry, exactly as HUM Table 32.6 defines it.
 *
 * @details
 * HUM Table 32.6 "TAS entry format" (p 1691) gives a TAS entry exactly two
 * fields: a one-bit gate state ``GS`` and a 28-bit gate time ``GT`` in
 * NANOSECONDS. There is no per-class gate vector inside an entry -- the
 * TAS RAM is partitioned per descriptor queue by ::ra8_etha_tas_queue_t,
 * so an entry's single ``GS`` bit belongs to the queue whose block it sits
 * in.
 *
 * The previous shape of this struct modelled an entry as an eight-bit
 * per-class gate vector plus a "cut-through" flag, which is the 802.1Qbv
 * textbook layout but not this silicon's (#539).
 *
 * @invariant ``gate_time_ns`` fits in 28 bits (<= 0x0FFFFFFF).
 *
 * @par Example:
 * @code
 * const ra8_etha_tas_entry_t open_then_shut[2] = {
 *   {.gate_time_ns = 125000U, .gate_open = true},
 *   {.gate_time_ns = 875000U, .gate_open = false},
 * };
 * @endcode
 *
 * @see ra8_etha_tas_queue_t
 * @see ra8_etha_set_tas_schedule
 */
typedef struct {
  uint32_t gate_time_ns; /**< TAS.GT -- entry duration in ns, 28 bits. */
  bool     gate_open;    /**< TAS.GS -- true opens this queue's gate.  */
} ra8_etha_tas_entry_t;

/**
 * @struct ra8_etha_tas_queue_t
 * @brief The gate-control list of one descriptor queue.
 *
 * @details
 * Each of the eight descriptor queues owns a contiguous block of TAS RAM
 * entries; ``EATASENCi.TASAEN`` records how many entries queue ``i`` has
 * (HUM Ch 32.3.5.3 p 1647). A queue with ``count == 0`` is left out of the
 * schedule entirely and its ``entries`` pointer is never dereferenced.
 *
 * @invariant ``entries`` is non-null whenever ``count`` is non-zero.
 * @invariant The sum of ``count`` over all queues is at most
 *            ::k_ra8_etha_tas_entries_max.
 *
 * @see ra8_etha_tas_entry_t
 * @see ra8_etha_set_tas_schedule
 */
typedef struct {
  const ra8_etha_tas_entry_t* entries; /**< Gate list, or nullptr when count is 0. */
  uint16_t                    count;   /**< Entries for this queue (EATASENCi).    */
} ra8_etha_tas_queue_t;

/**
 * @typedef ra8_etha_event_fn_t
 * @brief ETHA per-port event callback.
 *
 * @param[in] ctx Caller-supplied opaque cookie.
 * @param[in] port Port that raised the event.
 * @param[in] eaeis0 Snapshot of EAEIS0 at dispatch time.
 * @param[in] eaeis1 Snapshot of EAEIS1 at dispatch time.
 * @param[in] eaeis2 Snapshot of EAEIS2 at dispatch time.
 */
typedef void (*ra8_etha_event_fn_t)(void*           ctx,
                                    ra8_etha_port_t port,
                                    uint32_t        eaeis0,
                                    uint32_t        eaeis1,
                                    uint32_t        eaeis2);
