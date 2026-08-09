/**
 * @file ra8_fs_types.h
 * @brief On-disk and handle data types shared by the ra8_fs public API.
 *
 * @details
 * The value types the filesystem API is written in terms of: the static-limit
 * and layout enums, the block-device backend interface, the format-options
 * struct, the mount and open-file handles, the stat result, the listdir
 * callback, and the partition selector. They live in their own header so
 * `ra8_fs.h` stays under the repository's 1000-line source cap while remaining
 * the single include a consumer needs -- `ra8_fs.h` includes this file, so
 * nothing a consumer includes changes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Compile-time limits
 * =============================================================================
 */

/**
 * @enum ra8_fs_limits_t
 * @brief Static-allocation limits for the FAT filesystem adapter.
 */
typedef enum : uint8_t {
  k_ra8_fs_max_files      = 4,  /**< Max concurrent open file handles.         */
  k_ra8_fs_max_mounts     = 2,  /**< Max concurrent mount points.              */
  k_ra8_fs_sector_size    = 64, /**< Reserved -- not used as bytes; see below. */
  k_ra8_fs_short_name_len = 12, /**< 8.3 short name "AAAAAAAA.EXT" + NUL.      */
} ra8_fs_limits_t;

/**
 * @enum ra8_fs_byte_sizes_t
 * @brief Byte-size constants used by the FAT layout.
 */
typedef enum : uint16_t {
  k_ra8_fs_bytes_per_sector = 512, /**< Only sector size we accept.          */
  k_ra8_fs_dir_entry_bytes  = 32,  /**< MS FAT spec sec 6 "Directory Entry". */
} ra8_fs_byte_sizes_t;

/* =============================================================================
 * File-mode + attribute enums
 * =============================================================================
 */

/**
 * @enum ra8_fs_mode_t
 * @brief File-open modes accepted by `ra8_fs_open()`.
 */
typedef enum : uint8_t {
  k_ra8_fs_mode_read   = 0, /**< Read-only, must exist.            */
  k_ra8_fs_mode_write  = 1, /**< Truncate (or create) for writing. */
  k_ra8_fs_mode_append = 2, /**< Open at EOF for writing.          */
} ra8_fs_mode_t;

/**
 * @enum ra8_fs_attr_t
 * @brief 8-bit FAT attribute byte (MS FAT spec sec 6 "DIR_Attr").
 */
typedef enum : uint8_t {
  k_ra8_fs_attr_read_only = 0x01, /**< MS FAT spec sec 6 "ATTR_READ_ONLY". */
  k_ra8_fs_attr_hidden    = 0x02, /**< MS FAT spec sec 6 "ATTR_HIDDEN".    */
  k_ra8_fs_attr_system    = 0x04, /**< MS FAT spec sec 6 "ATTR_SYSTEM".    */
  k_ra8_fs_attr_volume_id = 0x08, /**< MS FAT spec sec 6 "ATTR_VOLUME_ID". */
  k_ra8_fs_attr_directory = 0x10, /**< MS FAT spec sec 6 "ATTR_DIRECTORY". */
  k_ra8_fs_attr_archive   = 0x20, /**< MS FAT spec sec 6 "ATTR_ARCHIVE".   */
  k_ra8_fs_attr_lfn       = 0x0F, /**< Long-file-name marker (we skip).    */
} ra8_fs_attr_t;

/**
 * @enum ra8_fs_type_t
 * @brief FAT variant detected from the BPB cluster-count rule.
 */
typedef enum : uint8_t {
  k_ra8_fs_type_unknown = 0,  /**< Not yet detected / mount failed.         */
  k_ra8_fs_type_fat12   = 12, /**< count_of_clusters < 4085.                */
  k_ra8_fs_type_fat16   = 16, /**< 4085 <= count_of_clusters < 65525.       */
  k_ra8_fs_type_fat32   = 32, /**< count_of_clusters >= 65525.              */
  k_ra8_fs_type_exfat   = 64, /**< exFAT (read + streaming write + format). */
} ra8_fs_type_t;

