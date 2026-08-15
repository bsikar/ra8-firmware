/**
 * @file ra8_fs_fat_protos_a_internal.h
 * @brief Cross-TU helper prototypes for the FAT/exFAT adapter (part A of 2).
 * @ingroup grp_storage
 *
 * @details
 * The first half of the FAT/exFAT adapter's cross-TU helper prototypes. Each
 * helper is defined `static`-free in exactly one FAT/exFAT translation unit and
 * called from at least one other. This part covers the alphabetical run from
 * `priv_83_to_str()` through `priv_exfat_upcase_verify()`; the remaining helpers live
 * in `ra8_fs_fat_protos_b_internal.h`. Both are aggregated by the
 * `ra8_fs_fat_internal.h` umbrella, which every `ra8_fs_fat*.c` file includes.
 *
 * This header aggregates each cross-TU helper's full Doxygen contract.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_fs_fat_types_internal.h"

/* ===========================================================================
 * Cross-TU helper prototypes. Each is defined `static`-free in exactly one
 * FAT/exFAT translation unit and called from at least one other.
 * ===========================================================================
 */

/**
 * @brief Unpack on-disk 11-byte 8.3 name into NUL-terminated "NAME.EXT".
 *
 * @details Trims trailing space pad in the base portion, restores the
 *          0x05 -> 0xE5 kanji escape, and emits the dot + extension
 *          only when the extension is non-empty. The two `DIR_NTRes` case
 *          flags are applied as they are unpacked, so an entry written for
 *          `data.log` -- stored upper-case with both flags set -- reads back
 *          as `data.log` rather than `DATA.LOG`. Pass 0 for @p ntres to get
 *          the raw upper-case form.
 *
 * @param[in]  in11  Packed 11-byte name.
 * @param[in]  ntres The entry's `DIR_NTRes` byte (0 when it has none).
 * @param[out] out13 Buffer of at least 13 bytes (8 + . + 3 + NUL).
 *
 * @pre `in11` and `out13` are non-NULL.
 * @pre `out13` has at least 13 writable bytes.
 * @post `out13` is NUL-terminated.
 * @post Trailing space padding has been stripped.
 *
 * @note Helper used only by `ra8_fs_listdir`.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_83_to_str(const uint8_t* in11, uint8_t ntres, char* out13);

/**
 * @brief Free-cluster scan from the next-free hint, wrapping exactly once.
 *
 * @details Starts at the mount's next-free hint (::priv_alloc_hint_get, seeded
 *          from FAT32's `FSI_Nxt_Free` when that validates) and walks forward
 *          for at most `count_of_clusters` steps, wrapping back to cluster 2
 *          once, so the whole volume is still examined before it is declared
 *          full. The FAT entries are read through the one-sector cache, which
 *          is what makes a scan cost one block read per 128 clusters on FAT32
 *          instead of one per cluster.
 *
 *          On success the hint moves to the cluster after the one taken and
 *          the tracked free count drops by one, so the FSInfo writeback has
 *          something true to write.
 *
 * @param[in]  m           Mount providing geometry and backend.
 * @param[out] out_cluster On success, the allocated cluster number.
 *
 * @return Error code.
 * @retval k_ra8_ok          Cluster found; `*out_cluster` set.
 * @retval k_ra8_err_no_mem  Volume is full -- no free clusters.
 * @retval k_ra8_err_*       Backend read failure.
 *
 * @pre `m` and `out_cluster` are non-NULL.
 * @pre Volume is mounted and geometry is valid.
 * @post On success, `*out_cluster` is in range and free.
 * @post On failure, `*out_cluster` is unspecified.
 *
 * @note Caller must mark the cluster as EOC after carving it.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_alloc_cluster(const ra8_fs_mount_t* m, uint32_t* out_cluster);

/**
 * @brief Allocate a fresh cluster, mark it EOC, and return its number.
 *
 * @details Combines `priv_alloc_cluster` with a `priv_fat_set` to the
 *          canonical EOC value. Used by the write path when the file
 *          chain needs to grow.
 *
 * @param[in]  m     Mount providing FAT access.
 * @param[out] out_c Receives the allocated cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok    Cluster allocated and marked EOC.
 * @retval k_ra8_err_* Backend or FAT error.
 *
 * @pre `m` and `out_c` are non-NULL.
 * @pre Volume has free clusters.
 * @post On success, `*out_c` is in range and FAT entry = EOC.
 * @post On failure, FAT may have been partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_alloc_eoc_cluster(const ra8_fs_mount_t* m, uint32_t* out_c);

/**
 * @brief Allocate a free entry from the file table; returns NULL if full.
 *
 * @details Linear scan of `s_files` for an entry with `in_use == 0`.
 *
 * @return Pointer to a free file slot, or NULL if all are busy.
 * @retval non-NULL Pointer to a `ra8_fs_file_t` with `in_use == 0`.
 * @retval NULL     File table is full.
 *
 * @pre Module is initialized.
 * @pre Caller serialises open/close operations.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_fs_file_t* priv_alloc_file_slot(void);

/**
 * @brief Uppercase an ASCII character (others returned unchanged).
 *
 * @details Maps a-z to A-Z; any other byte is returned unchanged.
 *
 * @param[in] c Input character.
 * @return Uppercased character.
 * @retval c The (possibly) uppercased value.
 * @pre None.
 * @pre @p c is a byte value.
 * @post No state modified.
 * @post Result depends only on @p c.
 * @note Avoids compound conditions (MC/DC).
 * @since 0.1.0
 */
