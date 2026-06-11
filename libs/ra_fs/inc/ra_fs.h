/**
 * @file ra_fs.h
 * @brief Minimal FAT12/FAT16/FAT32 filesystem adapter (read + write).
 *
 * @details
 * `ra_fs` is a self-contained, pure-C FAT filesystem implementation that sits
 * on top of an arbitrary block-device backend (`ra_fs_backend_t`). It mirrors
 * the public shape of Renesas FSP's `rm_filex` / FileX adapter but does not
 * carry the FileX or FSP dependencies.
 *
 * Two backends are intended for production use:
 *   1. `ra_sdhi` -- on-board SD/MMC card (sweep 1).
 *   2. `ra_usb_pmsc` storage backend -- mass-storage gadget loopback (sweep 2).
 *
 * Any object that supplies `read_block()`, `write_block()`, and
 * `get_capacity()` works -- including the in-memory mock used in
 * `tests/test_ra_fs.c`.
 *
 * ## What this implements
 *   - BPB (BIOS Parameter Block) parse + FAT type auto-detection per
 *     Microsoft "FAT: General Overview of On-Disk Format" v1.03 (the
 *     formal `count_of_clusters` rule).
 *   - FAT12 / FAT16 / FAT32 cluster-chain walking (read AND write).
 *   - 8.3 short filename directory parsing in the FAT12/16 fixed root
 *     directory and in any FAT32 cluster-chain root.
 *   - Linear file read across cluster boundaries.
 *   - File create + write, growing the chain by allocating from the FAT
 *     free-cluster scan when the current cluster fills.
 *   - Seek / tell / size.
 *   - Listdir via callback.
 *   - Unlink (mark dir entry 0xE5, free chain).
 *
 * ## What this deliberately skips
 *   - Long File Names (LFN, 0x0F attribute) -- short 8.3 only.
 *   - Sub-directory creation / removal (`mkdir`, `rmdir`).
 *   - FAT32 FSInfo free-cluster cache (we always linearly scan).
 *   - Date/time stamps on writes (left at 0, like FSP's minimal mode).
 *   - Multi-partition MBR scanning (only partition 0 is followed; a
 *     superfloppy BPB at LBA 0 is still supported transparently).
 *
 * Limits (compile-time):
 *   - 4 concurrent open file handles (`k_ra_fs_max_files`).
 *   - 2 concurrent mount points (`k_ra_fs_max_mounts`).
 *   - File size up to 4 GiB - 1 (FAT32 maximum).
 *   - Sector size: 512 bytes (the only size we test against; BPB is
 *     validated to enforce this).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Compile-time limits
 * =============================================================================
 */

/**
 * @enum ra_fs_limits_t
 * @brief Static-allocation limits for the FAT filesystem adapter.
 */
typedef enum : uint8_t {
  k_ra_fs_max_files      = 4,  /**< Max concurrent open file handles. */
  k_ra_fs_max_mounts     = 2,  /**< Max concurrent mount points. */
  k_ra_fs_sector_size    = 64, /**< Reserved -- not used as bytes; see below. */
  k_ra_fs_short_name_len = 12, /**< 8.3 short name "AAAAAAAA.EXT" + NUL. */
} ra_fs_limits_t;

/**
 * @enum ra_fs_byte_sizes_t
 * @brief Byte-size constants used by the FAT layout.
 */
typedef enum : uint16_t {
  k_ra_fs_bytes_per_sector = 512, /**< Only sector size we accept. */
  k_ra_fs_dir_entry_bytes  = 32,  /**< MS FAT spec sec 6 "Directory Entry". */
} ra_fs_byte_sizes_t;

/* =============================================================================
 * File-mode + attribute enums
 * =============================================================================
 */

/**
 * @enum ra_fs_mode_t
 * @brief File-open modes accepted by `ra_fs_open()`.
 */
typedef enum : uint8_t {
  k_ra_fs_mode_read   = 0, /**< Read-only, must exist.            */
  k_ra_fs_mode_write  = 1, /**< Truncate (or create) for writing. */
  k_ra_fs_mode_append = 2, /**< Open at EOF for writing.          */
} ra_fs_mode_t;

/**
 * @enum ra_fs_attr_t
 * @brief 8-bit FAT attribute byte (MS FAT spec sec 6 "DIR_Attr").
 */
