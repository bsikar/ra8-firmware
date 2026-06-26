/**
 * @file ra_etha_types.h
 * @brief Per-port Ethernet Agent (ETHA) data types -- HUM Ch 32 (p 1627-1702)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Configuration / status / statistics / descriptor structures and the
 * event-callback typedef shared by every ETHA entry point. Split out of
 * the umbrella ra_etha.h so that header stays under the per-file line
 * budget; this is a pure move of the original declarations. The register
 * enums these structs reference (::ra_etha_opc_t, ::ra_etha_ops_t,
 * ::ra_etha_port_t,...) live in ra8d2_etha_regs.h, and the PHY link /
 * advertise types come from ra_rmac.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8d2_etha_regs.h"
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