/* =============================================================================
 * Backend interface
 * =============================================================================
 */

/**
 * @struct ra8_fs_backend_t
 * @brief Block-device interface that ra8_fs runs on top of.
 *
 * @details
 * Three function pointers + a context cookie. Each implementation (SDHI,
 * USB MSC, mock) fills these in and passes the struct to `ra8_fs_mount()`.
 *
 * @invariant `read_block`, `write_block`, `get_capacity` are non-NULL for
 *            a usable backend. `erase_blocks` and `ctx` may be NULL.
 */
typedef struct {
  /**
   * @brief Read `count` consecutive 512-byte blocks starting at `lba`.
   * @param[in]  ctx   Backend cookie.
   * @param[in]  lba   Logical block address (0 = BPB).
   * @param[in]  count Number of blocks to read.
   * @param[out] buf   Destination buffer of at least `count*512` bytes.
   */
  ra8_err_t (*read_block)(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf);

  /**
   * @brief Write `count` consecutive 512-byte blocks starting at `lba`.
   */
  ra8_err_t (*write_block)(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf);

  /**
   * @brief Report the device size.
   * @param[out] block_count Total blocks (sectors).
   * @param[out] block_size  Bytes per block (must equal 512).
   */
  ra8_err_t (*get_capacity)(void* ctx, uint32_t* block_count, uint32_t* block_size);

  /**
   * @brief OPTIONAL: bulk-erase `count` blocks at `lba` to all-zero bytes.
   *
   * @details May be NULL. When non-NULL and it returns ::k_ra8_ok, the range
   * `[lba, lba+count)` is guaranteed to read back as `0x00` -- letting the
   * formatter skip an explicit zero-write of that region (much faster on flash
   * media that erases internally). A backend that cannot guarantee a zero
   * post-erase value (or does not implement erase) must return
   * ::k_ra8_err_not_supported so the formatter falls back to writing zeros; any
   * other non-OK error aborts the format.
   * @param[in] ctx   Backend cookie.
   * @param[in] lba   First block to erase.
   * @param[in] count Number of blocks to erase.
   */
  ra8_err_t (*erase_blocks)(void* ctx, uint32_t lba, uint32_t count);

  /** @brief Caller-owned context passed back into the function pointers. */
  void* ctx;
} ra8_fs_backend_t;

/* =============================================================================
 * Format (mkfs) options
 * =============================================================================
 */

/**
 * @struct ra8_fs_format_opts_t
 * @brief Tunables for `ra8_fs_format()` (the on-disk geometry of a fresh volume).
 *
 * @details
 * Open/Closed seam for the formatter: callers select the FAT variant and
 * label, and may optionally pin the cluster size. Zero-initialise the struct
 * (`= {}`) for sane defaults -- a zero `sectors_per_cluster` asks the formatter
 * to pick the smallest power-of-two that lands the cluster count inside the
 * range valid for `type` on the backend's capacity.
 *
 * @invariant `type` is one of `k_ra8_fs_type_fat12`, `_fat16`, `_fat32`, or
 *            `_exfat` (every type `ra8_fs` can write -- see `ra8_fs_format()`).
 * @invariant `sectors_per_cluster`, when non-zero, is a power of two in the
 *            closed range 1..128.
 *
 * @par Example:
 * @code
 * ra8_fs_format_opts_t opts = {};
 * opts.type  = k_ra8_fs_type_fat32;
 * opts.label = "DATA";
 * ra8_err_t e = ra8_fs_format(&backend, &opts);  // auto cluster size
 * @endcode
 *
 * @see ra8_fs_format()
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_type_t type;                /**< FAT variant to lay down (12/16/32). */
  const char*   label;               /**< 0..11 char volume label, or NULL.   */
  uint8_t       sectors_per_cluster; /**< Cluster size; 0 = auto-select.      */
} ra8_fs_format_opts_t;

