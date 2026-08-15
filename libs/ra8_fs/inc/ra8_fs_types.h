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
  k_ra8_fs_max_files      = 4,  /**< Max concurrent open file handles.    */
  k_ra8_fs_max_mounts     = 2,  /**< Max concurrent mount points.         */
  k_ra8_fs_short_name_len = 12, /**< 8.3 short name "AAAAAAAA.EXT" + NUL. */
} ra8_fs_limits_t;

/**
 * @enum ra8_fs_byte_sizes_t
 * @brief Byte-size constants used by the FAT layout.
 *
 * @details The sector size is a RUNTIME property of the mounted medium
 *          (`ra8_fs_mount_t::bytes_per_sector`), taken from the backend's
 *          reported block size and cross-checked against the volume's BPB /
 *          VBR at mount. These two constants bound it: every supported size
 *          is a power of two in `[k_ra8_fs_sector_min, k_ra8_fs_sector_max]`
 *          (512, 1024, 2048, 4096), which is exactly the range the FAT
 *          specification allows for `BPB_BytsPerSec` and exFAT allows for
 *          `BytesPerSectorShift` (9..12). Static sector buffers are sized to
 *          the maximum so a 4Kn medium needs no allocation the 512-byte path
 *          did not already have.
 */
typedef enum : uint16_t {
  k_ra8_fs_sector_min      = 512,  /**< Smallest supported sector size.      */
  k_ra8_fs_sector_max      = 4096, /**< Largest supported sector size (4Kn). */
  k_ra8_fs_dir_entry_bytes = 32,   /**< MS FAT spec sec 6 "Directory Entry". */
  k_ra8_fs_dir_name_cap    = 742,  /**< Longest listed UTF-8 name plus NUL.  */
  k_ra8_fs_dir_state_bytes = 640,  /**< Opaque caller-owned cursor state.    */
} ra8_fs_byte_sizes_t;

/**
 * @enum ra8_fs_fat_limit_t
 * @brief The FAT12/16/32 per-file size ceiling.
 *
 * @details `DIR_FileSize` is a 32-bit field (MS FAT spec sec 6), so no FAT
 *          file can exceed 4 GiB - 1 bytes. The ceiling is inherent to the
 *          format, not to this driver: a write or truncate that would push a
 *          FAT file past it fails with ::k_ra8_err_invalid_size at the FAT
 *          boundary. exFAT files carry 64-bit lengths and are not subject to
 *          this cap.
 */