typedef enum : uint8_t {
  k_ra_fs_attr_read_only = 0x01, /**< MS FAT spec sec 6 "ATTR_READ_ONLY". */
  k_ra_fs_attr_hidden    = 0x02, /**< MS FAT spec sec 6 "ATTR_HIDDEN".    */
  k_ra_fs_attr_system    = 0x04, /**< MS FAT spec sec 6 "ATTR_SYSTEM".    */
  k_ra_fs_attr_volume_id = 0x08, /**< MS FAT spec sec 6 "ATTR_VOLUME_ID". */
  k_ra_fs_attr_directory = 0x10, /**< MS FAT spec sec 6 "ATTR_DIRECTORY". */
  k_ra_fs_attr_archive   = 0x20, /**< MS FAT spec sec 6 "ATTR_ARCHIVE".   */
  k_ra_fs_attr_lfn       = 0x0F, /**< Long-file-name marker (we skip).    */
} ra_fs_attr_t;

/**
 * @enum ra_fs_type_t
 * @brief FAT variant detected from the BPB cluster-count rule.
 */
typedef enum : uint8_t {
  k_ra_fs_type_unknown = 0,  /**< Not yet detected / mount failed. */
  k_ra_fs_type_fat12   = 12, /**< count_of_clusters < 4085. */
  k_ra_fs_type_fat16   = 16, /**< 4085 <= count_of_clusters < 65525. */
  k_ra_fs_type_fat32   = 32, /**< count_of_clusters >= 65525. */
  k_ra_fs_type_exfat   = 64, /**< exFAT (read-only). */
} ra_fs_type_t;

/* =============================================================================
 * Backend interface
 * =============================================================================
 */

/**
 * @struct ra_fs_backend_t
 * @brief Block-device interface that ra_fs runs on top of.
 *
 * @details
 * Three function pointers + a context cookie. Each implementation (SDHI,
 * USB MSC, mock) fills these in and passes the struct to `ra_fs_mount()`.
 *
 * @invariant `read_block`, `write_block`, `get_capacity` are non-NULL for
 *            a usable backend. `ctx` may be NULL.
 */
typedef struct {
  /**
   * @brief Read `count` consecutive 512-byte blocks starting at `lba`.
   * @param[in]  ctx   Backend cookie.
   * @param[in]  lba   Logical block address (0 = BPB).
   * @param[in]  count Number of blocks to read.
   * @param[out] buf   Destination buffer of at least `count*512` bytes.
   */
  ra_err_t (*read_block)(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf);

  /**
   * @brief Write `count` consecutive 512-byte blocks starting at `lba`.
   */
  ra_err_t (*write_block)(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf);

  /**
   * @brief Report the device size.
   * @param[out] block_count Total blocks (sectors).
   * @param[out] block_size  Bytes per block (must equal 512).
   */
  ra_err_t (*get_capacity)(void* ctx, uint32_t* block_count, uint32_t* block_size);

  /** @brief Caller-owned context passed back into the function pointers. */
  void* ctx;
} ra_fs_backend_t;

/* =============================================================================
 * Mount / file handle (opaque-ish)
 * =============================================================================
 */

/**
 * @struct ra_fs_mount_t
 * @brief Cached parse of one mounted FAT volume.
 *
 * @details
 * Populated by `ra_fs_mount()` from the on-disk BPB. The struct is
 * intentionally exposed so callers can statically allocate it; treat
 * the fields as read-only.
 */
typedef struct {
  ra_fs_backend_t backend;             /**< Block-device backend.            */
  ra_fs_type_t    type;                /**< FAT12 / FAT16 / FAT32.           */
  uint32_t        bytes_per_sector;    /**< BPB BPB_BytsPerSec.              */
  uint32_t        sectors_per_cluster; /**< BPB BPB_SecPerClus.           */
  uint32_t        reserved_sectors;    /**< BPB BPB_RsvdSecCnt.              */
  uint32_t        num_fats;            /**< BPB BPB_NumFATs.                 */
  uint32_t        root_entries;        /**< BPB BPB_RootEntCnt (FAT12/16).   */
  uint32_t        total_sectors;       /**< BPB BPB_TotSec16 / BPB_TotSec32. */
  uint32_t        fat_size_sectors;    /**< BPB BPB_FATSz16 / BPB_FATSz32.   */
  uint32_t        root_cluster;        /**< BPB BPB_RootClus (FAT32 only).   */
  uint32_t        first_fat_lba;       /**< Computed: first FAT sector.      */
  uint32_t        first_root_lba;      /**< FAT12/16 fixed root-dir start.   */
  uint32_t        first_data_lba;      /**< First sector of the data region. */
  uint32_t        count_of_clusters;   /**< Per MS spec: data_sectors / SPC.*/
  uint32_t        partition_base_lba;  /**< MBR partition start (0 = superfloppy). */
  uint8_t         in_use;              /**< 0 = slot free, 1 = mounted.      */
} ra_fs_mount_t;