RA8_PRIV
char priv_ascii_upper(char c);

/**
 * @brief Length-checked byte copy used in place of memcpy().
 *
 * @details
 * Replaces memcpy() so clang-tidy's `clang-analyzer-security.insecureAPI`
 * checker stays happy. Same effect on -O2 generated code.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  src Source buffer.
 * @param[in]  n   Number of bytes to copy.
 *
 * @pre `dst` and `src` are non-NULL and point to at least `n` bytes.
 * @pre `dst` and `src` do not overlap.
 * @post First `n` bytes of `dst` equal first `n` bytes of `src`.
 * @post No state outside `dst` is modified.
 *
 * @note Bounded loop, NASA Rule 2 compliant.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n);

/**
 * @brief Compare two byte buffers for equality (length n).
 *
 * @details Returns early on first mismatch. Used in place of memcmp().
 *
 * @param[in] a First buffer.
 * @param[in] b Second buffer.
 * @param[in] n Number of bytes to compare.
 *
 * @return 1 on equal, 0 on mismatch.
 * @retval 1  All `n` bytes equal.
 * @retval 0  At least one byte differs.
 *
 * @pre `a` and `b` are non-NULL and point to at least `n` bytes.
 * @pre Caller has bounds-checked both buffers.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_byte_equal(const uint8_t* a, const uint8_t* b, uint32_t n);

/**
 * @brief Close an open file -- the guarded body of ::ra8_fs_close().
 *
 * @details Carries the whole contract documented for ::ra8_fs_close() in
 *          `ra8_fs.h`; the public symbol is the wrapper that brackets this call
 *          with ::priv_lock_acquire / ::priv_lock_release. Exposed across
 *          translation units because ::ra8_fs_write_file()'s guarded body has to
 *          reach it without taking the lock a second time.
 *
 * @param[in,out] file Handle returned by ::priv_open_locked / ::ra8_fs_open().
 *
 * @return Error code.
 * @retval k_ra8_ok           File closed.
 * @retval k_ra8_err_null_ptr @p file was NULL.
 * @retval k_ra8_err_*        The final modification-time stamp or the FSInfo
 *                            writeback failed; the handle is released anyway.
 *
 * @pre The library lock is held (or none is installed).
 * @pre All pending writes have already been issued.
 * @post The file slot is marked free for reuse, whatever the metadata write did.
 * @post `file->mount` is reset to NULL.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @par MC/DC:
 * The metadata write is guarded by a four-condition conjunction: the handle is
 * dirty, the handle is in use, it has a mount, and that mount is in use. Its
 * five vectors are driven by `test_close_stamps_final_mtime` and
 * `test_close_guards_an_unusable_handle` in `tests/test_ra8_fs_timestamps.c`,
 * which cite it as
 * `libs/ra8_fs/src/ra8_fs_fat_file.c@priv_close_locked`.
 *
 * @since 0.1.0
 */
RA8_PRIV
RA8_EXPECTS_LOCK("ra8_fs_lock")
ra8_err_t priv_close_locked(ra8_fs_file_t* file);

