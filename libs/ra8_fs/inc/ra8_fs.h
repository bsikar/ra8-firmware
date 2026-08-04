/**
 * @file ra8_fs.h
 * @brief Minimal FAT12/FAT16/FAT32 filesystem adapter (read + write).
 * @ingroup grp_storage
 *
 * @details
 * `ra8_fs` is a self-contained, pure-C FAT filesystem implementation that sits
 * on top of an arbitrary block-device backend (`ra8_fs_backend_t`). It mirrors
 * the public shape of Renesas FSP's `rm_filex` / FileX adapter but does not
 * carry the FileX or FSP dependencies.
 *
 * Two backends are intended for production use:
 *   1. `ra8_sdhi` -- on-board SD/MMC card (sweep 1).
 *   2. `ra8_usb_pmsc` storage backend -- mass-storage gadget loopback (sweep 2).
 *
 * Any object that supplies `read_block()`, `write_block()`, and
 * `get_capacity()` works -- including the in-memory mock used in
 * `tests/test_ra8_fs.c`.
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
 *     free-cluster scan when the current cluster fills. The scan starts at a
 *     per-mount next-free hint and reads the FAT through a one-sector cache,
 *     so appending to a file costs a bounded number of block reads per
 *     cluster instead of rescanning the whole FAT.
 *   - FAT32 FSInfo: validated at mount (both signatures plus the trailing
 *     signature), used to seed the free count and the next-free hint, and
 *     written back when a file is closed or the volume is unmounted.
 *   - Create / modify / access timestamps on FAT and exFAT. With no clock
 *     installed every stamp is the legal FAT epoch (1980-01-01 00:00:00);
 *     install one with `ra8_fs_set_clock()` to record real time.
 *   - Seek / tell / size.
 *   - Listdir via callback.
 *   - Unlink (mark dir entry 0xE5, free chain) and rmdir of an empty
 *     directory. Both refuse the wrong kind of entry: `unlink` will not take
 *     a directory and `rmdir` will not take a file.
 *
 * ## What this deliberately skips
 *   - Long File Names (LFN, 0x0F attribute) on write -- short 8.3 only
 *     (LFN reads are matched).
 *   - exFAT directory creation and removal (`mkdir` / `rmdir` are FAT-only;
 *     both are supported there, including nested-path resolution).
 *   - Multi-partition MBR scanning (only partition 0 is followed; a
 *     superfloppy BPB at LBA 0 is still supported transparently).
 *
 * Limits (compile-time):
 *   - 4 concurrent open file handles (`k_ra8_fs_max_files`).
 *   - 2 concurrent mount points (`k_ra8_fs_max_mounts`).
 *   - File size up to 4 GiB - 1 (FAT32 maximum).
 *   - Sector size: 512 bytes (the only size we test against; BPB is
 *     validated to enforce this).
 *
 * ## Concurrency
 * The library owns three pieces of shared mutable state -- the file-handle
 * table, the mount table and one static scratch sector -- so every public
 * entry point is serialised against every other one, not merely against
 * itself. With no lock installed (the default) that serialisation is the
 * caller's job and the library behaves exactly as it always has: bare metal
 * pays nothing, not even a branch worth measuring.
 *
 * An RTOS-world caller installs a lock instead of duplicating that discipline
 * at every call site -- see ::ra8_fs_lock_t and ::ra8_fs_set_lock(). The lock
 * is taken at the public boundary and released on every return path, including
 * error returns; no internal helper takes it, so there is no lock-ordering
 * graph and no recursion. It serialises the *library*: it does not make one
 * open ::ra8_fs_file_t safe to drive from two threads at once.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs_seams.h"

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
  k_ra8_fs_type_unknown = 0,  /**< Not yet detected / mount failed.          */
  k_ra8_fs_type_fat12   = 12, /**< count_of_clusters < 4085.                 */
  k_ra8_fs_type_fat16   = 16, /**< 4085 <= count_of_clusters < 65525.        */
  k_ra8_fs_type_fat32   = 32, /**< count_of_clusters >= 65525.               */
  k_ra8_fs_type_exfat   = 64, /**< exFAT (read + whole-file write + format). */
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
} ra8_fs_mount_t;