/**
 * @struct ra_fs_file_t
 * @brief Open-file state.
 */
typedef struct {
  ra_fs_mount_t* mount;         /**< Owning mount point.                       */
  uint32_t       first_cluster; /**< Head of the file's cluster chain.        */
  uint32_t       cur_cluster;   /**< Cluster the offset currently points into.*/
  uint32_t       size_bytes;    /**< File size (DIR_FileSize).                */
  uint32_t       offset;        /**< Current read/write offset.               */
  uint32_t       dir_entry_lba; /**< Sector containing the dir entry.         */
  uint32_t       dir_entry_idx; /**< Byte offset of dir entry within sector.  */
  ra_fs_mode_t   mode;          /**< Open mode.                               */
  uint8_t        in_use;        /**< 0 = slot free, 1 = open.                 */
  uint8_t        no_fat_chain;  /**< exFAT contiguous file (no FAT walk).     */
} ra_fs_file_t;

/* =============================================================================
 * Listdir callback
 * =============================================================================
 */

/**
 * @typedef ra_fs_listdir_cb_t
 * @brief Callback invoked once per directory entry by `ra_fs_listdir()`.
 *
 * @param[in] name 8.3 short filename, NUL-terminated, max 12 bytes.
 * @param[in] attr `ra_fs_attr_t` attribute byte.
 * @param[in] size File size in bytes (0 for directories).
 * @param[in] ctx  Caller-provided cookie.
 */
typedef void (*ra_fs_listdir_cb_t)(const char* name, uint8_t attr, uint32_t size, void* ctx);

/* =============================================================================
 * Public API -- mount / unmount
 * =============================================================================
 */

/**
 * @brief Mount a FAT volume from a block-device backend.
 *
 * @details
 * Reads sector 0 (the BPB), validates the signature and bytes-per-sector,
 * computes the cluster count, dispatches to FAT12 / FAT16 / FAT32, and
 * caches the layout in `*out_handle`.
 *
 * @param[in]  backend    Block-device implementation. Must remain alive
 *                        for the lifetime of the mount.
 * @param[out] out_handle Populated mount handle on success.
 *
 * @return ra_err_t
 * @retval k_ra_ok                   Volume mounted successfully.
 * @retval k_ra_err_null_ptr         backend or out_handle is NULL.
 * @retval k_ra_err_invalid_arg      Backend has NULL function pointers.
 * @retval k_ra_err_no_mem           No free mount slot.
 * @retval k_ra_err_validation_failed BPB signature invalid / unsupported.
 *
 * @pre `backend->read_block` is non-NULL.
 * @pre `out_handle` is non-NULL.
 * @post On success, `out_handle->in_use == 1` and `type != unknown`.
 * @post On failure, `*out_handle` is left untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_mount(const ra_fs_backend_t* backend, ra_fs_mount_t** out_handle);

/**
 * @brief Unmount a previously mounted volume and release its slot.
 *
 * @param[in,out] handle Mount handle returned by `ra_fs_mount()`.
 *
 * @retval k_ra_ok           Slot released.
 * @retval k_ra_err_null_ptr handle is NULL.
 * @retval k_ra_err_invalid_state Slot was not in use.
 *
 * @pre handle was returned by ra_fs_mount.
 * @post handle->in_use == 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_unmount(ra_fs_mount_t* handle);

/* =============================================================================
 * Public API -- file ops
 * =============================================================================
 */