typedef enum : uint32_t {
  k_ra8_fs_fat_max_file_bytes = 0xFFFFFFFFU, /**< 4 GiB - 1: max DIR_FileSize. */
} ra8_fs_fat_limit_t;

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
 * LBAs and the block count are 64-bit so media past the 32-bit-LBA reach
 * (2 TiB at 512-byte blocks) are addressable end to end -- through the mount's
 * partition base, the GPT parser and every cluster computation (#683). The
 * block size is the medium's real sector size: 512-byte and 4096-byte-native
 * (4Kn) devices are both supported, 1024/2048 included.
 *
 * @warning The 4Kn and beyond-2-TiB paths are verified by host-side simulation
 *          against fake backends only -- no such medium has been on the bench.
 *          The 512-byte, sub-2-TiB path is the hardware-proven one.
 *
 * @invariant `read_block`, `write_block`, `get_capacity` are non-NULL for
 *            a usable backend. `erase_blocks` and `ctx` may be NULL.
 */
typedef struct {
  /**
   * @brief Read `count` consecutive blocks starting at `lba`.
   * @param[in]  ctx   Backend cookie.
   * @param[in]  lba   Logical block address (0 = BPB).
   * @param[in]  count Number of blocks to read.
   * @param[out] buf   Destination buffer of at least `count * block_size` bytes.
   */
  ra8_err_t (*read_block)(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf);

  /**
   * @brief Write `count` consecutive blocks starting at `lba`.
   */
  ra8_err_t (*write_block)(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf);

  /**
   * @brief Report the device size.
   * @param[out] block_count Total blocks (sectors).
   * @param[out] block_size  Bytes per block: a power of two in
   *                         `k_ra8_fs_sector_min..k_ra8_fs_sector_max`.
   */
  ra8_err_t (*get_capacity)(void* ctx, uint64_t* block_count, uint32_t* block_size);

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
  ra8_err_t (*erase_blocks)(void* ctx, uint64_t lba, uint64_t count);

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
  ra8_fs_backend_t backend;             /**< Block-device backend.                      */
  ra8_fs_type_t    type;                /**< FAT12 / FAT16 / FAT32.                     */
  uint32_t         bytes_per_sector;    /**< Sector size (BPB / VBR == backend).        */
  uint32_t         sectors_per_cluster; /**< BPB BPB_SecPerClus.                        */
  uint32_t         reserved_sectors;    /**< BPB BPB_RsvdSecCnt.                        */
  uint32_t         num_fats;            /**< BPB BPB_NumFATs.                           */
  uint32_t         root_entries;        /**< BPB BPB_RootEntCnt (FAT12/16).             */
  uint64_t         total_sectors;       /**< BPB TotSec / exFAT VolumeLength.           */
  uint32_t         fat_size_sectors;    /**< BPB BPB_FATSz16 / BPB_FATSz32.             */
  uint32_t         root_cluster;        /**< BPB BPB_RootClus (FAT32 only).             */
  uint64_t         first_fat_lba;       /**< Computed: first FAT sector.                */
  uint64_t         first_root_lba;      /**< FAT12/16 fixed root-dir start.             */
  uint64_t         first_data_lba;      /**< First sector of the data region.           */
  uint32_t         count_of_clusters;   /**< Per MS spec: data_sectors / SPC.           */
  uint64_t         partition_base_lba;  /**< MBR/GPT partition start (0 = superfloppy). */
  uint8_t          in_use;              /**< 0 = slot free, 1 = mounted.                */
  uint8_t          exfat_upcase_ok;     /**< exFAT: volume's up-case table is ours.     */
} ra8_fs_mount_t;

/** @brief Stable directory-entry value copied by ::ra8_fs_dir_next. */
typedef struct {
  char     name[k_ra8_fs_dir_name_cap]; /**< NUL-terminated visible leaf. */
  uint64_t size_bytes;                  /**< File bytes; zero for dirs.   */
  uint8_t  attr;                        /**< On-disk FAT attributes.      */
} ra8_fs_dirent_t;

/** @brief Caller-owned opaque directory cursor. */
typedef struct {
  alignas(uint64_t) uint8_t state[k_ra8_fs_dir_state_bytes]; /**< Private cursor state. */
  bool is_open; /**< Lifecycle guard owned by the facade. */
} ra8_fs_dir_t;

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
 * The three byte lengths are 64-bit because exFAT's on-disk `DataLength` /
 * `ValidDataLength` are (#676): a file past 4 GiB is exactly what exFAT
 * exists to carry. On FAT12/16/32 the same fields never exceed
 * ::k_ra8_fs_fat_max_file_bytes -- `DIR_FileSize` is 32-bit -- and the write
 * and truncate paths enforce that cap rather than wrapping.
 *
 * @invariant `valid_bytes <= size_bytes` on an exFAT handle.
 * @invariant `alloc_clusters * cluster_bytes >= size_bytes` on an exFAT handle.
 * @invariant `no_fat_chain != 0` implies the allocation is the contiguous run
 *            `[first_cluster, first_cluster + alloc_clusters)`.
 * @invariant `size_bytes <= k_ra8_fs_fat_max_file_bytes` on a FAT handle.
 * @see ra8_fs_open()
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_mount_t* mount;              /**< Owning mount point.                        */
  uint32_t        first_cluster;      /**< Head of the file's cluster chain.          */
  uint32_t        cur_cluster;        /**< Cluster the offset currently points into.  */
  uint32_t        walk_cache_idx;     /**< Chain index whose cluster is cached below. */
  uint32_t        walk_cache_cluster; /**< Cluster at walk_cache_idx; < 2 = no cache. */
  uint64_t        size_bytes;         /**< File size (DIR_FileSize / DataLength).     */
  uint64_t        offset;             /**< Current read/write offset.                 */
  uint64_t        dir_entry_lba;      /**< FAT: sector containing the dir entry.      */
  uint32_t        dir_entry_idx;      /**< FAT: byte offset of the entry in it.       */
  uint64_t        valid_bytes;        /**< exFAT ValidDataLength (bytes written).     */
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
 * @struct ra8_fs_datetime_t
 * @brief Broken-down civil date and time used at the filesystem boundary.
 *
 * @details This value is shared by the injected write clock and metadata read
 *          results. The fields are civil time; `utc_offset_min` says which
 *          zone they belong to when the on-disk format records one. FAT has no
 *          UTC-offset field, while exFAT does.
 *
 * @invariant A valid value has year 1980..2107, month 1..12, day 1..31,
 *            hour 0..23, minute/second 0..59, and centisecond 0..99.
 * @since 0.1.0
 */
typedef struct {
  uint16_t year;           /**< Full civil year, e.g. 2026.                      */
  int16_t  utc_offset_min; /**< Offset of the civil fields from UTC, in minutes. */
  uint8_t  month;          /**< Month of year, 1..12.                            */
  uint8_t  day;            /**< Day of month, 1..31.                             */
  uint8_t  hour;           /**< Hour of day, 0..23.                              */
  uint8_t  minute;         /**< Minute of hour, 0..59.                           */
  uint8_t  second;         /**< Second of minute, 0..59.                         */
  uint8_t  centisecond;    /**< Hundredths within `second`, 0..99.               */
} ra8_fs_datetime_t;

/**
 * @struct ra8_fs_timestamp_t
 * @brief One decoded on-disk timestamp plus availability facts.
 *
 * @details `valid` distinguishes a real decoded entry stamp from the volume
 *          root (which has no directory entry) or malformed third-party
 *          metadata. `utc_offset_valid` is independent because FAT stores a
 *          valid civil date without any zone, and exFAT may explicitly mark
 *          its offset unknown.
 *
 * @invariant `utc_offset_valid` implies `valid`.
 * @invariant When `valid` is false, `value` is all zero.
 * @since 0.1.0
 */
typedef struct {
  ra8_fs_datetime_t value;            /**< Decoded civil date/time.           */
  bool              valid;            /**< true => the on-disk date is legal. */
  bool              utc_offset_valid; /**< true => `utc_offset_min` is known. */
} ra8_fs_timestamp_t;

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
  uint64_t           size_bytes;    /**< File length in bytes; 0 for a directory.        */
  uint32_t           first_cluster; /**< Head of the entry's cluster chain (0 if empty). */
  ra8_fs_timestamp_t created;       /**< Creation timestamp, or invalid for the root.    */
  ra8_fs_timestamp_t modified;      /**< Last-modified timestamp.                        */
  ra8_fs_timestamp_t accessed;      /**< Last-accessed timestamp (date-only on FAT).     */
  uint8_t            attr;          /**< The entry's own FAT attribute byte.             */
  bool               is_directory;  /**< true => the ATTR_DIRECTORY bit is set.          */
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
 * @param[in] size File size in bytes (0 for directories; 64-bit because an
 *                 exFAT entry may exceed 4 GiB).
 * @param[in] ctx  Caller-provided cookie.
 */
typedef void (*ra8_fs_listdir_cb_t)(const char* name, uint8_t attr, uint64_t size, void* ctx);

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