/**
 * @brief Convert a cluster number into its first data-region LBA.
 *
 * @details Cluster numbering starts at `k_cluster_first_data` (= 2).
 *
 * @param[in] m       Mount providing geometry.
 * @param[in] cluster Cluster number (>= `k_cluster_first_data`).
 *
 * @return Sector LBA of the cluster's first sector.
 * @retval 0..UINT32_MAX  Computed LBA.
 *
 * @pre `m` is non-NULL with valid geometry.
 * @pre `cluster >= k_cluster_first_data`.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint64_t priv_cluster_to_lba(const ra8_fs_mount_t* m, uint32_t cluster);

/**
 * @brief Find a directory entry by 8.3 name within a given directory.
 *
 * @details Walks the directory @p loc (root or a subdirectory) and matches on
 *          the packed 11-byte name field. Skips LFN entries (attr 0x0F) and
 *          deleted slots.
 *
 * @param[in]  m             Mount providing geometry and backend.
 * @param[in]  loc           Directory to search (root or a subdirectory).
 * @param[in]  name83        Packed 11-byte name.
 * @param[out] out_lba       Sector containing the entry.
 * @param[out] out_entry_off Byte offset within the sector.
 * @param[out] out_entry     32 bytes of the entry payload.
 *
 * @return Error code.
 * @retval k_ra8_ok            Entry found; out parameters populated.
 * @retval k_ra8_err_not_found End-of-directory reached without a match.
 * @retval k_ra8_err_*         Backend error.
 *
 * @pre All output pointers are non-NULL.
 * @pre `name83` is non-NULL and points to 11 bytes.
 * @post On success, out parameters identify the on-disk entry.
 * @post On failure, out parameters are unspecified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_find(const ra8_fs_mount_t* m,
                        const dir_loc_t*      loc,
                        const uint8_t*        name83,
                        uint64_t*             out_lba,
                        uint32_t*             out_entry_off,
                        uint8_t               out_entry[k_ra8_fs_dir_entry_bytes]);

/**
 * @brief Find a directory entry by its VFAT long name (case-insensitive).
 *
 * @details Walks the directory described by @p loc sector by sector, calling
 *          priv_dir_find_long_sector() on each one. LFN chains are carried
 *          across sector boundaries via an lfn_state_t accumulator. A leading
 *          '/' in @p want is stripped before matching. Returns on the first
 *          name that matches @p want via priv_name_ieq(), or reports not-found
 *          when the end-of-directory marker is reached without a hit. Used as
 *          the fallback by ra8_fs_open() when the 8.3 short-name lookup misses,
 *          so that files with names longer than 8.3 (e.g. ".epub" four-char
 *          extensions) are accessible by their real long name.
 *
 * @param[in]  m             Mount providing geometry and backend.
 * @param[in]  loc           Directory to search (root or a subdirectory).
 * @param[in]  want          Requested name (a leading '/' is ignored).
 * @param[out] out_lba       Sector containing the matched 8.3 entry.
 * @param[out] out_entry_off Byte offset within the sector.
 * @param[out] out_entry     32 bytes of the matched 8.3 directory entry.
 *
 * @return Error code.
 * @retval k_ra8_ok            Long name matched; out parameters populated.
 * @retval k_ra8_err_not_found No entry's long name equals @p want.
 * @retval k_ra8_err_*         Backend read error propagated from priv_read_sector().
 *
 * @pre All pointer parameters are non-NULL.
 * @pre @p want is a NUL-terminated ASCII string.
 * @post On k_ra8_ok, out parameters identify the on-disk 8.3 entry for @p want.
 * @post On failure, the out parameters are left in an unspecified state.
 *
 * @note Not thread-safe; the caller serialises directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_find_long(const ra8_fs_mount_t* m,
                             const dir_loc_t*      loc,
                             const char*           want,
                             uint64_t*             out_lba,
                             uint32_t*             out_entry_off,
                             uint8_t               out_entry[k_ra8_fs_dir_entry_bytes]);

/**
 * @brief Initialise a directory walker for an arbitrary directory location.
 *
 * @details Dispatches to `priv_dir_walk_init_root` for the root, or sets up a
 *          plain cluster-chain walk starting at `loc->cluster` for a
 *          subdirectory (the same machinery the FAT32 root uses, so
 *          `priv_dir_walk_next_sector` follows the chain unchanged).
 *
 * @param[in]  m   Mount providing geometry and FAT type.
 * @param[in]  loc Directory to walk (root or a subdirectory cluster).
 * @param[out] w   Walker cursor to initialise.
 *
 * @pre `m`, `loc`, and `w` are non-NULL.
 * @pre For a subdirectory, `loc->cluster >= k_cluster_first_data`.
 * @post `w` points at the first sector of the chosen directory.
 * @post `w->entry_idx` is zero.
 *
 * @note Pure init -- does not touch the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_dir_walk_init_loc(const ra8_fs_mount_t* m, const dir_loc_t* loc, dir_walk_t* w);

/**
 * @brief Advance the walker to the next sector.
 *
 * @details For fixed-region roots simply increments the LBA. For
 *          cluster-chain roots advances within the cluster, then
 *          follows the FAT chain when the cluster is exhausted.
 *
 * @param[in]     m       Mount providing geometry and backend.
 * @param[in,out] w       Walker cursor to advance.
 * @param[out]    out_eod Set to 1 if end-of-directory reached, else 0.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Walker advanced (or EOD signalled in `*out_eod`).
 * @retval k_ra8_err_protocol_error Cluster-chain cycle detected (corrupt FAT).
 * @retval k_ra8_err_*              Backend error from a FAT read.
 *
 * @pre `m`, `w`, and `out_eod` are non-NULL.
 * @pre Walker has been initialized by `priv_dir_walk_init_root`.
 * @post On success `w` either points at a new sector or `*out_eod` is 1.
 * @post `w->entry_idx` is reset to 0 on a successful advance.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_walk_next_sector(const ra8_fs_mount_t* m, dir_walk_t* w, uint8_t* out_eod);

/**
 * @brief Read the first cluster from a 32-byte directory entry.
 *
 * @details Combines the high and low cluster halves into a single
 *          32-bit value (FAT32 layout; high half is 0 on FAT12/16).
 *
 * @param[in] entry 32-byte directory entry.
 *
 * @return First cluster of the file.
 * @retval 0..UINT32_MAX  Cluster number.
 *
 * @pre `entry` is non-NULL and points to 32 readable bytes.
 * @pre Caller has already filtered LFN / deleted entries.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_entry_first_cluster(const uint8_t* entry);

/**
 * @brief Patch first-cluster + size back into a 32-byte directory entry.
 *
 * @details Inverse of `priv_entry_first_cluster`; also writes file size.
 *
 * @param[in,out] entry   32-byte directory entry to update.
 * @param[in]     cluster New first cluster.
 * @param[in]     size    New file size in bytes.
 *
 * @pre `entry` is non-NULL and points to 32 writable bytes.
 * @pre Caller has staged the entry in a sector buffer that will be
 *      written back to disk.
 * @post `entry` reflects the new first-cluster and size fields.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_entry_set_cluster_size(uint8_t* entry, uint32_t cluster, uint32_t size);
/**
 * @brief End-of-chain value to write for this FAT type.
 *
 * @details Returns the canonical EOC value (`0xFFF`, `0xFFFF`, or
 *          `0x0FFFFFFF`).
 *
 * @param[in] m Mount providing the FAT type.
 *
 * @return Canonical EOC value for this volume.
 * @retval k_cluster_eoc_write_fat12   FAT12 EOC.
 * @retval k_cluster_eoc_write_fat16   FAT16 EOC.
 * @retval k_cluster_eoc_write_fat32   FAT32 EOC.
 *
 * @pre `m` is non-NULL.
 * @pre `m->type` has been computed by `priv_compute_geometry`.
 * @post No state modified.
 * @post Result is purely a function of `m->type`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_eoc_write(const ra8_fs_mount_t* m);

/**
 * @brief Flush the cached bitmap sector and load @p lba if it changed.
 *
 * @details Writes the dirty cached sector before reading the newly requested one.
 *
 * @param[in]     m      Mounted exFAT volume.
 * @param[in]     lba    Bitmap sector wanted next.
 * @param[in,out] loaded Currently-cached LBA (UINT32_MAX if none).
 * @param[in,out] sec    Cached sector buffer.
 * @return Error code.
 * @retval k_ra8_ok    @p sec now holds @p lba.
 * @retval k_ra8_err_* Backend read/write failure.
 * @pre All pointers are non-NULL.
 * @pre @p sec matches @p loaded on entry.
 * @post @p sec holds @p lba; the previous sector was written if dirty.
 * @post @p loaded == @p lba.
 * @note Keeps the caller's loop nesting shallow.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_bmp_switch(const ra8_fs_mount_t* m, uint64_t lba, uint64_t* loaded, uint8_t* sec);

/**
 * @brief exFAT 32-bit rotate-right-add checksum (boot region + up-case table).
 *
 * @details Folds @p len bytes of @p buf into the running checksum @p cs with
 * `cs = ror1(cs) + byte` per the Microsoft exFAT spec (sections 3.4 and 8.2.2).
 * Shared by the boot-region checksum (which folds surrounding byte ranges to
 * skip the three volatile bytes) and the up-case-table checksum.
 *
 * @param[in] cs  Running checksum (0 to start).
 * @param[in] buf Bytes to fold in.
 * @param[in] len Number of bytes to fold.
 * @return Updated checksum.
 * @retval 0..UINT32_MAX The rotate-add fold of @p cs over @p buf[0..len-1].
 * @pre @p buf holds at least @p len bytes.
 * @pre @p len is the exact byte count of the span to fold.
 * @post No state is modified; the function is pure.
 * @post Return value depends only on @p cs, @p buf, and @p len.
 * @note Pure function; trivially thread-safe.
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_exfat_csum32(uint32_t cs, const uint8_t* buf, uint32_t len);

/**
 * @brief Find a name in ONE exFAT directory.
 *
 * @details Streams @p dir's entries, matching each File entry set against
 * @p path; stops at end-of-directory, the end of the directory's run, or the
 * scan bound. This is a single-directory lookup, not a path resolver -- pass a
 * leaf name and the directory it should be in, or use ::priv_exfat_lookup,
 * which resolves a whole path and then calls this (#605).
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  dir       Directory to search.
 * @param[in]  path      Target leaf name, UTF-8; leading slashes are skipped.
 * @param[out] out_strm  Receives the matched 32-byte Stream-extension entry --
 *                       first cluster, both lengths and the secondary flags,
 *                       so a caller decodes the fields it needs rather than
 *                       every caller paying for another out-parameter.
 * @param[out] out_attr  Low byte of the File entry's FileAttributes, which is
 *                       where ::k_exfat_attr_directory lives. Callers that act
 *                       on the entry MUST consult it: a directory answers this
 *                       lookup exactly like a file, and treating one as a file
 *                       frees its cluster chain (#604).
 * @return Error code.
 * @retval k_ra8_ok            Entry found; outputs populated.
 * @retval k_ra8_err_not_found No matching entry in @p dir.
 * @retval k_ra8_err_*         Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre @p dir locates an existing directory on this volume.
 * @post On success @p out_strm mirrors the on-disk Stream entry.
 * @post Scan is bounded by ::k_exfat_scan_limit entries.
 * @note Not thread-safe; callers serialise.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_find(const ra8_fs_mount_t* m,
                          const exfat_dir_t*    dir,
                          const char*           path,
                          uint8_t*              out_strm,
                          uint8_t*              out_attr);

/**
 * @brief Locate the allocation-bitmap entry in the exFAT root directory.
 *
 * @details Streams the root directory for the 0x81 entry and returns its data run.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[out] out_clus  First cluster of the allocation bitmap.
 * @param[out] out_len   Bitmap length in bytes.
 * @return Error code.
 * @retval k_ra8_ok            Bitmap located.
 * @retval k_ra8_err_not_found No allocation-bitmap entry.
 * @retval k_ra8_err_*         Backend read failure.
 * @pre All pointers are non-NULL; ``m->type`` is exFAT.
 * @pre ``m->root_cluster`` is valid.
 * @post On success the bitmap location is returned.
 * @post No volume state modified.
 * @note Reads only the directory chain.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_find_bitmap(const ra8_fs_mount_t* m, uint32_t* out_clus, uint32_t* out_len);

/**
 * @brief Format the backend as a PC-standard partitioned exFAT volume (#102).
 *
 * @details Writes a DOS/MBR partition table at LBA 0 with a single type-0x07
 *          (exFAT/NTFS) partition aligned at ::k_exfat_fmt_part_lba, then lays a
 *          complete exFAT volume INSIDE that partition: main + backup boot
 *          regions with boot checksums (VolumeLength = partition length,
 *          PartitionOffset = @p k_exfat_fmt_part_lba), the single FAT
 *          (bitmap/up-case/root chains), the allocation bitmap with the system
 *          clusters pre-marked, the canonical Microsoft up-case table + its
 *          checksum, and the root directory entry set. A PC therefore sees a
 *          normal partitioned removable disk and the volume mounts with no
 *          repair; ::ra8_fs_mount follows the MBR back to the partition.
 *
 * @param[in] backend       Block-device backend.
 * @param[in] total_sectors Device capacity in device sectors.
 * @param[in] bps           Device sector size in bytes (a power of two,
 *                          512..4096, validated by the format entry point).
 * @param[in] label         Optional volume label (<= 11 chars), may be NULL.
 *
 * @return Error code.
 * @retval k_ra8_ok                A mountable partitioned exFAT volume was written.
 * @retval k_ra8_err_invalid_size  Partition too small for an exFAT volume.
 * @retval k_ra8_err_not_supported Below the exFAT minimum plus the 1 MiB
 *                                partition alignment, past the MBR's 32-bit
 *                                fields (2 TiB at 512-byte sectors), or system
 *                                cluster chains exceed FAT sector 0.
 * @retval k_ra8_err_*             Backend write failure.
 *
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p total_sectors is the actual device capacity reported by the backend.
 * @post On k_ra8_ok, LBA 0 holds an MBR and the partition holds a complete exFAT volume.
 * @post On failure, partial writes may have been made; the device should be reformatted.
 *
 * @note Not thread-safe; serialize with mounts on the same backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_format(const ra8_fs_backend_t* backend,
                            uint64_t                total_sectors,
                            uint32_t                bps,
                            const char*             label);

/**
 * @brief Enumerate ONE directory of an exFAT volume.
 *
 * @details Walks @p dir's entry stream; every in-use File entry set yields one
 * callback with the ASCII name (truncated to the local buffer, NUL-terminated),
 * the low attribute byte, and the size. Deleted entries and non-file sets
 * (bitmap, up-case table, label) are skipped. Subdirectories are reported like
 * any other entry set, with ::k_exfat_attr_directory set in the attribute byte.
 *
 * @param[in] m   Mounted exFAT volume.
 * @param[in] dir Directory to enumerate (::priv_exfat_dir_root for the root).
 * @param[in] cb  Callback invoked once per visible entry set.
 * @param[in] ctx Cookie forwarded to the callback.
 * @return Error code.
 * @retval k_ra8_ok    Enumeration completed (EOD or end of run reached).
 * @retval k_ra8_err_* Backend read failure.
 * @pre @p m, @p dir and @p cb are non-NULL; mount is exFAT.
 * @pre @p dir locates an existing directory on this volume.
 * @post @p cb ran once per in-use entry set.
 * @post No volume state modified.
 * @note Names longer than the buffer are truncated (still NUL-terminated).
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_listdir(const ra8_fs_mount_t* m,
                             const exfat_dir_t*    dir,
                             ra8_fs_listdir_cb_t   cb,
                             void*                 ctx);
/**
 * @brief Copy the next visible exFAT directory entry from an existing cursor.
 * @details Advances the cursor and copies one stable name, attribute mask, and size.
 * @param[in] m Mounted exFAT volume.
 * @param[in,out] cur Independent caller-owned directory cursor state.
 * @param[out] out Stable copied entry value.
 * @param[out] out_entry True for one copied entry; false at clean end.
 * @return Media, corruption, bound, or clean-end status.
 * @retval k_ra8_ok One entry was copied or clean end was reached.
 * @retval k_ra8_err_* Media, entry-set, name, or cursor-bound failure.
 * @pre Required pointers are non-NULL and @p cur was initialized for @p m.
 * @pre The caller holds the filesystem serialization required by @p m.
 * @post The cursor advances monotonically and no callback is invoked.
 * @post Clean end leaves @p out_entry false without exposing sector scratch.
 * @note The caller owns filesystem serialization around this operation.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_exfat_dir_next(const ra8_fs_mount_t* m,
                                       exfat_cursor_t*       cur,
                                       ra8_fs_dirent_t*      out,
                                       bool*                 out_entry);
/**
 * @brief Compare one file-name entry's 15 UTF-16 units against a needle.
 *
 * @details Case-insensitive under the canonical up-case table
 * (::priv_exfat_upcase_unit), which is the fold the exFAT specification defines
 * name matching against. Positions at/after @p nlen are treated as already
 * matched (tail padding).
 *
 * It used to compare the low byte of each unit with an ASCII fold and reject
 * any unit whose high byte was set, so a name outside ASCII never matched at
 * all (#606).
 *
 * @param[in] entry 32-byte file-name (0xC1) entry.
 * @param[in] name  Target name as UTF-16 code units.
 * @param[in] pos   Index of the first name unit this entry covers.
 * @param[in] nlen  Total name length in UTF-16 units.
 * @return 1 if this slice matches, else 0.
 * @retval 1 Slice matches.
 * @retval 0 At least one folded unit differs.
 * @pre @p entry and @p name are non-NULL.
 * @pre @p name holds at least @p nlen units.
 * @post No state modified.
 * @post Inputs are unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV
uint8_t
priv_exfat_name_chunk_eq(const uint8_t* entry, const uint16_t* name, uint32_t pos, uint32_t nlen);

/**
 * @brief Compute the exFAT NameHash for a name in UTF-16 code units.
 *
 * @details Hashes the up-cased UTF-16LE name (low then high byte per unit),
 * up-casing through ::priv_exfat_upcase_unit -- the volume's own table, which is
 * what the specification defines the hash over.
 *
 * Up-casing with an ASCII-only rule, which is what this did, stored a hash no
 * compliant reader recomputes for any name outside ASCII: the host could see
 * the file listed and then fail to find it, because the hash is the index it
 * probes with (#606).
 *
 * @param[in] name File name as UTF-16 code units.
 * @param[in] nlen Name length in UTF-16 units.
 * @return 16-bit name hash.
 * @retval 0..0xFFFF The hash value.
 * @pre @p name is non-NULL and holds at least @p nlen units.
 * @pre The volume's up-case table is the canonical one, or the name is ASCII.
 * @post No state modified.
 * @post Inputs unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_exfat_name_hash(const uint16_t* name, uint32_t nlen);

/**
 * @brief Convert a caller's exFAT name to code units, refusing what will not fold.
 *
 * @details The one place an exFAT path becomes the units every on-disk
 * structure counts in. Two refusals, both loud: a name that is not well-formed
 * UTF-8, and a name outside ASCII on a volume whose up-case table this build
 * cannot reproduce -- because the `NameHash` it would store is one the host
 * disagrees with (see ::priv_exfat_upcase_verify).
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      Caller's name, NUL-terminated UTF-8, no leading slash.
 * @param[out] out       Receives up to ::k_exfat_name_cap code units.
 * @param[out] out_units Receives the unit count.
 * @return Error code.
 * @retval k_ra8_ok                Converted.
 * @retval k_ra8_err_invalid_arg   @p path is not well-formed UTF-8.
 * @retval k_ra8_err_no_mem        Over ::k_exfat_name_cap units.
 * @retval k_ra8_err_not_supported Non-ASCII on a volume with a foreign table.
 * @pre All pointers are non-NULL; @p out holds ::k_exfat_name_cap units.
 * @pre @p m is a mounted exFAT volume.
 * @post On failure `*out_units` is 0.
 * @post No volume state is modified.
 * @note Pure apart from the outputs.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_name_to_units(const ra8_fs_mount_t* m,
                                   const char*           path,
                                   uint16_t*             out,
                                   uint32_t*             out_units);

/**
 * @brief Convert a leaf name to code units, mapping over-long to @p too_long.
 *
 * @details The exact wrapper the mutate verbs share: ::priv_exfat_name_to_units
 * plus the one line that turns its ::k_ra8_err_no_mem -- a name past the cap --
 * into whatever "absent" or "unsupported" code the caller reports. Factored
 * out so find_set, rename and the create paths do not each restate it (#606).
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  name      Leaf name, NUL-terminated UTF-8, no leading slash.
 * @param[out] out       Receives up to ::k_exfat_name_cap code units.
 * @param[out] out_units Receives the unit count.
 * @param[in]  too_long  The code returned when @p name exceeds the cap.
 * @return Error code.
 * @retval k_ra8_ok                Converted.
 * @retval too_long                @p name is longer than ::k_exfat_name_cap.
 * @retval k_ra8_err_invalid_arg   @p name is not well-formed UTF-8.
 * @retval k_ra8_err_not_supported Non-ASCII on a volume with a foreign table.
 * @pre All pointers are non-NULL; @p out holds ::k_exfat_name_cap units.
 * @pre @p m is a mounted exFAT volume.
 * @post On failure the unit count is 0.
 * @post No volume state is modified.
 * @note Pure apart from the outputs.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_needle_units(const ra8_fs_mount_t* m,
                                  const char*           name,
                                  uint16_t*             out,
                                  uint32_t*             out_units,
                                  ra8_err_t             too_long);

/**
 * @brief Fetch the next 32-byte directory entry, following the cluster chain.
 *
 * @details Advances across sectors and (via the FAT) clusters. Reports
 * end-of-directory as ::k_ra8_err_not_found when the chain reaches EOC.
 *
 * @param[in]     m   Mounted exFAT volume.
 * @param[in,out] cur Cursor; advanced by one entry on success.
 * @param[out]    out Receives the 32-byte entry.
 * @return Error code.
 * @retval k_ra8_ok            ``out`` holds the next entry.
 * @retval k_ra8_err_not_found The directory chain ended (EOC).
 * @retval k_ra8_err_*         Backend or FAT read failure.
 * @pre @p m, @p cur, and @p out are non-NULL.
 * @pre ``cur->cluster`` is a valid directory cluster.
 * @post On success ``cur`` points at the following entry.
 * @post On failure ``out`` is undefined.
 * @note Re-reads the sector per entry (simple; dir scans are short).
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_next_entry(const ra8_fs_mount_t* m, exfat_cursor_t* cur, uint8_t* out);

/**
 * @brief Open a file (read-only) on a mounted exFAT volume.
 *
 * @details Resolves @p path in the root directory and populates a read handle;
 * write/append modes are rejected (exFAT is read-only here). A name that
 * resolves to a directory is rejected rather than opened as the empty file its
 * zero DataLength would otherwise describe (#604).
 *
 * @param[in]  handle   Mounted exFAT volume.
 * @param[in]  path     Flat root-level file name, UTF-8.
 * @param[in]  mode     Open mode; only ::k_ra8_fs_mode_read is supported.
 * @param[out] out_file Receives the open handle.
 * @return Error code.
 * @retval k_ra8_ok                File opened.
 * @retval k_ra8_err_not_supported Write/append requested (exFAT is read-only).
 * @retval k_ra8_err_invalid_arg   @p path names a directory, not a file.
 * @retval k_ra8_err_not_found     No such file.
 * @retval k_ra8_err_no_mem        File table full.
 * @pre @p handle, @p path, @p out_file are non-NULL; mount is exFAT.
 * @pre @p handle is in use.
 * @post On success ``*out_file`` is an in-use read handle.
 * @post On failure no file slot is consumed.
 * @note Not thread-safe; callers serialize.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_open(ra8_fs_mount_t* handle,
                          const char*     path,
                          ra8_fs_mode_t   mode,
                          ra8_fs_file_t** out_file);

/**
 * @brief Rename a root-level file on an exFAT volume, at any storable length.
 *
 * @details Rewrites the whole entry set under the new name: the Stream entry's
 * NameLength + NameHash, a run of one Name entry per fifteen UTF-16 units, the
 * File entry's SecondaryCount, and the recomputed SetChecksum. When the new
 * name keeps the entry count the set is rewritten in place; when it needs more
 * or fewer Name entries the set is relocated to a fresh run of slots (growing
 * the directory if needed) and the old set is then retired, so names up to
 * ::k_exfat_name_cap units -- the same range ra8_fs_write_file() writes -- all
 * rename. The data itself never moves: FirstCluster, DataLength and the
 * timestamps ride across untouched.
 *
 * @param[in] m        Mounted exFAT volume.
 * @param[in] old_path Existing root-level name.
 * @param[in] new_path Replacement name (must not exist).
 * @return Error code.
 * @retval k_ra8_ok                File renamed.
 * @retval k_ra8_err_not_found     @p old_path does not exist.
 * @retval k_ra8_err_exists        @p new_path already resolves.
 * @retval k_ra8_err_invalid_arg   @p new_path is empty, over the name cap, or
 *                                 not well-formed UTF-8.
 * @retval k_ra8_err_not_supported The paths cross directories, or a non-ASCII
 *                                 name on a volume with a foreign up-case table.
 * @retval k_ra8_err_no_mem        The directory cannot grow to hold a longer set.
 * @pre @p m and both paths are non-NULL; mount is exFAT.
 * @pre The file is not open.
 * @post @p new_path resolves to the same data; @p old_path is gone.
 * @post File attributes, size, and clusters are unchanged.
 * @note Root-directory namespace only.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_rename(const ra8_fs_mount_t* m, const char* old_path, const char* new_path);

/**
 * @brief Compute the SetChecksum over a built directory entry set.
 *
 * @details Folds every byte except the File entry's checksum field (bytes 2-3).
 * @param[in] set   Contiguous entry-set bytes (File + Stream + Name entries).
 * @param[in] bytes Total byte count of the set.
 * @return 16-bit SetChecksum.
 * @retval 0..0xFFFF The checksum.
 * @pre @p set is non-NULL and at least @p bytes long.
 * @pre @p bytes is a multiple of the entry size.
 * @post No state modified.
 * @post @p set is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_exfat_set_checksum(const uint8_t* set, uint32_t bytes);

/**
 * @brief Delete a file on an exFAT volume, at any depth.
 *
 * @details Resolves the path's parent, then hands the leaf to
 * ::priv_exfat_unlink_at: the entry set is located, every entry's in-use bit
 * (bit 7 of the entry type) is cleared, and the clusters are freed in the
 * allocation bitmap. A set carrying ::k_exfat_attr_directory is refused:
 * freeing a directory's run would strand every entry inside it (#604).
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] path File path, UTF-8, nested or root-level.
 * @return Error code.
 * @retval k_ra8_ok              File unlinked.
 * @retval k_ra8_err_invalid_arg @p path names a directory or the volume root.
 * @retval k_ra8_err_not_found   No such file, or a component is missing.
 * @retval k_ra8_err_*           Directory or bitmap write failure.
 * @pre @p m and @p path are non-NULL; mount is exFAT.
 * @pre The file is not open.
 * @post The name no longer resolves; its clusters are free.
 * @post Other directory entries are untouched.
 * @note Not thread-safe; callers serialise.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_unlink(const ra8_fs_mount_t* m, const char* path);

/**
 * @brief Fold a UTF-16 code unit through the canonical exFAT up-case table.
 *
 * @details The exFAT specification defines both `NameHash` and name comparison
 * over the UP-CASED name, using the table the volume carries. This build
 * embeds and writes the canonical Microsoft table -- the one `mkfs.exfat` and
 * the Windows formatter emit, checksum 0xE619D30D -- and this is the reader
 * for it, so the hash stored on create is the hash a host recomputes.
 *
 * Look-up walks the compressed table rather than expanding it, because the
 * expansion is 128 KiB. The ASCII range short-circuits to ::priv_ascii_upper,
 * which the table agrees with for every one of those 128 units.
 *
 * A surrogate unit folds to itself: the table covers the BMP, and a
 * supplementary code point has no simple case mapping inside a single unit.
 *
 * @param[in] unit UTF-16 code unit to fold.
 * @return The up-cased unit.
 * @retval unit  No mapping applies (identity run, or past the table).
 * @retval other The table's mapping for @p unit.
 * @pre None; every 16-bit value is a legal input.
 * @pre The caller wants the VOLUME's fold and has checked
 *      ::ra8_fs_mount_t::exfat_upcase_ok, or is folding a FAT long name.
 * @post No state modified.
 * @post The result depends only on @p unit and the embedded table.
 * @note Pure function; trivially thread-safe.
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_exfat_upcase_unit(uint16_t unit);

/**
 * @brief Rotate-add checksum of the up-case table this build embeds.
 *
 * @details The same 32-bit fold ::priv_exfat_write_upcase records in the
 * volume's up-case directory entry, computed over the in-flash table. Having
 * it as a function rather than a constant is the point: a constant could drift
 * from the bytes it claims to describe, and this cannot.
 *
 * @return Checksum of ::k_exfat_fmt_upc_std_bytes table bytes.
 * @retval 0xE619D30D The canonical Microsoft table, which is what is embedded.
 * @pre None.
 * @pre The caller compares it against a volume's `TableChecksum`.
 * @post No state modified.
 * @post The result is the same on every call.
 * @note Pure function; trivially thread-safe.
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_exfat_upcase_checksum(void);

/**
 * @brief Decide whether this build's fold matches the mounted volume's table.
 *
 * @details Walks the root directory for the up-case-table entry (0x82) and
 * compares its `TableChecksum` against ::priv_exfat_upcase_checksum(). The
 * answer lands in ::ra8_fs_mount_t::exfat_upcase_ok and decides one thing:
 * whether a name containing anything outside ASCII may be created, renamed or
 * looked up on this volume at all. An ASCII-only name folds identically under
 * every conforming table, so it is never affected.
 *
 * Refusing the operation is the point. Hashing a name with a fold the volume
 * does not use stores a `NameHash` the host disagrees with, and a host that
 * cannot match the hash cannot find a file it can see listed -- a disagreement
 * with the on-disk format rather than a limitation of this API (#606).
 *
 * A missing entry or a read error is treated exactly like a mismatch, so a
 * volume this function could not interrogate degrades to ASCII-only names
 * rather than failing to mount.
 *
 * @param[in,out] m Mount whose exFAT geometry is already populated.
 * @return Nothing.
 * @pre @p m is non-NULL and its exFAT region geometry is valid.
 * @pre The backend is bound and ``partition_base_lba`` is final.
 * @post ``m->exfat_upcase_ok`` is 0 or 1 and is never left unwritten.
 * @post No volume state is modified.
 * @note Not thread-safe; callers serialise mount operations.
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_upcase_verify(ra8_fs_mount_t* m);