/**
 * @brief Open or create a file by 8.3 short path (root dir only).
 *
 * @param[in]  handle    Mount handle.
 * @param[in]  path      8.3 path, e.g. "HELLO.TXT". Subdirectories not supported.
 * @param[in]  mode      `k_ra_fs_mode_read`, `_write`, or `_append`.
 * @param[out] out_file  Populated file handle on success.
 *
 * @retval k_ra_ok                     File opened.
 * @retval k_ra_err_null_ptr           Any pointer arg is NULL.
 * @retval k_ra_err_not_found          Read mode and path doesn't exist.
 * @retval k_ra_err_no_mem             No free file slot.
 * @retval k_ra_err_no_data            Write mode failed to allocate cluster.
 *
 * @pre handle->in_use == 1.
 * @post On success, out_file->in_use == 1 and offset is at file start (read/write)
 *       or end (append).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_fs_open(ra_fs_mount_t* handle, const char* path, ra_fs_mode_t mode, ra_fs_file_t** out_file);

/**
 * @brief Close an open file. Pending writes have already been flushed by
 *        `ra_fs_write()`, so this just releases the slot.
 *
 * @retval k_ra_ok           Closed.
 * @retval k_ra_err_null_ptr file is NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_close(ra_fs_file_t* file);

/**
 * @brief Read up to `max_len` bytes; advance the cluster chain on cluster crossings.
 *
 * @param[in]  file    Open file (any mode).
 * @param[out] buf     Destination buffer.
 * @param[in]  max_len Maximum bytes to copy.
 * @param[out] got_len Actual bytes read (0 at EOF).
 *
 * @retval k_ra_ok                Read completed (got_len may be 0 = EOF).
 * @retval k_ra_err_null_ptr      Any pointer arg is NULL.
 * @retval k_ra_err_invalid_state file->in_use == 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_fs_read(ra_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len);

/**
 * @brief Write `len` bytes; allocate new clusters from FAT free-space scan as needed.
 *
 * @retval k_ra_ok                Wrote all bytes; size + dir entry updated.
 * @retval k_ra_err_null_ptr      file or buf NULL.
 * @retval k_ra_err_invalid_state file not opened for writing.
 * @retval k_ra_err_no_mem        Volume out of free clusters.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_write(ra_fs_file_t* file, const uint8_t* buf, uint32_t len);

/**
 * @brief Create a whole file in one call (provisioning helper).
 *
 * @details Convenience wrapper that creates @p path and writes @p data. On FAT
 * volumes it opens in write mode, writes, and closes. On exFAT it allocates a
 * contiguous cluster run and links a fresh directory entry (the only exFAT
 * write path). It does not overwrite an existing file.
 *
 * @param[in] handle Mounted volume.
 * @param[in] path   Flat root-level file name (ASCII).
 * @param[in] data   File contents.
 * @param[in] len    Byte count (> 0).
 *
 * @retval k_ra_ok           File created and written.
 * @retval k_ra_err_null_ptr Any pointer argument was NULL.
 * @retval k_ra_err_no_mem   Out of contiguous space or directory slots.
 * @retval k_ra_err_*        Backend error.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_fs_write_file(ra_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len);

/**
 * @brief Move the file offset to `offset_bytes` (clamped to size).
 * @retval k_ra_ok            Seek committed.
 * @retval k_ra_err_null_ptr  file is NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_seek(ra_fs_file_t* file, uint32_t offset_bytes);

/** @brief Report the current offset. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_fs_tell(const ra_fs_file_t* file, uint32_t* out_offset);

/** @brief Report the file's size in bytes. @since 0.1.0 */
[[nodiscard]] ra_err_t ra_fs_size(const ra_fs_file_t* file, uint32_t* out_bytes);

/* =============================================================================
 * Public API -- directory ops
 * =============================================================================
 */

/**
 * @brief Enumerate directory entries; invoke `cb` once per visible entry.
 *
 * @param[in] handle Mount handle.
 * @param[in] path   Directory path. Currently only "/" is supported.
 * @param[in] cb     Callback (must be non-NULL).
 * @param[in] ctx    Cookie forwarded to the callback.
 *
 * @retval k_ra_ok                 Enumeration complete.
 * @retval k_ra_err_null_ptr       handle/cb NULL.
 * @retval k_ra_err_not_supported  path != "/".
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_fs_listdir(ra_fs_mount_t* handle, const char* path, ra_fs_listdir_cb_t cb, void* ctx);

/**
 * @brief Delete a file: mark its dir entry 0xE5 and free the cluster chain.
 *
 * @retval k_ra_ok               File unlinked.
 * @retval k_ra_err_null_ptr     handle/path NULL.
 * @retval k_ra_err_not_found    File doesn't exist.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_fs_unlink(ra_fs_mount_t* handle, const char* path);

#ifdef __cplusplus
}
#endif