/**
 * @struct ra8_fs_file_t
 * @brief Open-file state.
 */
typedef struct {
  ra8_fs_mount_t* mount;              /**< Owning mount point.                        */
  uint32_t        first_cluster;      /**< Head of the file's cluster chain.          */
  uint32_t        cur_cluster;        /**< Cluster the offset currently points into.  */
  uint32_t        walk_cache_idx;     /**< Chain index whose cluster is cached below. */
  uint32_t        walk_cache_cluster; /**< Cluster at walk_cache_idx; < 2 = no cache. */
  uint32_t        size_bytes;         /**< File size (DIR_FileSize).                  */
  uint32_t        offset;             /**< Current read/write offset.                 */
  uint32_t        dir_entry_lba;      /**< Sector containing the dir entry.           */
  uint32_t        dir_entry_idx;      /**< Byte offset of dir entry within sector.    */
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
 * Public API -- format (mkfs)
 * =============================================================================
 */

/**
 * @brief Format a block device as a fresh, empty FAT12/FAT16/FAT32/exFAT volume.
 *
 * @details
 * Lays down a complete volume so the very next `ra8_fs_mount()` on the same
 * backend detects exactly `opts->type`. The FAT variants are written as a
 * superfloppy (no MBR) at LBA 0; exFAT is written into an MBR partition (see
 * below). For the FAT variants this is a classic BPB layout:
 *   - a full BPB (jump prologue, OEM name, `BytsPerSec`, `SecPerClus`,
 *     `RsvdSecCnt`, `NumFATs`, `RootEntCnt` or `RootClus`, `TotSec`, media
 *     descriptor, `FATSz`, volume label, filesystem-type string, and the
 *     `0x55 0xAA` boot signature);
 *   - the reserved `FAT[0]` media descriptor and `FAT[1]` end-of-chain marker
 *     in every FAT copy (plus the root-cluster EOC and an `FSInfo` sector for
 *     FAT32);
 *   - a zeroed root directory (FAT12/16 fixed root, or the FAT32 root cluster).
 *
 * The cluster size is chosen (when `opts->sectors_per_cluster == 0`) so the
 * resulting `count_of_clusters` lands in the band the requested type requires
 * per Microsoft "FAT: General Overview of On-Disk Format" sec 3.5
 * (FAT12 < 4085, 4085 <= FAT16 < 65525, FAT32 >= 65525). If the backend's
 * capacity cannot satisfy `opts->type` -- too small for FAT16/FAT32, or too
 * large for FAT12/FAT16 even at the maximum cluster size -- the call fails
 * without writing anything.
 *
 * exFAT (`k_ra8_fs_type_exfat`) is written the way a PC writes it, so a card
 * formatted here mounts on a desktop: a DOS/MBR partition table at LBA 0 with
 * one type-0x07 partition aligned at 1 MiB, and the volume itself inside that
 * partition rather than at LBA 0. `ra8_fs_mount()` follows the partition table
 * back (`ra8_fs_mount_t::partition_base_lba` records where it landed), and also
 * mounts a card partitioned by a desktop. Because the partition cannot start at
 * sector 0, the device must be big enough for the 1 MiB alignment gap ON TOP OF
 * the 32 MiB minimum volume -- a 32 MiB card is no longer formattable.
 *
 * Inside the partition exFAT lays down its own on-disk structures instead of a
 * FAT BPB: the 12-sector boot region (Main + Backup) with the VBR checksum
 * sector, the single FAT, the allocation-bitmap cluster(s), the canonical
 * compressed up-case table (which may span several clusters), and a
 * root-directory cluster carrying the volume-label, allocation-bitmap, and
 * up-case-table system directory entries. The image is `fsck.exfat`-clean and
 * round-trips through `ra8_fs_mount()` plus the exFAT file API: whole-file
 * creation via `ra8_fs_write_file()`, read-back via `ra8_fs_open()` (read) +
 * `ra8_fs_read()`, `ra8_fs_rename()`, and `ra8_fs_unlink()` (exFAT has no
 * streaming open-for-write; see `ra8_fs_open()`).
 *
 * @param[in] backend Block-device implementation (read/write/get_capacity all
 *                    non-NULL). Its `write_block` is driven during the format.
 * @param[in] opts    Format options. `type` selects the variant; `label` is an
 *                    optional 0..11 char volume label; `sectors_per_cluster`
 *                    pins the cluster size (0 = auto-select).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Volume formatted; ready to mount.
 * @retval k_ra8_err_null_ptr          `backend` or `opts` is NULL.
 * @retval k_ra8_err_invalid_arg       Backend has NULL callbacks, the reported
 *                                    block size is not 512, or
 *                                    `sectors_per_cluster` is non-zero and not
 *                                    a power of two in 1..128.
 * @retval k_ra8_err_not_supported     `opts->type` is unknown / not a writable
 *                                    filesystem, or the device is too small to
 *                                    hold a partitioned exFAT volume (the 1 MiB
 *                                    alignment gap plus the 32 MiB minimum).
 * @retval k_ra8_err_invalid_size      Capacity cannot satisfy `opts->type`.
 * @retval k_ra8_err_*                 Backend read/write failure (volume may be
 *                                    partially written on a mid-format I/O
 *                                    error).
 *
 * @pre `backend->write_block` and `backend->get_capacity` are non-NULL.
 * @pre No volume from this backend is currently mounted (the caller has
 *      unmounted it first; formatting under a live mount corrupts cached
 *      geometry).
 * @post On success the backend's first sectors hold a valid `opts->type` BPB,
 *       FAT, and an empty root directory.
 * @post On `k_ra8_err_invalid_size` / argument errors, the backend is untouched.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 * @warning Destroys all data on the device.
 *
 * @par Example:
 * @code
 * ra8_fs_format_opts_t opts = {};
 * opts.type  = k_ra8_fs_type_fat16;
 * opts.label = "SCRATCH";
 * if (ra8_fs_format(&backend, &opts) == k_ra8_ok) {
 *   ra8_fs_mount_t* mnt = nullptr;
 *   (void)ra8_fs_mount(&backend, &mnt);  // mnt->type == k_ra8_fs_type_fat16
 * }
 * @endcode
 *
 * @see ra8_fs_mount()  Detects the type this routine wrote.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_format(const ra8_fs_backend_t*     backend,
                                      const ra8_fs_format_opts_t* opts);

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
 * @return ra8_err_t
 * @retval k_ra8_ok                   Volume mounted successfully.
 * @retval k_ra8_err_null_ptr         backend or out_handle is NULL.
 * @retval k_ra8_err_invalid_arg      Backend has NULL function pointers.
 * @retval k_ra8_err_no_mem           No free mount slot.
 * @retval k_ra8_err_validation_failed BPB signature invalid / unsupported.
 *
 * @pre `backend->read_block` is non-NULL.
 * @pre `out_handle` is non-NULL.
 * @post On success, `out_handle->in_use == 1` and `type != unknown`.
 * @post On failure, `*out_handle` is left untouched.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_mount(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_handle);

/**
 * @brief Unmount a previously mounted volume and release its slot.
 *
 * @details On a FAT32 volume carrying a valid FSInfo sector this is also where
 *          the free-cluster count and next-free hint are written back, so a
 *          card the firmware wrote reports the right free space on a desktop
 *          and `fsck.fat` stops calling the summary wrong. The slot is
 *          released even when that write fails -- an unmount that could be
 *          refused would be worse than a stale free count.
 *
 * @param[in,out] handle Mount handle returned by `ra8_fs_mount()`.
 *
 * @retval k_ra8_ok           Slot released.
 * @retval k_ra8_err_null_ptr handle is NULL.
 * @retval k_ra8_err_invalid_state Slot was not in use.
 * @retval k_ra8_err_*        FSInfo writeback failed; the slot is still released.
 *
 * @pre handle was returned by ra8_fs_mount.
 * @post handle->in_use == 0.
 * @post Any pending FSInfo update has been attempted exactly once.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_unmount(ra8_fs_mount_t* handle);

/* =============================================================================
 * Public API -- file ops
 * =============================================================================
 */

/**
 * @brief Open or create a file by 8.3 short path (root dir only).
 *
 * @details A path that resolves to a DIRECTORY is rejected in every mode. In
 * write mode that rejection is load-bearing: the truncate step would free the
 * cluster chain holding the directory's contents and leave every file inside it
 * unreachable. In read mode it prevents handing back the zero-byte handle a
 * directory's `DIR_FileSize` of 0 would otherwise describe.
 *
 * @param[in]  handle    Mount handle.
 * @param[in]  path      8.3 path, e.g. "HELLO.TXT". Subdirectories not supported.
 * @param[in]  mode      `k_ra8_fs_mode_read`, `_write`, or `_append`.
 * @param[out] out_file  Populated file handle on success.
 *
 * @retval k_ra8_ok                     File opened.
 * @retval k_ra8_err_null_ptr           Any pointer arg is NULL.
 * @retval k_ra8_err_invalid_arg        `path` names a directory (any mode), or
 *                                     a new file's name is not 8.3.
 * @retval k_ra8_err_not_found          Read mode and path doesn't exist.
 * @retval k_ra8_err_no_mem             No free file slot.
 * @retval k_ra8_err_no_data            Write mode failed to allocate cluster.
 * @retval k_ra8_err_not_supported      Write/append mode on an exFAT mount: exFAT
 *                                     opens read-only via this seam. Create exFAT
 *                                     files with `ra8_fs_write_file()` instead.
 *
 * @pre handle->in_use == 1.
 * @post On success, out_file->in_use == 1 and offset is at file start (read/write)
 *       or end (append).
 * @post On `k_ra8_err_invalid_arg` the volume is unchanged.
 * @note On FAT12/16/32 all three modes work; exFAT accepts only
 *       `k_ra8_fs_mode_read` here (whole-file writes go through
 *       `ra8_fs_write_file()`).
 * @see ra8_fs_rmdir()  The verb for removing a directory.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_open(ra8_fs_mount_t* handle, const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file);

/**
 * @brief Close an open file, stamping its final modification time.
 *
 * @details File contents are never buffered -- `ra8_fs_write()` has already
 *          put every byte on the volume -- so what closing does is metadata.
 *          A handle that was written through gets `DIR_WrtTime` /
 *          `DIR_WrtDate` set to the moment of the close (the time a host shows
 *          as "modified" and a sync tool compares), and the volume's FAT32
 *          FSInfo free count is committed. A handle opened read-only, or
 *          write-opened and never written, touches nothing.
 *
 * @param[in,out] file Handle from `ra8_fs_open()`.
 *
 * @retval k_ra8_ok           Closed.
 * @retval k_ra8_err_null_ptr file is NULL.
 * @retval k_ra8_err_*        The timestamp or FSInfo write failed. The handle
 *                            is released anyway -- a close that could be
 *                            refused would leak the slot.
 * @post The file slot is free for reuse whatever the metadata write did.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_close(ra8_fs_file_t* file);

/**
 * @brief Read up to `max_len` bytes; advance the cluster chain on cluster crossings.
 *
 * @param[in]  file    Open file (any mode).
 * @param[out] buf     Destination buffer.
 * @param[in]  max_len Maximum bytes to copy.
 * @param[out] got_len Actual bytes read (0 at EOF).
 *
 * @retval k_ra8_ok                Read completed (got_len may be 0 = EOF).
 * @retval k_ra8_err_null_ptr      Any pointer arg is NULL.
 * @retval k_ra8_err_invalid_state file->in_use == 0.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_read(ra8_fs_file_t* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len);

/**
 * @brief Write `len` bytes; allocate new clusters from FAT free-space scan as needed.
 *
 * @details New clusters come from a scan that starts at the mount's next-free
 *          hint rather than at cluster 2, reading the FAT through a one-sector
 *          cache, so appending stays linear in the bytes written instead of
 *          quadratic. The directory entry's modification time advances with
 *          every call, and again when the handle is closed.
 *
 * @retval k_ra8_ok                Wrote all bytes; size + dir entry updated.
 * @retval k_ra8_err_null_ptr      file or buf NULL.
 * @retval k_ra8_err_invalid_state file not opened for writing.
 * @retval k_ra8_err_no_mem        Volume out of free clusters.
 * @post On success the entry's `DIR_WrtTime` / `DIR_WrtDate` name this write.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_write(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len);

/**
 * @brief Create a whole file in one call (provisioning helper).
 *
 * @details Convenience wrapper that creates @p path and writes @p data. On FAT
 * volumes it opens in write mode, writes, and closes. On exFAT it releases any
 * existing entry set for the name, then allocates a contiguous cluster run and
 * links a fresh directory entry (the only exFAT write path).
 *
 * An existing @p path is REPLACED on both filesystems: its old contents are
 * discarded and its clusters returned to the volume's free space. Calling this
 * twice with the same name leaves exactly one file, of the second call's
 * contents, with no space lost to the first.
 *
 * @param[in] handle Mounted volume.
 * @param[in] path   Flat root-level file name (ASCII).
 * @param[in] data   File contents.
 * @param[in] len    Byte count (> 0).
 *
 * @retval k_ra8_ok              File created and written.
 * @retval k_ra8_err_null_ptr    Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg Empty/oversized name, zero @p len, or @p path
 *                               names an existing directory.
 * @retval k_ra8_err_no_mem      Out of contiguous space or directory slots.
 * @retval k_ra8_err_*           Backend error.
 *
 * @pre No handle is open on @p path.
 * @post On success @p path resolves to exactly one entry of @p len bytes.
 * @post On success a replaced predecessor's clusters read as free.
 * @note Not atomic: a replaced file is released before the new content is
 *       written, so a failure mid-write leaves @p path absent, not stale.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_write_file(ra8_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len);

/**
 * @brief Move the file offset to `offset_bytes` (clamped to size).
 * @retval k_ra8_ok            Seek committed.
 * @retval k_ra8_err_null_ptr  file is NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_seek(ra8_fs_file_t* file, uint32_t offset_bytes);

/** @brief Report the current offset. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_fs_tell(const ra8_fs_file_t* file, uint32_t* out_offset);

/** @brief Report the file's size in bytes. @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_fs_size(const ra8_fs_file_t* file, uint32_t* out_bytes);

/* =============================================================================
 * Public API -- metadata
 * =============================================================================
 */

/**
 * @brief Report what a path names -- file or directory -- without opening it.
 *
 * @details
 * Resolves @p path the same way ::ra8_fs_open() does (parent walk, 8.3 lookup,
 * VFAT long-name fallback on FAT; root-directory entry-set scan on exFAT) but
 * stops at the directory entry and reads the answer straight out of it. Nothing
 * is opened, so no file-table slot is consumed and a directory is reported as a
 * directory rather than as a zero-byte file -- which is what opening one would
 * have made it look like, `DIR_FileSize` being 0 by definition.
 *
 * A path naming the volume root (`""`, `"/"`) is answered from the mount
 * geometry: it always exists and is always a directory.
 *
 * @param[in]  handle Mount handle.
 * @param[in]  path   NUL-terminated path. Nested paths resolve on FAT12/16/32;
 *                    exFAT resolves root-level names only.
 * @param[out] out    Receives the entry's metadata on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Entry found; @p out populated.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_not_found     Nothing at @p path (or an intermediate
 *                                 component is missing).
 * @retval k_ra8_err_invalid_arg   A path component is not a valid 8.3 name.
 * @retval k_ra8_err_*             Backend read failure.
 *
 * @pre `handle`, `path` and `out` are non-NULL.
 * @pre Mount is in use.
 * @post On ::k_ra8_ok, `out->is_directory` matches the entry's ATTR_DIRECTORY
 *       bit and `out->size_bytes` is 0 whenever it is set.
 * @post No volume state is modified and no file slot is consumed, on any path.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @par Example:
 * @code
 * ra8_fs_stat_t st = {};
 * const ra8_err_t e = ra8_fs_stat(mnt, "/README.TXT", &st);
 * // e == k_ra8_err_not_found means "no such name", not "I/O failed".
 * @endcode
 *
 * @see ra8_fs_listdir()  Enumerate a directory this reported.
 * @see ra8_fs_open()     Same resolution, but takes a handle.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_stat(ra8_fs_mount_t* handle, const char* path, ra8_fs_stat_t* out);

/* =============================================================================
 * Public API -- directory ops
 * =============================================================================
 */

/**
 * @brief Enumerate directory entries; invoke `cb` once per visible entry.
 *
 * @details FAT12/16/32 enumerate any directory by path (`"/"` for the root,
 * `"/books"` for a subdirectory). exFAT enumeration remains root-only (`"/"`).
 *
 * @param[in] handle Mount handle.
 * @param[in] path   Directory path (`"/"` or a nested path on FAT).
 * @param[in] cb     Callback (must be non-NULL).
 * @param[in] ctx    Cookie forwarded to the callback.
 *
 * @retval k_ra8_ok                 Enumeration complete.
 * @retval k_ra8_err_null_ptr       handle/cb NULL.
 * @retval k_ra8_err_not_found      A path component does not exist.
 * @retval k_ra8_err_not_supported  exFAT path other than "/".
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_listdir(ra8_fs_mount_t* handle, const char* path, ra8_fs_listdir_cb_t cb, void* ctx);

/**
 * @brief Delete a file: mark its dir entry deleted and free its clusters.
 *
 * @details FAT12/16/32: 0xE5-marks the entry and frees the FAT chain.
 * exFAT: clears the in-use bit on the whole entry set and frees the
 * clusters in the allocation bitmap (the bitmap is authoritative).
 *
 * A DIRECTORY is refused on both filesystems. Deleting one this way would free
 * the chain that holds its children while their own entries still claim their
 * clusters, leaving them allocated and unreachable -- lost clusters that only a
 * reformat recovers. Use `ra8_fs_rmdir()`.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated path to the file.
 *
 * @retval k_ra8_ok               File unlinked.
 * @retval k_ra8_err_null_ptr     handle/path NULL.
 * @retval k_ra8_err_invalid_arg  `path` names a directory, or is not 8.3.
 * @retval k_ra8_err_not_found    File doesn't exist.
 *
 * @pre `handle` and `path` are non-NULL; the mount is in use.
 * @pre No open file handle refers to `path`.
 * @post On success the name no longer resolves and its clusters are free.
 * @post On `k_ra8_err_invalid_arg` the volume is unchanged.
 *
 * @note Not thread-safe; callers serialise.
 * @see ra8_fs_rmdir()  Removes a directory instead.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_unlink(ra8_fs_mount_t* handle, const char* path);

/**
 * @brief Rename a root-level file in place.
 *
 * @details FAT12/16/32: rewrites the 11-byte packed 8.3 name in the
 * existing directory entry. exFAT: patches the Stream entry's NameLength
 * and NameHash, rebuilds the Name entry, and recomputes the SetChecksum --
 * supported when both names fit one Name entry (<= 15 characters), which
 * keeps the entry-set length unchanged. Data clusters never move.
 *
 * The LAST-ACCESS stamp advances; the modification stamp deliberately does
 * not. A rename changes the name, not the bytes, so moving the modification
 * time would tell every `rsync`, backup and "pick the newest image" heuristic
 * that the contents changed. Neither format has a metadata-change field, so
 * the access stamp is the honest record that the entry was touched.
 *
 * @param[in] handle   Mount handle.
 * @param[in] old_path Existing root-level name.
 * @param[in] new_path Replacement name (must not already exist).
 *
 * @retval k_ra8_ok                File renamed.
 * @retval k_ra8_err_null_ptr      Any pointer arg is NULL.
 * @retval k_ra8_err_not_found     @p old_path does not exist.
 * @retval k_ra8_err_exists        @p new_path already resolves.
 * @retval k_ra8_err_not_supported exFAT name longer than 15 characters.
 * @pre The file is not open.
 * @post On success @p new_path resolves to the same data.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_rename(ra8_fs_mount_t* handle, const char* old_path, const char* new_path);

/**
 * @brief Create a directory at @p path (FAT12/16/32).
 *
 * @details Resolves all-but-the-last path component to an existing parent
 * directory, then creates the final component as a new, empty subdirectory with
 * "." and ".." links. Nested paths are supported (`"/books/scifi"`), provided
 * each intermediate component already exists and every component is an 8.3 name.
 * exFAT directory creation is not supported. A partial allocation is rolled
 * back on failure, so the volume is never leaked.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to create.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_null_ptr      `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   The leaf is not a valid 8.3 name.
 * @retval k_ra8_err_exists        The name already exists in the parent.
 * @retval k_ra8_err_not_found     An intermediate component does not exist.
 * @retval k_ra8_err_no_mem        Parent directory or volume is full.
 * @retval k_ra8_err_not_supported The volume is exFAT.
 *
 * @pre `handle` and `path` are non-NULL; the parent path exists.
 * @pre Mount is in use.
 * @post On success an empty directory exists at @p path.
 * @post On failure the volume is unchanged.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_mkdir(ra8_fs_mount_t* handle, const char* path);

/**
 * @brief Remove an empty directory at @p path (FAT12/16/32).
 *
 * @details The symmetric partner of `ra8_fs_mkdir()`: resolves all-but-the-last
 * path component to an existing parent, requires the final component to be an
 * existing directory, requires that directory to hold nothing but its own "."
 * and ".." links, then frees its cluster chain and marks its entry in the parent
 * deleted. Nested paths are supported (`"/books/scifi"`).
 *
 * The emptiness proof runs before anything is freed, so a refusal costs the
 * volume nothing. Deleted (0xE5) slots and orphaned long-name remnants do not
 * count as contents -- `ra8_fs_unlink()` leaves the latter behind -- so a
 * directory whose files have all been unlinked is removable.
 *
 * The volume root is not removable, and neither is a file: use `ra8_fs_unlink()`
 * for those. exFAT is not supported, matching `ra8_fs_mkdir()`.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to remove.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Directory removed.
 * @retval k_ra8_err_null_ptr       `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state  Mount is not in use.
 * @retval k_ra8_err_invalid_arg    `path` is the root, is not a valid 8.3 name,
 *                                  or names a file rather than a directory.
 * @retval k_ra8_err_not_found      No such entry, or a component is missing.
 * @retval k_ra8_err_not_empty      The directory still holds entries.
 * @retval k_ra8_err_protocol_error Corrupt chain, or a directory entry that
 *                                  claims no data cluster.
 * @retval k_ra8_err_not_supported  The volume is exFAT.
 *
 * @pre `handle` and `path` are non-NULL; the mount is in use.
 * @pre No open file handle refers to an entry inside @p path.
 * @post On success @p path no longer resolves and its cluster is free.
 * @post On any refusal (wrong type, non-empty, root) the volume is unchanged.
 *
 * @note An open handle to a file inside @p path cannot be orphaned by this
 *       call: that file's directory entry still exists, so the emptiness check
 *       refuses the removal first.
 * @note Not thread-safe; callers serialise.
 *
 * @par Example:
 * @code
 * ra8_err_t e = ra8_fs_rmdir(mnt, "/logs");
 * if (e == k_ra8_err_not_empty) {
 *   // unlink the contents first, then retry
 * }
 * @endcode
 *
 * @see ra8_fs_mkdir()   Creates the directory this removes.
 * @see ra8_fs_unlink()  Removes a file instead.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_rmdir(ra8_fs_mount_t* handle, const char* path);

#ifdef __cplusplus
}
#endif