/* =============================================================================
 * Mount / file handle (opaque-ish)
 * =============================================================================
 */

/**
 * @struct ra8_fs_mount_t
 * @brief Cached parse of one mounted FAT volume.
 *
 * @details
 * Populated by `ra8_fs_mount()` from the on-disk BPB. The struct is
 * intentionally exposed so callers can statically allocate it; treat
 * the fields as read-only.
 */
typedef struct {
  ra8_fs_backend_t backend;             /**< Block-device backend.                  */
  ra8_fs_type_t    type;                /**< FAT12 / FAT16 / FAT32.                 */
  uint32_t         bytes_per_sector;    /**< BPB BPB_BytsPerSec.                    */
  uint32_t         sectors_per_cluster; /**< BPB BPB_SecPerClus.                    */
  uint32_t         reserved_sectors;    /**< BPB BPB_RsvdSecCnt.                    */
  uint32_t         num_fats;            /**< BPB BPB_NumFATs.                       */
  uint32_t         root_entries;        /**< BPB BPB_RootEntCnt (FAT12/16).         */
  uint32_t         total_sectors;       /**< BPB BPB_TotSec16 / BPB_TotSec32.       */
  uint32_t         fat_size_sectors;    /**< BPB BPB_FATSz16 / BPB_FATSz32.         */
  uint32_t         root_cluster;        /**< BPB BPB_RootClus (FAT32 only).         */
  uint32_t         first_fat_lba;       /**< Computed: first FAT sector.            */
  uint32_t         first_root_lba;      /**< FAT12/16 fixed root-dir start.         */
  uint32_t         first_data_lba;      /**< First sector of the data region.       */
  uint32_t         count_of_clusters;   /**< Per MS spec: data_sectors / SPC.       */
  uint32_t         partition_base_lba;  /**< MBR partition start (0 = superfloppy). */
  uint8_t          in_use;              /**< 0 = slot free, 1 = mounted.            */
  uint8_t          exfat_upcase_ok;     /**< exFAT: volume's up-case table is ours. */
} ra8_fs_mount_t;

/**
 * @struct ra8_fs_file_t
 * @brief Open-file state.
 *
 * @details The first block is common to both filesystems. The `entry_set_*`,
 * `alloc_clusters`, `tail_cluster` and `valid_bytes` fields carry the extra
 * bookkeeping an exFAT stream needs and are meaningless on a FAT12/16/32
 * handle: exFAT locates a file by a directory ENTRY SET rather than one
 * 32-byte entry, tracks its allocation separately from its length (the
 * allocation bitmap, not a FAT chain, is authoritative), and distinguishes
 * the bytes actually written (`ValidDataLength`) from the file's length
 * (`DataLength`).
 *
 * @invariant `valid_bytes <= size_bytes` on an exFAT handle.
 * @invariant `alloc_clusters * cluster_bytes >= size_bytes` on an exFAT handle.
 * @invariant `no_fat_chain != 0` implies the allocation is the contiguous run
 *            `[first_cluster, first_cluster + alloc_clusters)`.
 * @see ra8_fs_open()
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_mount_t* mount;              /**< Owning mount point.                        */
  uint32_t        first_cluster;      /**< Head of the file's cluster chain.          */
  uint32_t        cur_cluster;        /**< Cluster the offset currently points into.  */
  uint32_t        walk_cache_idx;     /**< Chain index whose cluster is cached below. */
  uint32_t        walk_cache_cluster; /**< Cluster at walk_cache_idx; < 2 = no cache. */
  uint32_t        size_bytes;         /**< File size (DIR_FileSize / DataLength).     */
  uint32_t        offset;             /**< Current read/write offset.                 */
  uint32_t        dir_entry_lba;      /**< FAT: sector containing the dir entry.      */
  uint32_t        dir_entry_idx;      /**< FAT: byte offset of the entry in it.       */
  uint32_t        valid_bytes;        /**< exFAT ValidDataLength (bytes written).     */
  uint32_t        entry_set_cluster;  /**< exFAT: dir cluster of the File entry.      */
  uint32_t        entry_set_index;    /**< exFAT: entry index of the File entry.      */
  uint32_t        entry_set_count;    /**< exFAT: 1 + SecondaryCount.                 */
  uint32_t        alloc_clusters;     /**< exFAT: clusters currently allocated.       */
  uint32_t        tail_cluster;       /**< exFAT: last cluster of the allocation.     */
  ra8_fs_mode_t   mode;               /**< Open mode.                                 */
  uint8_t         in_use;             /**< 0 = slot free, 1 = open.                   */
  uint8_t         no_fat_chain;       /**< exFAT contiguous file (no FAT walk).       */
  uint8_t         dirty;              /**< 1 once written; drives the close mtime.    */
} ra8_fs_file_t;

