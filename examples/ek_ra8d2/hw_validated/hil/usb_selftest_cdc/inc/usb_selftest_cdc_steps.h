/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc/inc/usb_selftest_cdc_steps.h
 * @brief Host-side self-test ladder + console helpers shared with main.c
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion translation unit for the ``usb_selftest_cdc`` HIL example. It
 * carries the host-side polled CDC enumerate + bulk-echo ladder, the SCI8
 * console formatters those routines print through, and the per-round echo
 * pattern. main.c keeps the device-side USBX worker, the board bring-up, and
 * the entry point, and only reaches into this unit through ::cdc_host_worker
 * (the ThreadX entry it spawns in ``tx_application_define``).
 *
 * The enums declared here (::cdc_config_t, ::cdc_geom_t) are the constants
 * shared between the two units; the host-private constants live entirely in
 * the companion ``.c``.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @enum cdc_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_cdc_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_cdc_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_cdc_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_cdc_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_cdc_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_cdc_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_cdc_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_cdc_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_cdc_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_cdc_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} cdc_config_t;

/**
 * @enum cdc_geom_t
 * @brief Echo payload + bulk-pipe + pattern constants.
 */
typedef enum : uint32_t {
  k_cdc_mps           = 64U,         /**< Bulk endpoint wMaxPacketSize (FS). */
  k_cdc_payload       = 60U,         /**< Bytes per echo round (sub-MPS).    */
  k_cdc_rounds        = 8U,          /**< Echo rounds the host runs.         */
  k_cdc_echo_buf      = 64U,         /**< One-MPS scratch buffer (bytes).    */
  k_cdc_dev_addr      = 1U,          /**< Address the host assigns.          */
  k_cdc_ep_in_num     = 1U,          /**< Device bulk-IN endpoint number.    */
  k_cdc_ep_out_num    = 2U,          /**< Device bulk-OUT endpoint number.   */
  k_cdc_pipe_in       = 1U,          /**< Host pipe for the device bulk-IN.  */
  k_cdc_pipe_out      = 2U,          /**< Host pipe for the device bulk-OUT. */
  k_cdc_no_mismatch   = 0xFFFFFFFFU, /**< Probe: no mismatch.                */
  k_cdc_pat_round_mul = 97U,         /**< Per-round pattern multiplier.      */
  k_cdc_pat_idx_mul   = 7U,          /**< Per-index pattern multiplier.      */
  k_cdc_pat_bias      = 0x5AU,       /**< Pattern constant bias.             */
  k_cdc_byte_mask     = 0xFFU,       /**< Byte mask.                         */
} cdc_geom_t;

#ifndef RA8_OFF_TARGET
#include "tx_api.h"

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the host pass
 * with a retry pause until every echo round verifies; afterwards parks so
 * the verdict stays on the wire. Spawned by ``tx_application_define`` in
 * main.c as the host ThreadX entry.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra8_time.
 * @since 0.1.0
 */
VOID cdc_host_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
