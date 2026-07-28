/**
 * @file usb_selftest_mlun_steps.h
 * @brief Shared constants + cross-TU prototypes for usb_selftest_mlun
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The multi-LUN USB self-loop app is split into two translation units to keep
 * each one under the per-file line cap: main.c owns boot, pin/clock bring-up,
 * the device-side USBX MSC class, and the ThreadX entry hooks; the steps TU
 * (usb_selftest_mlun_steps.c) owns the shared per-(LUN,LBA) pattern, the SCI8
 * console + text formatters, and the host-side enumerate / per-LUN verify /
 * retry worker.
 *
 * This header holds the typed-enum constants both TUs share (thread + pool
 * sizing, the text-formatter widths, the LUN geometry, the host-ladder probe
 * phases) plus prototypes for the two symbols that cross the TU boundary:
 * ::mlun_pattern_fill (the device media-read in main.c calls it) and
 * ::mlun_host_worker (::tx_application_define in main.c spawns it). Every
 * other moved helper stays file-static in the steps TU.
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
 * @enum mlun_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_mlun_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_mlun_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_mlun_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_mlun_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_mlun_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_mlun_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_mlun_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_mlun_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_mlun_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_mlun_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} mlun_config_t;

/**
 * @enum mlun_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_mlun_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_mlun_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_mlun_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_mlun_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_mlun_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} mlun_hex_t;

/**
 * @enum mlun_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_mlun_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_mlun_dec_radix   = 10U,  /**< Base for decimal conversion. */
} mlun_mask_t;

/**
 * @enum mlun_geom_t
 * @brief LUN geometry + verification constants.
 */
typedef enum : uint32_t {
  k_mlun_count              = 2U,          /**< Logical units exposed (UX_MAX_SLAVE_LUN). */
  k_mlun_sectors            = 256U,        /**< 512-byte sectors per LUN.                 */
  k_mlun_block_size         = 512U,        /**< SCSI logical block size.                  */
  k_mlun_burst_blocks       = 8U,          /**< Blocks per READ(10) burst.                */
  k_mlun_burst_bytes        = 4096U,       /**< 8 x 512 B burst buffer.                   */
  k_mlun_target_lun0        = 0U,          /**< First LUN index.                          */
  k_mlun_no_mismatch        = 0xFFFFFFFFU, /**< Probe: no mismatch.                       */
  k_mlun_pat_lun_mul        = 97U,         /**< Per-LUN pattern multiplier.               */
  k_mlun_pat_lba_mul        = 7U,          /**< Per-LBA pattern multiplier.               */
  k_mlun_pat_bias           = 0x5AU,       /**< Pattern constant bias.                    */
  k_mlun_byte_mask          = 0xFFU,       /**< Byte mask.                                */
  k_mlun_mismatch_lun_shift = 24U,         /**< s_dbg_mismatch: LUN in bits 31:24.        */
} mlun_geom_t;

/**
 * @enum mlun_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_mlun_phase_boot   = 0U, /**< Host thread not started.  */
  k_mlun_phase_init   = 1U, /**< ra8_usb_hmsc_init issued. */
  k_mlun_phase_enum   = 2U, /**< Enumerating.              */
  k_mlun_phase_verify = 3U, /**< Reading + checking LUNs.  */
  k_mlun_phase_pass   = 4U, /**< All LUNs verified.        */
} mlun_phase_t;

#ifndef RA8_OFF_TARGET

#include "tx_api.h"
#include "ux_api.h"

/**
 * @brief Fill one 512-byte sector with this LUN/LBA's deterministic bytes.
 *
 * @details Byte i = ``(lun*97 + lba*7 + i + 0x5A) & 0xFF``. Distinct per
 * LUN and per LBA so the host can prove it addressed the right logical
 * unit and sector. The device media-read and the host verifier compute
 * it identically.
 *
 * @param[in]  lun The logical unit (0..1).
 * @param[in]  lba The logical block address within the LUN.
 * @param[out] out 512-byte destination buffer.
 *
 * @pre @p out has ::k_mlun_block_size writable bytes.
 * @pre @p lun and @p lba are within the exposed geometry.
 * @post @p out holds the sector's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function. Defined in usb_selftest_mlun_steps.c; the device
 *       media-read in main.c shares it with the host verifier.
 * @since 0.1.0
 */
void mlun_pattern_fill(uint32_t lun, uint32_t lba, UCHAR* out);

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full
 * enumerate + per-LUN verify pass with a retry pause until both LUNs
 * verify; afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra8_time. Defined in
 *       usb_selftest_mlun_steps.c; ::tx_application_define spawns it.
 * @since 0.1.0
 */
VOID mlun_host_worker(ULONG arg);

#endif /* !RA8_OFF_TARGET */
