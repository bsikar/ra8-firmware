/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_dfu/inc/usb_selftest_dfu_steps.h
 * @brief Shared constants + host-side DFU ladder interface for usb_selftest_dfu
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Declares the compile-time tunables, image geometry, text-formatter sizing,
 * Chapter-9 + DFU class request constants, and the polled host-ladder entry
 * point that ``main.c`` shares with ``usb_selftest_dfu_steps.c``. The host
 * worker thread (in ``main.c``) calls ::dfu_host_pass; everything else in the
 * sibling translation unit stays ``static``.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-15
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum dfu_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_dfu_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_dfu_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_dfu_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_dfu_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_dfu_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_dfu_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_dfu_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_dfu_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_dfu_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_dfu_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} dfu_config_t;

/**
 * @enum dfu_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_dfu_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_dfu_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_dfu_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_dfu_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_dfu_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} dfu_hex_t;

/**
 * @enum dfu_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_dfu_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_dfu_dec_radix   = 10U,  /**< Base for decimal conversion. */
} dfu_mask_t;

/**
 * @enum dfu_geom_t
 * @brief Firmware-image geometry + pattern constants.
 */
typedef enum : uint32_t {
  k_dfu_xfer_size   = 64U,         /**< wTransferSize: bytes per DFU block. */
  k_dfu_blocks      = 8U,          /**< Blocks in the rehearsal image.      */
  k_dfu_image_bytes = 512U,        /**< k_dfu_blocks * k_dfu_xfer_size.     */
  k_dfu_no_mismatch = 0xFFFFFFFFU, /**< Probe: no mismatch.                 */
  k_dfu_pat_blk_mul = 131U,        /**< Per-block pattern multiplier.       */
  k_dfu_pat_idx_mul = 7U,          /**< Per-index pattern multiplier.       */
  k_dfu_pat_bias    = 0xA5U,       /**< Pattern constant bias.              */
  k_dfu_byte_mask   = 0xFFU,       /**< Byte mask.                          */
  k_dfu_dev_addr    = 1U,          /**< Address the host assigns.           */
  k_dfu_intf        = 0U,          /**< DFU interface number.               */
  k_dfu_config_val  = 1U,          /**< bConfigurationValue.                */
} dfu_geom_t;

/**
 * @enum dfu_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_dfu_phase_boot     = 0U, /**< Host thread not started.   */
  k_dfu_phase_init     = 1U, /**< Host controller init.      */
  k_dfu_phase_enum     = 2U, /**< Enumerating.               */
  k_dfu_phase_download = 3U, /**< Running DFU_DNLOAD.        */
  k_dfu_phase_upload   = 4U, /**< Running DFU_UPLOAD.        */
  k_dfu_phase_pass     = 5U, /**< Image verified byte-equal. */
} dfu_phase_t;

/**
 * @enum dfu_req_t
 * @brief Chapter-9 + DFU class request / descriptor constants.
 */
typedef enum : uint16_t {
  k_dfu_bm_std_dev_in     = 0x80U, /**< bmRequestType: Std | Device | In.       */
  k_dfu_bm_std_dev_out    = 0x00U, /**< bmRequestType: Std | Device | Out.      */
  k_dfu_bm_class_if_out   = 0x21U, /**< bmRequestType: Class | Interface | Out. */
  k_dfu_bm_class_if_in    = 0xA1U, /**< bmRequestType: Class | Interface | In.  */
  k_dfu_breq_get_desc     = 0x06U, /**< GET_DESCRIPTOR.                         */
  k_dfu_breq_set_addr     = 0x05U, /**< SET_ADDRESS.                            */
  k_dfu_breq_set_config   = 0x09U, /**< SET_CONFIGURATION.                      */
  k_dfu_breq_dnload       = 0x01U, /**< DFU_DNLOAD.                             */
  k_dfu_breq_upload       = 0x02U, /**< DFU_UPLOAD.                             */
  k_dfu_breq_getstatus    = 0x03U, /**< DFU_GETSTATUS.                          */
  k_dfu_breq_abort        = 0x06U, /**< DFU_ABORT (-> dfuIDLE).                 */
  k_dfu_desc_device       = 0x01U, /**< DEVICE descriptor type.                 */
  k_dfu_dev_desc_len      = 18U,   /**< DEVICE descriptor length.               */
  k_dfu_off_dev_pid       = 10U,   /**< idProduct LSB byte offset.              */
  k_dfu_byte_bits         = 8U,    /**< Bits per byte.                          */
  k_dfu_getstatus_len     = 6U,    /**< DFU_GETSTATUS payload len.              */
  k_dfu_off_status_state  = 4U,    /**< bState offset in GETSTATUS.             */
  k_dfu_state_dnload_idle = 5U,    /**< dfuDNLOAD-IDLE.                         */
  k_dfu_state_idle        = 2U,    /**< dfuIDLE.                                */
} dfu_req_t;

/**
 * @enum dfu_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration + status polling.
 */
typedef enum : uint32_t {
  k_dfu_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_dfu_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_dfu_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_dfu_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_dfu_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_dfu_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_dfu_status_poll_ms = 2U,        /**< Pause between GETSTATUS polls.       */
  k_dfu_status_tries   = 50U,       /**< GETSTATUS polls before giving up.    */
  k_dfu_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_dfu_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} dfu_enum_tune_t;

#ifndef RA8_OFF_TARGET

/**
 * @brief Run the full host pass: enumerate, download, upload-verify.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed DFU PASS.
 * @pre Device-side DFU class is registered (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success the host pass counter advanced and LED2 is on.
 * @post On failure the host controller is deinitialized for a clean retry.
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t dfu_host_pass(void);

#endif /* !RA8_OFF_TARGET */
