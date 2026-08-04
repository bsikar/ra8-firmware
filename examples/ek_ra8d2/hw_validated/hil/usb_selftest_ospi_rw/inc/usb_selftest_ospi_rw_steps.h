/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_ospi_rw/inc/usb_selftest_ospi_rw_steps.h
 * @brief Shared tunables + host-side worker seam for the writable-OSPI self-loop
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Splits the `usb_selftest_ospi_rw` application across two translation
 * units to keep each below the 1000-line file-size cap. This header carries
 * the compile-time tunables (threads, pool, console cadence, LUN geometry,
 * text-formatter sizing, host-ladder phase markers) that BOTH `main.c`
 * (device + board bring-up side) and the host-side step routines in
 * ``usb_selftest_ospi_rw_steps.c`` reference, plus the prototype for the
 * single host worker that ``tx_application_define`` (in `main.c`) spawns.
 *
 * The enums are defined here -- not duplicated -- so the two TUs share one
 * authoritative definition. The console formatters, the per-(LUN,LBA)
 * pattern generator, and the host READ/WRITE verifier all live in the
 * sibling and are file-private there; only ::ospirw_host_worker crosses the
 * TU boundary, so it is the only function prototype exported here.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/* -------------------------------------------------------------------------- */
/* Tunables (shared by main.c and the host-side step routines) */
/* -------------------------------------------------------------------------- */

/**
 * @enum ospirw_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_ospirw_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_ospirw_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_ospirw_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_ospirw_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_ospirw_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_ospirw_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_ospirw_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_ospirw_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_ospirw_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_ospirw_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} ospirw_config_t;

/**
 * @enum ospirw_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_ospirw_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_ospirw_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_ospirw_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_ospirw_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_ospirw_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} ospirw_hex_t;

/**
 * @enum ospirw_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_ospirw_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_ospirw_dec_radix   = 10U,  /**< Base for decimal conversion. */
} ospirw_mask_t;

/**
 * @enum ospirw_geom_t
 * @brief LUN geometry + verification constants.
 */
typedef enum : uint32_t {
  k_ospirw_count              = 1U,          /**< Logical units exposed (single writable). */
  k_ospirw_sectors            = 64U,         /**< 512-byte sectors (OSPI window = 32 KiB). */
  k_ospirw_block_size         = 512U,        /**< SCSI logical block size.                 */
  k_ospirw_burst_blocks       = 8U,          /**< Blocks per READ(10) burst.               */
  k_ospirw_burst_bytes        = 4096U,       /**< 8 x 512 B burst buffer.                  */
  k_ospirw_target_lun0        = 0U,          /**< First LUN index.                         */
  k_ospirw_no_mismatch        = 0xFFFFFFFFU, /**< Probe: no mismatch.                      */
  k_ospirw_pat_lun_mul        = 97U,         /**< Per-LUN pattern multiplier.              */
  k_ospirw_pat_lba_mul        = 7U,          /**< Per-LBA pattern multiplier.              */
  k_ospirw_pat_bias           = 0x5AU,       /**< Pattern constant bias.                   */
  k_ospirw_byte_mask          = 0xFFU,       /**< Byte mask.                               */
  k_ospirw_mismatch_lun_shift = 24U,         /**< s_dbg_mismatch: LUN in bits 31:24.       */
  k_ospirw_instance           = 0U,          /**< xSPI controller instance.                */
  k_ospirw_offset             = 0x00200000U, /**< 2 MiB into the chip (scratch).           */
  k_ospirw_erase_sector       = 0x00001000U, /**< IS25LX512M 4 KiB erase sector.           */
} ospirw_geom_t;

/**
 * @enum ospirw_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_ospirw_phase_boot   = 0U, /**< Host thread not started.  */
  k_ospirw_phase_init   = 1U, /**< ra8_usb_hmsc_init issued. */
  k_ospirw_phase_enum   = 2U, /**< Enumerating.              */
  k_ospirw_phase_verify = 3U, /**< Reading + checking LUNs.  */
  k_ospirw_phase_pass   = 4U, /**< All LUNs verified.        */
} ospirw_phase_t;

/**
 * @brief Bring the OSPI flash up and erase the writable window.
 *
 * @details Mirrors the read-only OSPI self-loop's bring-up: U15 expander
 * courtesy write, ``ra8_board_xspi_pins_init`` (OCTA pins + RESET pulse),
 * ``ra8_xspi_init`` 1S-1S-1S, JEDEC-id readback, then a one-shot erase of
 * the ::k_ospirw_sectors window at ::k_ospirw_offset so the per-write path
 * is fast program-only. The host owns the data. The body lives in
 * ``usb_selftest_ospi_rw_device.c``; ``ospirw_device_worker`` in `main.c`
 * calls it once before USB attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok OSPI is up and the write window is erased.
 * @retval (other) A pins / init / erase step failed.
 *
 * @pre CGC is initialized (main ran ra8_cgc_init).
 * @pre Single caller (the device worker, before USB attach).
 * @post The OSPI bring-up / id J-Link probes reflect the outcome.
 * @post The 32 KiB OSPI window is erased (0xFF) on success.
 *
 * @note Blocking; runs once at boot before USB attach.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ospirw_ospi_provision(void);

#ifndef RA8_OFF_TARGET
/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops the full
 * host pass (enumerate + WRITE(10) + read-verify) with a retry pause
 * until the writable LUN verifies; afterwards parks so the verdict stays
 * on the wire. Spawned by ``tx_application_define`` in `main.c`; the body
 * and all of its helpers live in ``usb_selftest_ospi_rw_steps.c``.
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
VOID ospirw_host_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
