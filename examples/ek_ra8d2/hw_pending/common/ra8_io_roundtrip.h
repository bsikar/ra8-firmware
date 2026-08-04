/**
 * @file examples/ek_ra8d2/hw_pending/common/ra8_io_roundtrip.h
 * @brief Shared, backend-agnostic ra8_io VFS round-trip for the FAT demos (#155).
 *
 * @details
 * The four `ra8_io_*_demo` apps (RAM, SDRAM, OSPI/xSPI NOR, SD-over-SPI) all run
 * the IDENTICAL fabric round-trip above the block-device seam: bridge the bound
 * block device to `ra8_fs`, format + mount a FAT volume, register it in the VFS
 * under a short name, then write a file / read it back / byte-compare, and
 * mkdir a subdirectory and round-trip a second file two levels deep. The ONLY
 * thing that differs per demo is the backend bind line (`ra8_io_blockdev_*_init`)
 * plus the unavoidable peripheral bring-up (OSPI controller, SD-SPI transport).
 *
 * This module factors that shared round-trip out of the four `main.c` files so
 * each demo collapses to: boot the console, bind ONE backend, hand the bound
 * ::ra8_io_blockdev_t to the helpers below, and print its own PASS banner. The
 * helpers never print -- the per-demo `main.c` owns its banner string so the
 * existing ra8_emulator smoke expectations stay byte-identical.
 *
 * Two phases are exposed so a demo can drive either or both:
 *   - ::ra8_io_roundtrip_mount       -- bridge + format + mount + VFS register.
 *   - ::ra8_io_roundtrip_root_file   -- whole-file write at the volume root +
 *                                      VFS read-back + byte-compare.
 *   - ::ra8_io_roundtrip_subdir_file -- mkdir + nested open/write/read/compare.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"

/**
 * @struct ra8_io_roundtrip_params_t
 * @brief Per-demo knobs for the shared ra8_io round-trip.
 *
 * @details Carries everything that differs between the four FAT demos so the
 *          round-trip logic itself can be identical. All string members are
 *          caller-owned NUL-terminated literals that out-live every call; the
 *          helpers neither copy nor free them.
 *
 * @invariant `vfs_prefix` is a non-empty volume name with no `':'` or `'/'`.
 * @invariant `fat_type` is one of `k_ra8_fs_type_fat12`, `_fat16`, or `_fat32`.
 *
 * @par Example:
 * @code
 * const ra8_io_roundtrip_params_t p = {
 *   .vfs_prefix    = "ram",
 *   .fat_type      = k_ra8_fs_type_fat12,
 *   .volume_label  = "RAIO",
 *   .root_file     = "HELLO.TXT",
 *   .root_path     = "ram:/HELLO.TXT",
 *   .root_bytes    = 128U,
 *   .subdir_path   = "ram:/SUB",
 *   .subdir_file   = "ram:/SUB/NOTE.TXT",
 *   .subdir_bytes  = 64U,
 * };
 * @endcode
 *
 * @see ra8_io_roundtrip_mount
 * @see ra8_io_roundtrip_root_file
 * @see ra8_io_roundtrip_subdir_file
 */
typedef struct {
  const char*   vfs_prefix;   /**< VFS volume name, e.g. "ram" (no ':' / '/').   */
  ra8_fs_type_t fat_type;     /**< FAT variant laid down by the format step.     */
  const char*   volume_label; /**< 11-char-max FAT volume label literal.         */
  const char*   log_tag;      /**< ra8_log tag used on every step failure.       */
  const char*   root_file;    /**< Root file name for the whole-file write.      */
  const char*   root_path;    /**< Full VFS path of the root file for read-back. */
  uint32_t      root_bytes;   /**< Deterministic payload length at the root.     */
  const char*   subdir_path;  /**< Full VFS path of the directory to create.     */
  const char*   subdir_file;  /**< Full VFS path of the nested file.             */
  uint32_t      subdir_bytes; /**< Deterministic payload length in the subdir.   */
} ra8_io_roundtrip_params_t;

