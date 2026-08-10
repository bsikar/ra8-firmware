/**
 * @file ra8_fs.h
 * @brief Minimal FAT12/FAT16/FAT32 filesystem adapter (read + write).
 * @ingroup grp_storage
 *
 * @details
 * `ra8_fs` is a self-contained, pure-C FAT filesystem implementation that sits
 * on top of an arbitrary block-device backend (`ra8_fs_backend_t`), with no
 * RTOS and no vendor-SDK dependencies. It is the platform's only filesystem:
 * the vendored FileX it once coexisted with was retired by #611.
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
 *   - VFAT long file names, read AND write: a name that is not
 *     8.3-representable is stored as a chain of attr-0x0F entries behind a
 *     generated `LONGNA~1.TXT` alias, and `unlink` / `rmdir` / `rename` take
 *     the chain away with the entry it belongs to. A name that differs from an
 *     8.3 one only in case costs no chain at all -- it travels in the two
 *     `DIR_NTRes` flags, so `data.log` reads back as `data.log`.
 *   - Linear file read across cluster boundaries.
 *   - File create + write, growing the chain by allocating from the FAT
 *     free-cluster scan when the current cluster fills. The scan starts at a
 *     per-mount next-free hint and reads the FAT through a one-sector cache,
 *     so appending to a file costs a bounded number of block reads per
 *     cluster instead of rescanning the whole FAT.
 *   - exFAT streaming write: open / create / append / truncate through the
 *     same `ra8_fs_open()` seam, growing out of the allocation bitmap one
 *     cluster at a time. The entry set keeps `NoFatChain` while the run stays
 *     contiguous and materialises a real FAT chain the moment it cannot, so a
 *     fragmented volume is written rather than refused, and `ValidDataLength`
 *     is tracked apart from `DataLength` so bytes past the written prefix
 *     read as zero the way the format requires.
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
 *   - Directories on exFAT too, not only on FAT: `mkdir`, `rmdir`, nested path
 *     resolution and per-directory listing. An exFAT directory is the same
 *     File + Stream + Name entry set a file gets, with the Directory attribute
 *     and a zeroed cluster behind it, and no "." / ".." entries -- exFAT has
 *     none.
 *   - GROWING a directory. A full directory takes another cluster out of the
 *     allocation bitmap, so its ceiling is free space, not one cluster: FAT
 *     subdirectories and the FAT32 root grow their chains, and an exFAT
 *     directory grows as its files do -- contiguous, then a FAT chain (#677).
 *
 * ## Names are UTF-8, on both formats
 * Every name crossing this API is UTF-8; both on-disk formats store UTF-16LE,
 * converted in one place so the three paths that did their own cannot disagree
 * again (#606).
 *
 *   - All of Unicode is storable: a 4-byte UTF-8 character becomes a surrogate
 *     PAIR on disk and returns as the same 4 bytes.
 *   - Malformed UTF-8 is REFUSED, never patched: an over-long encoding, a
 *     directly-encoded surrogate or a truncated sequence is `invalid_arg`.
 *   - Lookup is case-insensitive across the BMP, through the canonical
 *     up-case table this library writes at format time.
 *   - exFAT's stored `NameHash` uses that same table, which is what the format
 *     defines it over. A volume carrying a DIFFERENT table refuses non-ASCII
 *     names with `k_ra8_err_not_supported` instead; ASCII is unaffected.
 *   - Length limits are in UTF-16 UNITS: 247 on FAT, 64 on exFAT; a UTF-8
 *     argument may be three times as many bytes. An 8.3 short name stays ASCII,
 *     and a long name's alias maps what it cannot represent to `_`, as VFAT.
 *
 * ## What this deliberately skips
 *   - exFAT names past 64 UTF-16 units (the format allows 255), and
 *     locale-sensitive folding (Turkish dotless i, full case folding).
 *   - Extended / logical MBR partition chains: the four MBR primary entries
 *     and the GPT entry array are addressable by index through
 *     `ra8_fs_mount_partition()` (with `ra8_fs_mount()` auto-selecting the
 *     first, and a superfloppy BPB at LBA 0 still supported transparently),
 *     but an extended partition's logical chain is not walked.
 *
 * Limits (compile-time):
 *   - 4 concurrent open file handles (`k_ra8_fs_max_files`).
 *   - 2 concurrent mount points (`k_ra8_fs_max_mounts`).
 *   - File size: 64-bit on exFAT (#676) -- a file past 4 GiB is exactly what
 *     exFAT exists for. FAT12/16/32 files stay capped at 4 GiB - 1
 *     (`DIR_FileSize` is 32-bit; the format's own ceiling), enforced with
 *     `k_ra8_err_invalid_size` at the FAT boundary.
 *   - Sector size: 512 / 1024 / 2048 / 4096 bytes, taken from the backend's
 *     reported block size and cross-checked against the BPB / VBR (#683).
 *   - Media size: 64-bit LBAs end to end -- backend interface, partition
 *     base, GPT entries -- so volumes past 2 TiB are addressable (#683).
 *
 * The 4Kn-sector and beyond-2-TiB paths are SIMULATION-VERIFIED ONLY (host
 * tests over fake backends); no such medium has been on the bench. 512-byte
 * sub-2-TiB media remain the hardware-proven configuration.
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
#include "ra8_fs_types.h"

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
 * round-trips through `ra8_fs_mount()` plus the whole exFAT file API:
 * `ra8_fs_open()` in every mode, `ra8_fs_read()`, `ra8_fs_write()`,
 * `ra8_fs_write_file()`, `ra8_fs_rename()`, and `ra8_fs_unlink()`.
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
 *                                    block size is not a power of two in
 *                                    512..4096, or `sectors_per_cluster` is
 *                                    non-zero and not a power of two in 1..128.
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
 * @brief Mount a FAT volume from a block-device backend, auto-selecting the
 *        first partition.
 *
 * @details
 * Reads sector 0 and, if it is not itself a boot sector, follows the partition
 * table to the first volume: MBR partition 0, or -- on a GPT disk (protective
 * MBR type `0xEE`) -- the first Microsoft Basic Data entry. It then validates
 * the BPB signature and bytes-per-sector, computes the cluster count,
 * dispatches to FAT12 / FAT16 / FAT32, and caches the layout in `*out_handle`.
 * Exactly equivalent to `ra8_fs_mount_partition()` with
 * ::k_ra8_fs_partition_auto; use that sibling to reach any other partition.
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
 * @see ra8_fs_mount_partition()  Mount a partition chosen by index.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_mount(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_handle);

/**
 * @brief Mount a FAT/exFAT volume from a specific partition of a block device.
 *
 * @details
 * Generalises ::ra8_fs_mount() with an explicit partition selector so a
 * multi-partition card or disk exposes every volume, not just the first.
 * Sector 0 decides the table type: a GPT disk (protective MBR type `0xEE`)
 * selects entry `index` of the GPT partition-entry array; any other 0xAA55 MBR
 * selects primary entry `index` (0-3). The chosen partition's first LBA becomes
 * `out_handle->partition_base_lba` and every subsequent access is
 * partition-relative. Passing ::k_ra8_fs_partition_auto reproduces
 * ::ra8_fs_mount() exactly, including transparent superfloppy (LBA 0) mounts;
 * an explicit `index` requires a partition table, so a superfloppy with no
 * table returns `k_ra8_err_not_found`.
 *
 * @param[in]  backend    Block-device implementation. Must remain alive for
 *                        the lifetime of the mount.
 * @param[in]  index      Zero-based partition index, or ::k_ra8_fs_partition_auto.
 * @param[out] out_handle Populated mount handle on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Volume mounted successfully.
 * @retval k_ra8_err_null_ptr          backend or out_handle is NULL.
 * @retval k_ra8_err_invalid_arg       Backend has NULL function pointers.
 * @retval k_ra8_err_no_mem            No free mount slot.
 * @retval k_ra8_err_out_of_range      `index` is past the partition table.
 * @retval k_ra8_err_not_found         The selected entry is empty, or there is
 *                                     no partition table to index.
 * @retval k_ra8_err_not_supported     GPT entry-array geometry this backend
 *                                     cannot address (non-128-byte entries, or
 *                                     a first LBA above 32 bits).
 * @retval k_ra8_err_validation_failed BPB signature invalid / unsupported.
 *
 * @pre `backend->read_block` is non-NULL.
 * @pre `out_handle` is non-NULL.
 * @post On success, `out_handle->in_use == 1` and `type != unknown`.
 * @post On failure, `*out_handle` is left untouched.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 * @see ra8_fs_mount()  Auto-select the first partition.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_mount_partition(const ra8_fs_backend_t* backend, uint8_t index, ra8_fs_mount_t** out_handle);

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
 * @brief Open or create a file by path, 8.3 or long.
 *
 * @details A path that resolves to a DIRECTORY is rejected in every mode. In
 * write mode that rejection is load-bearing: the truncate step would free the
 * cluster chain holding the directory's contents and leave every file inside it
 * unreachable. In read mode it prevents handing back the zero-byte handle a
 * directory's `DIR_FileSize` of 0 would otherwise describe.
 *
 * All three modes work on every filesystem this adapter mounts, exFAT included
 * (#602). An exFAT write grows the file one cluster at a time out of the
 * allocation bitmap, so the size a caller can create is bounded by the volume's
 * free space rather than by RAM, and a fragmented volume is written the way the
 * format intends: the entry set keeps `NoFatChain` while the run stays
 * contiguous and drops it -- materialising a real FAT chain over the clusters
 * already allocated -- the first time the next cluster is not the successor of
 * the last.
 *
 * @param[in]  handle    Mount handle.
 * @param[in]  path      Path from the volume root, e.g. `"HELLO.TXT"` or
 *                       `"/Reading List/Chapter One.txt"`. Any component may
 *                       be a long name of up to 247 characters.
 * @param[in]  mode      `k_ra8_fs_mode_read`, `_write`, or `_append`.
 * @param[out] out_file  Populated file handle on success.
 *
 * @retval k_ra8_ok                     File opened.
 * @retval k_ra8_err_null_ptr           Any pointer arg is NULL.
 * @retval k_ra8_err_invalid_arg        `path` names a directory (any mode), or
 *                                     a new name is empty, holds a character no
 *                                     name may carry, or is longer than the
 *                                     volume allows -- 247 characters on FAT,
 *                                     64 on exFAT.
 * @retval k_ra8_err_not_found          Read mode and path doesn't exist.
 * @retval k_ra8_err_access_denied      A writing mode (`_write` / `_append`) on
 *                                     a file whose read-only attribute is set;
 *                                     a read open of the same file succeeds.
 * @retval k_ra8_err_no_mem             No free file slot, no directory slot, or
 *                                     the volume has no free cluster.
 * @retval k_ra8_err_no_data            Write mode failed to allocate cluster.
 * @retval k_ra8_err_not_supported      An exFAT entry set larger than this
 *                                     adapter rewrites (a name over 64 chars
 *                                     written by another implementation).
 *
 * @pre handle->in_use == 1.
 * @post On success, out_file->in_use == 1 and offset is at file start (read/write)
 *       or end (append).
 * @post On `k_ra8_err_invalid_arg` the volume is unchanged.
 * @note Write mode TRUNCATES in place on both filesystems: the name keeps its
 *       directory entry (and its creation stamp) and only the contents go.
 * @note The read-only attribute is honored HERE, at open time (like POSIX/DOS):
 *       a handle already opened for writing keeps writing even if the file is
 *       marked read-only afterwards via ::ra8_fs_set_attr(). A content write
 *       also sets the archive attribute.
 * @see ra8_fs_rmdir()   The verb for removing a directory.
 * @see ra8_fs_set_attr() Sets / clears the read-only attribute this honors.
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
 * @retval k_ra8_err_invalid_size  FAT volume and the write would push the file
 *                                past 4 GiB - 1 (`DIR_FileSize` is 32-bit);
 *                                exFAT files have no such cap (#676).
 * @retval k_ra8_err_no_mem        Volume out of free clusters.
 * @post On success the entry's `DIR_WrtTime` / `DIR_WrtDate` name this write.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_write(ra8_fs_file_t* file, const uint8_t* buf, uint32_t len);

/**
 * @brief Create a whole file in one call (provisioning helper).
 *
 * @details Convenience wrapper, and nothing more: on EVERY filesystem it opens
 * @p path in write mode, writes @p data, and closes. It carried a second,
 * exFAT-only implementation until exFAT learned to stream (#602) -- a
 * whole-file creator that needed one contiguous run and the entire payload in
 * RAM at once. Both limits are gone with it, and the one remaining path means
 * the two filesystems can no longer disagree about what this call does.
 *
 * An existing @p path is REPLACED on both filesystems: write mode truncates it,
 * so its old contents are discarded and its clusters returned to the volume's
 * free space. Calling this twice with the same name leaves exactly one file, of
 * the second call's contents, with no space lost to the first.
 *
 * @param[in] handle Mounted volume.
 * @param[in] path   File path, UTF-8; nested paths resolve on both
 *                   filesystems, provided every intermediate directory exists.
 * @param[in] data   File contents.
 * @param[in] len    Byte count; 0 leaves an empty file.
 *
 * @retval k_ra8_ok              File created and written.
 * @retval k_ra8_err_null_ptr    Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg Empty/oversized name, or @p path names an
 *                               existing directory.
 * @retval k_ra8_err_access_denied @p path exists and its read-only attribute is
 *                               set (it would be replaced -- refused).
 * @retval k_ra8_err_no_mem      Out of free clusters or directory slots.
 * @retval k_ra8_err_*           Backend error.
 *
 * @pre No handle is open on @p path.
 * @post On success @p path resolves to exactly one entry of @p len bytes.
 * @post On success a replaced predecessor's clusters read as free.
 * @note Not atomic: the truncate lands before the new content does, so a
 *       failure mid-write leaves @p path short, not stale.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_write_file(ra8_fs_mount_t* handle, const char* path, const uint8_t* data, uint32_t len);

/**
 * @brief Move the file offset to `offset_bytes` (clamped to size).
 * @details 64-bit so any position in a >4 GiB exFAT file is reachable (#676).
 * @retval k_ra8_ok            Seek committed.
 * @retval k_ra8_err_null_ptr  file is NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_seek(ra8_fs_file_t* file, uint64_t offset_bytes);

/** @brief Report the current offset (64-bit; see ::ra8_fs_seek). @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_fs_tell(const ra8_fs_file_t* file, uint64_t* out_offset);

/** @brief Report the file's size in bytes (64-bit on exFAT, #676). @since 0.1.0 */
[[nodiscard]] ra8_err_t ra8_fs_size(const ra8_fs_file_t* file, uint64_t* out_bytes);

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
 * @param[in]  path   NUL-terminated path. Nested paths resolve on every
 *                    supported filesystem, FAT12/16/32 and exFAT alike.
 * @param[out] out    Receives the entry's metadata on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Entry found; @p out populated.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_not_found     Nothing at @p path (or an intermediate
 *                                 component is missing).
 * @retval k_ra8_err_invalid_arg   A path component is longer than 247
 *                                 characters.
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
 * @details Every supported filesystem enumerates any directory by path (`"/"`
 * for the root, `"/books"` for a subdirectory) -- exFAT included since #605.
 * FAT's synthetic "." and ".." entries are not reported; exFAT has none to
 * report. A subdirectory appears in its parent's listing like any other entry,
 * with the directory bit set in the `attr` argument.
 *
 * @param[in] handle Mount handle.
 * @param[in] path   Directory path (`"/"` or a nested path).
 * @param[in] cb     Callback (must be non-NULL).
 * @param[in] ctx    Cookie forwarded to the callback.
 *
 * @retval k_ra8_ok                 Enumeration complete.
 * @retval k_ra8_err_null_ptr       handle/cb NULL.
 * @retval k_ra8_err_not_found      A path component does not exist.
 * @retval k_ra8_err_invalid_arg    A path component names a file.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_listdir(ra8_fs_mount_t* handle, const char* path, ra8_fs_listdir_cb_t cb, void* ctx);

