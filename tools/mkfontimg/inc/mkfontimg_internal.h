/**
 * @file inc/mkfontimg_internal.h
 * @brief Module-private bounded host-storage composition for mkfontimg
 *
 * @details
 * Keeps POSIX descriptors and atomic-publication state at the standalone
 * tool's composition edge. The filesystem itself sees only
 * `ra8_fs_backend_t` callbacks, and font bytes cross into it through bounded
 * `ra8_fs_write` calls. All state is caller-owned and allocation-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"

/** @brief Fixed private storage bounds for the hosted adapter. */
typedef enum : uint32_t {
  k_mkfontimg_host_path_cap = 4096U, /**< Hosted path capacity. */
  k_mkfontimg_host_name_cap = 256U,  /**< Hosted leaf capacity. */
} mkfontimg_host_limit_t;

/** @brief Descriptor-backed block-device state bound into `ra8_fs`. */
typedef struct {
  uint64_t block_count; /**< Addressable sectors.           */
  uint32_t block_size;  /**< Bytes per sector.              */
  int      fd;          /**< Temporary image descriptor.    */
  bool     io_failed;   /**< Sticky positioned-I/O failure. */
} mkfontimg_disk_t;

/** @brief Caller-owned sibling-temporary and block-backend binding. */
typedef struct {
  char             final_name[k_mkfontimg_host_name_cap]; /**< Destination leaf.   */
  char             temp_name[k_mkfontimg_host_name_cap];  /**< Temporary leaf.     */
  int              directory_fd;                          /**< Parent descriptor.  */
  int              image_fd;                              /**< Image descriptor.   */
  bool             temp_exists;                           /**< Temp cleanup guard. */
  mkfontimg_disk_t disk;                                  /**< Bound block state.  */
  ra8_fs_backend_t backend;                               /**< Portable FS facade. */
} mkfontimg_host_t;

/**
 * @brief Write one complete best-effort diagnostic fragment to standard error.
 * @details Retries interrupted and short raw-descriptor writes without stdio.
 * @param[in] text NUL-terminated diagnostic fragment.
 * @pre @p text is non-null and NUL-terminated.
 * @pre The standard-error descriptor may accept or reject output.
 * @post The complete fragment was attempted.
 * @post No image or filesystem state changed.
 * @note Fragments can interleave with diagnostics from another process.
 * @since 0.1.0
 */
RA8_PRIV void priv_mkfontimg_diag(const char* text);

/**
 * @brief Write one unsigned decimal value to standard error without stdio.
 * @details Converts through fixed local buffers and delegates one bounded string.
 * @param[in] value Unsigned value to render in base ten.
 * @pre The fixed digit buffer covers every `uint64_t` value.
 * @pre The standard-error descriptor may accept or reject output.
 * @post The complete decimal spelling was attempted.
 * @post No image or filesystem state changed.
 * @note Fragments can interleave with diagnostics from another process.
 * @since 0.1.0
 */
RA8_PRIV void priv_mkfontimg_diag_u64(uint64_t value);

/** @brief Create, size, and bind one private sibling-temporary image. */
[[nodiscard]] RA8_PRIV bool priv_mkfontimg_host_begin(const char*       final_path,
                                                      uint64_t          block_count,
                                                      uint32_t          block_size,
                                                      mkfontimg_host_t* host);

/** @brief Seed exact bytes at the beginning of the temporary image. */
[[nodiscard]] RA8_PRIV bool
priv_mkfontimg_host_seed(mkfontimg_host_t* host, const uint8_t* bytes, uint32_t length);

/** @brief Stream one stable host input into a card file and verify it by reread. */
[[nodiscard]] RA8_PRIV bool priv_mkfontimg_host_copy(const mkfontimg_host_t* host,
                                                     ra8_fs_mount_t*         mount,
                                                     const char*             input_path,
                                                     const char*             card_name,
                                                     uint64_t                minimum_bytes,
                                                     uint64_t                maximum_bytes,
                                                     uint64_t*               out_bytes);

/** @brief Sync and atomically publish a complete temporary image. */
[[nodiscard]] RA8_PRIV bool priv_mkfontimg_host_commit(mkfontimg_host_t* host);

/**
 * @brief Close and unlink an unpublished temporary image.
 * @details Idempotently releases every partially acquired host resource.
 * @param[in,out] host Publication state returned by the begin operation.
 * @pre @p host is non-null and may be only partially initialized.
 * @pre No mounted filesystem still uses `host->image_fd`.
 * @post Every owned descriptor is closed.
 * @post Any owned sibling temporary is removed; the final file is unchanged.
 * @note Not thread-safe through the same state object.
 * @since 0.1.0
 */
RA8_PRIV void priv_mkfontimg_host_abort(mkfontimg_host_t* host);