/**
 * @brief Bridge a bound block device to ra8_fs, format + mount FAT, VFS-register.
 *
 * @details The backend-agnostic mount core shared by every FAT demo. The caller
 *          has already bound @p bd with the appropriate `ra8_io_blockdev_*_init`;
 *          this function bridges it to an `ra8_fs_backend_t`, formats it to
 *          @p p->fat_type with @p p->volume_label, mounts it, and registers the
 *          mount in the VFS under @p p->vfs_prefix. The first failing step
 *          short-circuits with its error code (logged under @p p->log_tag).
 *
 * @param[in]  bd  Bound block device (its `ra8_io_blockdev_*_init` already ran).
 * @param[in]  p   Per-demo parameters (prefix, FAT type, label, log tag).
 * @param[out] be  Caller-owned backend storage populated by the bridge; it
 *                 out-lives the mount (the VFS keeps pointers into it).
 * @param[out] out_mount Receives the mount handle on success, nullptr on error.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            The volume is formatted, mounted, and registered.
 * @retval k_ra8_err_null_ptr  @p bd, @p p, @p be, or @p out_mount was NULL.
 * @retval (other)            The first failing fabric step's code.
 *
 * @pre @p bd, @p p, @p be, and @p out_mount are all non-NULL.
 * @pre The block device backing @p bd is initialised and reachable.
 * @post On success `<prefix>:/...` paths resolve to the new FAT volume.
 * @post On any non-ok return `*out_mount` is nullptr.
 *
 * @note Not thread-safe; single-threaded init / boot context only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_roundtrip_mount(ra8_io_blockdev_t*               bd,
                                               const ra8_io_roundtrip_params_t* p,
                                               ra8_fs_backend_t*                be,
                                               ra8_fs_mount_t**                 out_mount);

/**
 * @brief Whole-file write at the volume root, VFS read-back, and byte-compare.
 *
 * @details Builds a deterministic @p p->root_bytes payload, writes it to
 *          @p p->root_file with the whole-file `ra8_fs_write_file` API (supported
 *          by every backend), reads it back through the VFS name @p p->root_path,
 *          and compares both the length and every byte.
 *
 * @param[in] mnt The mount handle returned by ::ra8_io_roundtrip_mount.
 * @param[in] p   Per-demo parameters (root file name, path, payload length).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The payload round-tripped intact.
 * @retval k_ra8_err_null_ptr          @p mnt or @p p was NULL.
 * @retval k_ra8_err_invalid_size      The read-back length differed.
 * @retval k_ra8_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing fabric step's code.
 *
 * @pre @p mnt and @p p are non-NULL and the volume is mounted.
 * @pre `p->root_bytes <= k_ra8_io_roundtrip_max_payload`.
 * @post On success @p p->root_file holds the verified payload.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_roundtrip_root_file(ra8_fs_mount_t*                  mnt,
                                                   const ra8_io_roundtrip_params_t* p);

/**
 * @brief mkdir a subdirectory via the VFS, then round-trip a nested file.
 *
 * @details Creates @p p->subdir_path through the VFS, builds a deterministic
 *          @p p->subdir_bytes note, writes it to @p p->subdir_file via VFS
 *          open(write) + `ra8_fs_write` + close, reads it back via VFS
 *          open(read) + `ra8_fs_read` + close, and byte-compares -- exercising
 *          mkdir + nested-path resolution over the mounted FAT volume.
 *
 * @param[in] p Per-demo parameters (subdir path, nested file path, length).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The note round-tripped intact.
 * @retval k_ra8_err_null_ptr          @p p was NULL.
 * @retval k_ra8_err_invalid_size      The read-back length differed.
 * @retval k_ra8_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing VFS / ra8_fs step's code.
 *
 * @pre @p p is non-NULL and its volume is mounted (see ::ra8_io_roundtrip_mount).
 * @pre `p->subdir_bytes <= k_ra8_io_roundtrip_max_payload`.
 * @post On success @p p->subdir_file holds the verified note.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_roundtrip_subdir_file(const ra8_io_roundtrip_params_t* p);