/**
 * @brief Delete a file: mark its dir entry deleted and free its clusters.
 *
 * @details FAT12/16/32: 0xE5-marks the entry -- together with the VFAT
 * long-name chain in front of it, if it has one -- and frees the FAT chain.
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
 * @retval k_ra8_err_invalid_arg  `path` names a directory.
 * @retval k_ra8_err_access_denied `path`'s read-only attribute is set.
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
 * @brief Rename a file within its own directory.
 *
 * @details FAT12/16/32: re-files the entry under the new name -- writing the
 * new entry (with its long-name chain, if the new name needs one) before
 * taking the old one away, so a failure between the two leaves the file
 * reachable under both names rather than under neither. The cluster, the size
 * and the attributes are carried across unchanged. Legacy note: this used to
 * rewrite the 11-byte packed 8.3 name in the
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
 * @param[in] old_path Existing path.
 * @param[in] new_path Replacement path, in the same directory (must not
 *                     already exist).
 *
 * @retval k_ra8_ok                File renamed.
 * @retval k_ra8_err_null_ptr      Any pointer arg is NULL.
 * @retval k_ra8_err_not_found     @p old_path does not exist.
 * @retval k_ra8_err_access_denied @p old_path's read-only attribute is set.
 * @retval k_ra8_err_exists        @p new_path already resolves.
 * @retval k_ra8_err_not_supported exFAT name longer than 15 characters, or the
 *                                two paths are in different directories.
 * @retval k_ra8_err_invalid_arg   The new leaf holds a character no FAT name
 *                                may carry, or is longer than 247 characters.
 * @retval k_ra8_err_no_mem        The directory has no room for the new entry.
 * @pre The file is not open.
 * @post On success @p new_path resolves to the same data.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fs_rename(ra8_fs_mount_t* handle, const char* old_path, const char* new_path);

/**
 * @brief Create a directory at @p path.
 *
 * @details Resolves all-but-the-last path component to an existing parent
 * directory, then creates the final component as a new, empty subdirectory.
 * Nested paths are supported (`"/books/scifi"`), provided each intermediate
 * component already exists. A partial allocation is rolled back on failure, so
 * the volume is never leaked.
 *
 * On FAT the new directory is stamped with its "." and ".." links, and a leaf
 * that is not 8.3-representable is stored as a long name, exactly as
 * `ra8_fs_open()` stores one. On exFAT the new directory is one zeroed cluster
 * -- entry type 0x00 is end-of-directory, so that IS an empty directory -- and
 * there are no dot entries at all, because exFAT has none.
 *
 * An existing name is REFUSED with ::k_ra8_err_exists, whether it is a file or
 * a directory. `mkdir` is not a create-or-replace verb; `ra8_fs_write_file()`
 * is the one that replaces.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to create.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_null_ptr      `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_invalid_arg   The leaf is empty, longer than 247
 *                                 characters, or holds a character no FAT
 *                                 name may carry.
 * @retval k_ra8_err_exists        The name already exists in the parent.
 * @retval k_ra8_err_not_found     An intermediate component does not exist.
 * @retval k_ra8_err_no_mem        Parent directory or volume is full.
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
 * @brief Remove an empty directory at @p path.
 *
 * @details The symmetric partner of `ra8_fs_mkdir()`: resolves all-but-the-last
 * path component to an existing parent, requires the final component to be an
 * existing directory, requires that directory to be empty, then frees its
 * clusters and takes its entry away. Nested paths are supported
 * (`"/books/scifi"`).
 *
 * The emptiness proof runs before anything is freed, so a refusal costs the
 * volume nothing. Only remnants are discounted -- FAT's deleted (0xE5) slots
 * and orphaned long-name entries, exFAT's entries with the in-use bit clear --
 * so a directory another implementation left remnants in is still removable,
 * while anything live makes it ::k_ra8_err_not_empty. On FAT the directory's
 * own "." and ".." links do not count either; exFAT has none to discount.
 *
 * The volume root is not removable, and neither is a file: use `ra8_fs_unlink()`
 * for those.
 *
 * @param[in,out] handle Mount handle.
 * @param[in]     path   NUL-terminated directory path to remove.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Directory removed.
 * @retval k_ra8_err_null_ptr       `handle` or `path` was NULL.
 * @retval k_ra8_err_invalid_state  Mount is not in use.
 * @retval k_ra8_err_invalid_arg    `path` is the root, or names a file rather
 *                                  than a directory.
 * @retval k_ra8_err_not_found      No such entry, or a component is missing.
 * @retval k_ra8_err_not_empty      The directory still holds entries.
 * @retval k_ra8_err_protocol_error Corrupt chain, or a directory entry that
 *                                  claims no data cluster.
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