/**
 * @struct ra8_fs_stat_t
 * @brief What ::ra8_fs_stat() read out of a directory entry.
 *
 * @details
 * The on-disk facts about one name, reported without opening it. That
 * distinction is the point of the struct: a directory has no file handle to
 * open and a metadata query must not consume one of the four file slots, so
 * everything here comes from the directory entry itself.
 *
 * @invariant `is_directory` is exactly `(attr & k_ra8_fs_attr_directory) != 0`.
 * @invariant `size_bytes` is 0 whenever `is_directory` is true -- a FAT
 *            directory's `DIR_FileSize` is defined to be 0, and its real extent
 *            is its cluster chain.
 *
 * @par Example:
 * @code
 * ra8_fs_stat_t st = {};
 * if (ra8_fs_stat(mnt, "/books", &st) == k_ra8_ok && st.is_directory) {
 *   (void)ra8_fs_listdir(mnt, "/books", on_entry, nullptr);
 * }
 * @endcode
 *
 * @see ra8_fs_stat()  Fills this in.
 * @since 0.1.0
 */
typedef struct {
  uint32_t size_bytes;    /**< File length in bytes; 0 for a directory.        */
  uint32_t first_cluster; /**< Head of the entry's cluster chain (0 if empty). */
  uint8_t  attr;          /**< The entry's own FAT attribute byte.             */
  bool     is_directory;  /**< true => the ATTR_DIRECTORY bit is set.          */
} ra8_fs_stat_t;

/* =============================================================================
 * Listdir callback
 * =============================================================================
 */

/**
 * @typedef ra8_fs_listdir_cb_t
 * @brief Callback invoked once per directory entry by `ra8_fs_listdir()`.
 *
 * @param[in] name NUL-terminated file name (8.3 on FAT volumes; the
 *                 exFAT name, truncated to the driver buffer).
 * @param[in] attr `ra8_fs_attr_t` attribute byte.
 * @param[in] size File size in bytes (0 for directories).
 * @param[in] ctx  Caller-provided cookie.
 */
typedef void (*ra8_fs_listdir_cb_t)(const char* name, uint8_t attr, uint32_t size, void* ctx);

/* =============================================================================
 * Partition selector
 * =============================================================================
 */

/**
 * @enum ra8_fs_partition_sel_t
 * @brief Sentinel partition index requesting auto-selection.
 *
 * @details A disk carries at most four MBR primary partitions (indices 0-3) or
 * a GPT entry array (indices 0 .. entry-count-1, bounded at 128). Any of those
 * is a valid `index` for ::ra8_fs_mount_partition(); this sentinel instead asks
 * for the same first-partition auto-selection ::ra8_fs_mount() performs.
 *
 * @invariant Larger than any addressable partition index, so it can never
 *            collide with a real one.
 * @see ra8_fs_mount_partition()
 */
typedef enum : uint8_t {
  k_ra8_fs_partition_auto = 0xFFU, /**< Auto-select the first mountable partition. */
} ra8_fs_partition_sel_t;

#ifdef __cplusplus
}
#endif
