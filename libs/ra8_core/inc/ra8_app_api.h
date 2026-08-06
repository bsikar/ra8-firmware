/**
 * @file ra8_app_api.h
 * @brief Syscall table for RA8 application modules.
 * @ingroup grp_app
 *
 * @par Tag
 * [Ring 3 / App] {World: NS}
 *
 * @details
 * Defines the API surface that third-party modules loaded via the ThreadX
 * Module Manager may call. Each syscall is dispatched through the ThreadX
 * module dispatch mechanism (SVC -> kernel -> validated call -> return).
 *
 * Modules CANNOT call any firmware function directly. They can only
 * invoke these syscalls, which are validated and executed in privileged
 * (kernel) mode. This is the security boundary.
 *
 * @par Extension API IDs
 * ThreadX reserves IDs 0-499 for built-in module APIs. IDs 500-999 are
 * available for port-specific extensions (TXM_MODULE_PORT_EXTENSION_API_ID).
 * RA8 syscalls use this range.
 *
 * @par Capability Model
 * Each syscall is gated by a capability flag in the module's manifest.
 * A module that doesn't declare `RA8_APP_CAP_DISPLAY` cannot call
 * `ra8_app_gfx_*` syscalls, even if it knows the API ID.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_APP_API_H
#define RA8_APP_API_H

#include <stdint.h>

#include "ra8_err.h"

/* =========================================================================
 * Syscall API IDs (500-999 range, ThreadX port extension)
 * =========================================================================
 */

/**
 * @brief RA8 application syscall IDs.
 *
 * @details
 * These are dispatched through the ThreadX module manager's port-specific
 * extension mechanism. Each ID maps to a kernel-side handler that validates
 * parameters and executes the call in privileged mode.
 */
typedef enum : uint32_t {
  /* --- Lifecycle (500-509) --- */
  k_ra8_app_api_get_info     = 500U, /**< Get module info (name, version).  */
  k_ra8_app_api_request_stop = 501U, /**< Request graceful self-termination.*/

  /* --- Display (510-529) --- */
  k_ra8_app_api_gfx_init  = 510U, /**< Initialise graphics context.      */
  k_ra8_app_api_gfx_clear = 511U, /**< Clear framebuffer.                */
  k_ra8_app_api_gfx_blit  = 512U, /**< Blit buffer to display.           */
  k_ra8_app_api_gfx_flush = 513U, /**< Flush pending display ops.        */
  k_ra8_app_api_gfx_fill  = 514U, /**< Fill rectangle.                   */
  k_ra8_app_api_gfx_text  = 515U, /**< Render text string.               */

  /* --- Filesystem (530-549) --- */
  k_ra8_app_api_fs_open    = 530U, /**< Open file (read-only).            */
  k_ra8_app_api_fs_read    = 531U, /**< Read bytes from open file.        */
  k_ra8_app_api_fs_seek    = 532U, /**< Seek within open file.            */
  k_ra8_app_api_fs_close   = 533U, /**< Close open file.                  */
  k_ra8_app_api_fs_stat    = 534U, /**< Get file size / attributes.       */
  k_ra8_app_api_fs_readdir = 535U, /**< Read directory entries.            */

  /* --- Input (550-559) --- */
  k_ra8_app_api_input_poll = 550U, /**< Poll touch / button input.        */
  k_ra8_app_api_input_wait = 551U, /**< Block until input event.          */

  /* --- Time (560-569) --- */
  k_ra8_app_api_time_ticks = 560U, /**< Get system tick count.            */
  k_ra8_app_api_time_sleep = 561U, /**< Sleep for N ticks.                */
  k_ra8_app_api_time_ms    = 562U, /**< Get milliseconds since boot.      */

  /* --- Logging (570-579) --- */
  k_ra8_app_api_log_info  = 570U, /**< Log info message.                 */
  k_ra8_app_api_log_warn  = 571U, /**< Log warning message.              */
  k_ra8_app_api_log_error = 572U, /**< Log error message.                */

  /* --- Network / RPC (580-599) --- */
  k_ra8_app_api_rpc_call = 580U, /**< Issue RPC to host (via ESP32-C6). */
  k_ra8_app_api_rpc_poll = 581U, /**< Poll RPC response.                */

  /* --- reserved for future use (600-999) --- */
} ra8_app_api_id_t;

/* =========================================================================
 * Capability flags
 * =========================================================================
 */

/**
 * @brief Capability flags declared in a module's .ra8app manifest.
 *
 * @details
 * The kernel checks these against the module's declared capabilities
 * before dispatching each syscall. A module without the required
 * capability flag gets `k_ra8_err_not_allowed`.
 */
typedef enum : uint32_t {
  k_ra8_app_cap_lifecycle = (1U << 0U), /**< Lifecycle APIs (always on).    */
  k_ra8_app_cap_display   = (1U << 1U), /**< Display / graphics APIs.       */
  k_ra8_app_cap_fs_read   = (1U << 2U), /**< Read-only filesystem access.   */
  k_ra8_app_cap_input     = (1U << 3U), /**< Touch / button input.          */
  k_ra8_app_cap_time      = (1U << 4U), /**< Time / sleep APIs.             */
  k_ra8_app_cap_log       = (1U << 5U), /**< Logging APIs.                  */
  k_ra8_app_cap_network   = (1U << 6U), /**< Network / RPC APIs.            */
} ra8_app_cap_t;

/* =========================================================================
 * Module preamble (the first bytes of a .ra8app binary)
 * =========================================================================
 */

/**
 * @brief Magic bytes for .ra8app format identification.
 */
enum : uint32_t {
  k_ra8_app_magic = 0x52413841U, /**< "RA8A" in little-endian. */
};

/**
 * @brief .ra8app binary header (placed at offset 0 of the module binary).
 *
 * @details
 * This header is followed by the ThreadX module preamble (which ThreadX
 * itself validates), then the module code and data sections.
 *
 * The Ed25519 signature covers bytes [0, sig_offset) of the file - i.e.,
 * everything except the signature itself.
 */
typedef struct {
  uint32_t magic;          /**< Must be ::k_ra8_app_magic.                  */
  uint32_t version;        /**< Header version (1 for this format).         */
  uint32_t capabilities;   /**< Bitmask of ::ra8_app_cap_t flags.           */
  uint32_t min_fw_version; /**< Minimum firmware version required.          */
  uint32_t code_size;      /**< Size of code section in bytes.              */
  uint32_t data_size;      /**< Size of data section in bytes.              */
  uint32_t bss_size;       /**< Size of BSS section in bytes.               */
  uint32_t stack_size;     /**< Requested stack size for the module thread.  */
  uint32_t sig_offset;     /**< Offset of the 64-byte Ed25519 signature.    */
  uint32_t reserved[3];    /**< Reserved for future use (must be 0).        */
} ra8_app_header_t;

_Static_assert(sizeof(ra8_app_header_t) == 48U, "ra8_app_header_t must be 48 bytes");

/* =========================================================================
 * Kernel-side dispatch (implemented in ra8_app_dispatch.c)
 * =========================================================================
 */

/**
 * @brief Register all RA8 syscall handlers with the ThreadX module manager.
 *
 * @details
 * Called once during kernel boot, after `txm_module_manager_initialize()`.
 * Installs the port-extension dispatch table so module SVC calls route
 * to the correct kernel handler.
 *
 * @retval k_ra8_ok       All handlers registered successfully.
 * @retval k_ra8_fail     Registration failed.
 * @since 0.1.0
 */
ra8_err_t ra8_app_dispatch_init(void);

#endif /* RA8_APP_API_H */
