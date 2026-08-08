/**
 * @file ra8_fs_fat_exfat_dir_internal.h
 * @brief Cross-TU prototypes of the exFAT DIRECTORY seam.
 * @ingroup grp_storage
 *
 * @details
 * The adapter's general cross-TU prototypes live in the two alphabetical halves
 * `ra8_fs_fat_protos_a_internal.h` and `ra8_fs_fat_protos_b_internal.h`. The
 * helpers declared here are kept apart from those for the same reason the VFAT
 * long-name write seam is: they are one feature rather than one letter range --
 * everything needed to turn exFAT from a flat root-only namespace into a tree
 * (#605). Splitting them out also keeps the two halves inside the 1000-line
 * source cap, which they were already close to.
 *
 * The seam spans four translation units, so the declarations cannot live in any
 * one of them:
 *
 * - `ra8_fs_fat_exfat_read.c`   -- the directory location + cursor primitives
 * - `ra8_fs_fat_exfat_dir.c`    -- resolution, `mkdir`, `rmdir`
 * - `ra8_fs_fat_exfat_write.c`  -- the free-slot scan a set is linked into
 * - `ra8_fs_fat_exfat_mutate.c` -- entry-set retirement and bitmap release
 *
 * Aggregated by the `ra8_fs_fat_internal.h` umbrella, like the other themed
 * sub-headers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_fs_fat_types_internal.h"

/* ===========================================================================
 * Where a directory is: locations and cursors.
 * ===========================================================================
 */

/**
 * @brief Fill @p out with the volume root's directory location.
 *
 * @details The root's first cluster comes from the VBR and its contents are
 *          always reached through the FAT -- exFAT has no other shape for it --
 *          so the run bound is left at 0.
 *
 * @param[in]  m   Mounted exFAT volume.
 * @param[out] out Receives the root's location.
 *
 * @return Nothing.
 *
 * @pre @p m and @p out are non-NULL; `m->type` is exFAT.
 * @pre The mount carries a parsed VBR.
 * @post `out->cluster` is `m->root_cluster`.
 * @post `out->contig_end` is 0 (FAT-chained).
 *
 * @note Pure accessor; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_dir_root(const ra8_fs_mount_t* m, exfat_dir_t* out);

/**
 * @brief Build a directory location from a Stream entry's allocation fields.
 *
 * @details A FAT-chained directory (`nofat == 0`) keeps a run bound of 0 and is
 *          walked through the FAT. A contiguous one records the cluster one past
 *          its last, derived from DataLength -- which for an exFAT directory is
 *          its ALLOCATION, not a byte count, so the division is exact for every
 *          directory this driver writes.
 *
 * @param[in]  m    Mounted exFAT volume.
 * @param[in]  strm The entry set's 32-byte Stream-extension entry.
 * @param[out] out  Receives the directory location.
 *
 * @return Nothing.
 *
 * @pre @p m, @p strm and @p out are non-NULL.
 * @pre @p strm came from ::priv_exfat_find or ::priv_exfat_find_set.
 * @post `out->cluster` is the entry's FirstCluster.
 * @post `out->contig_end` is 0, or that cluster plus the run's cluster count.
 *
 * @note Pure computation; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_dir_from_set(const ra8_fs_mount_t* m, const uint8_t* strm, exfat_dir_t* out);

/**
 * @brief Aim a fresh cursor at the start of @p dir.
 *
 * @details Copies the location's first cluster and run bound into the cursor and
 *          zeroes the position and scan counters, so every directory walk in the
 *          adapter starts the same way instead of open-coding the initialiser.
 *
 * @param[in]  dir Directory to walk.
 * @param[out] out Receives the initialised cursor.
 *
 * @return Nothing.
 *
 * @pre @p dir and @p out are non-NULL.
 * @pre @p dir was produced by ::priv_exfat_dir_root or ::priv_exfat_dir_from_set.
 * @post The cursor addresses the directory's first entry.
 * @post The cursor's scan counter is 0.
 *
 * @note Pure initialisation; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_exfat_cursor_init(const exfat_dir_t* dir, exfat_cursor_t* out);

/**
 * @brief Compute the cluster that follows @p cluster in a directory.
 *
 * @details Two shapes, one answer. A FAT-chained directory (the volume root, or
 *          a subdirectory some other implementation grew) follows
 *          ::priv_fat_get and stops at end-of-chain. A contiguous run -- what
 *          this driver creates, with NoFatChain set -- steps to the adjacent
 *          cluster and stops at @p contig_end. Following the FAT for a
 *          NoFatChain run is not merely redundant: the exFAT specification
 *          declares those FAT entries invalid, and the ones this driver leaves
 *          behind read as 0 (free), which ::priv_is_eoc does not call
 *          end-of-chain, so the walk would continue into cluster 0.
 *
 * @param[in]  m          Mounted exFAT volume.
 * @param[in]  cluster    The directory's current cluster.
 * @param[in]  contig_end One past the run's last cluster; 0 => follow the FAT.
 * @param[out] out_next   Receives the successor cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok            ``*out_next`` is the next cluster.
 * @retval k_ra8_err_not_found The directory ends here.
 * @retval k_ra8_err_*         FAT read failure.
 *
 * @pre @p m and @p out_next are non-NULL.
 * @pre @p cluster belongs to the directory being walked.
 * @post On k_ra8_ok ``*out_next`` addresses a heap cluster.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @par MC/DC:
 * Decision: `contig_end != 0` (1 condition).
 * - a subdirectory created here (NoFatChain) -> true  -> step to cluster + 1.
 * - the volume root (FAT-chained)            -> false -> follow the FAT.
 * Decision: `adjacent >= contig_end` (1 condition), reached only on the true
 * arm above.
 * - the last cluster of the run   -> true  -> the directory ends.
 * - a run of two or more clusters -> false -> the adjacent cluster.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_step_cluster(const ra8_fs_mount_t* m,
                                  uint32_t              cluster,
                                  uint32_t              contig_end,
                                  uint32_t*             out_next);

/* ===========================================================================
 * Which directory a path names.
 * ===========================================================================
 */

/**
 * @brief Resolve every component of @p path but the last.
 *
 * @details Walks the path component by component from the volume root,
 *          descending into each intermediate name and requiring it to be a
 *          directory, then hands back that directory plus a pointer INTO
 *          @p path at the leaf. Runs of slashes are skipped, so `"/a//b"` and
 *          `"a/b"` resolve alike, and a trailing slash yields an empty leaf --
 *          which each caller rejects or interprets for itself. The exFAT mirror
 *          of ::priv_resolve_parent, and iterative for the same reason: NASA
 *          Power of 10 Rule 1 forbids recursion.
 *
 * @param[in]  m          Mounted exFAT volume.
 * @param[in]  path       NUL-terminated path.
 * @param[out] out_parent Receives the directory holding the leaf.
 * @param[out] out_leaf   Receives a pointer to the leaf within @p path.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Resolved; outputs populated.
 * @retval k_ra8_err_invalid_arg    A component names a FILE, is over-long, or
 *                                  the path is deeper than ::k_exfat_path_depth.
 * @retval k_ra8_err_not_found      An intermediate component does not exist.
 * @retval k_ra8_err_protocol_error A directory entry claims no data cluster.
 * @retval k_ra8_err_*              Backend read failure.
 *
 * @pre Every pointer argument is non-NULL; `m->type` is exFAT.
 * @pre @p path outlives the returned leaf pointer.
 * @post On success ``*out_leaf`` points into @p path.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_resolve_parent(const ra8_fs_mount_t* m,
                                    const char*           path,
                                    exfat_dir_t*          out_parent,
                                    const char**          out_leaf);

/**
 * @brief Resolve a whole path to the directory it names.
 *
 * @details `""` and `"/"` are the volume root; anything else is resolved to its
 *          parent and then descended into. A trailing slash names the same
 *          directory as the path without one.
 *
 * @param[in]  m    Mounted exFAT volume.
 * @param[in]  path NUL-terminated directory path.
 * @param[out] out  Receives the directory location.
 *
 * @return Error code.
 * @retval k_ra8_ok              Resolved; @p out populated.
 * @retval k_ra8_err_invalid_arg @p path names a FILE, or a component does.
 * @retval k_ra8_err_not_found   A component does not exist.
 * @retval k_ra8_err_*           Backend read failure.
 *
 * @pre @p m, @p path and @p out are non-NULL; `m->type` is exFAT.
 * @pre The volume is mounted.
 * @post On success @p out locates an existing directory.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_resolve_dir(const ra8_fs_mount_t* m, const char* path, exfat_dir_t* out);

/**
 * @brief Resolve a path and return the leaf entry set's location fields.
 *
 * @details ::priv_exfat_resolve_parent followed by ::priv_exfat_find in the
 *          resolved directory -- the nested-path replacement for what used to be
 *          a flat root-level ::priv_exfat_find on the whole path.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  path     NUL-terminated path.
 * @param[out] out_strm Receives the matched 32-byte Stream-extension entry.
 * @param[out] out_attr Receives the FileAttributes low byte.
 *
 * @return Error code.
 * @retval k_ra8_ok              Found; outputs populated.
 * @retval k_ra8_err_invalid_arg @p path names the volume root, or a component
 *                               names a file.
 * @retval k_ra8_err_not_found   No such entry.
 * @retval k_ra8_err_*           Backend read failure.
 *
 * @pre Every pointer argument is non-NULL; `m->type` is exFAT.
 * @pre The volume is mounted.
 * @post On success the outputs describe one on-disk entry set.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_lookup(const ra8_fs_mount_t* m, const char* path, uint8_t* out_strm, uint8_t* out_attr);

/* ===========================================================================
 * Making and unmaking a directory.
 * ===========================================================================
 */

/**
 * @brief Create one exFAT directory at @p path.
 *
 * @details Resolves the parent, refuses a name that already exists, reserves the
 *          parent's entry-slot run, allocates and zeroes one cluster, and links
 *          a File + Stream + Name set carrying ::k_exfat_attr_directory. There
 *          are no "." / ".." entries to write -- exFAT has none -- so an
 *          all-zero cluster is already a valid empty directory. On a failure
 *          after allocation the cluster is returned to the bitmap, so the volume
 *          is never leaked.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] path NUL-terminated directory path to create.
 *
 * @return Error code.
 * @retval k_ra8_ok              Directory created.
 * @retval k_ra8_err_invalid_arg The leaf is empty (the root) or over-long.
 * @retval k_ra8_err_exists      The name already exists in the parent.
 * @retval k_ra8_err_not_found   An intermediate component does not exist.
 * @retval k_ra8_err_no_mem      Parent directory full, or volume full.
 * @retval k_ra8_err_*           Bitmap or backend failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is exFAT.
 * @pre The parent path exists.
 * @post On success @p path resolves to an empty directory.
 * @post On failure no cluster is leaked.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_mkdir(const ra8_fs_mount_t* m, const char* path);

/**
 * @brief Remove one EMPTY exFAT directory at @p path.
 *
 * @details Locates the entry set, requires it to be a directory, proves the
 *          directory holds no live entry, then clears the in-use bit on every
 *          entry of the set and frees its clusters in the allocation bitmap. The
 *          emptiness proof runs before anything is written, so a refused removal
 *          changes nothing on disk.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] path NUL-terminated directory path to remove.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Directory removed.
 * @retval k_ra8_err_invalid_arg    @p path is the root, or names a FILE.
 * @retval k_ra8_err_not_found      No such entry, or a component is missing.
 * @retval k_ra8_err_not_empty      The directory still holds entries.
 * @retval k_ra8_err_protocol_error The entry claims no data cluster.
 * @retval k_ra8_err_*              Bitmap or backend failure.
 *
 * @pre @p m and @p path are non-NULL; `m->type` is exFAT.
 * @pre No open file handle refers to an entry inside @p path.
 * @post On success @p path no longer resolves and its clusters are free.
 * @post On any refusal the volume is unchanged.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_rmdir(const ra8_fs_mount_t* m, const char* path);

/* ===========================================================================
 * Finding room for a set, and giving space back.
 * ===========================================================================
 */

/**
 * @brief Clear a contiguous cluster run in the allocation bitmap.
 *
 * @details Mirror of ::priv_exfat_bitmap_mark: clears one bit per cluster,
 *          batching read-modify-write per bitmap sector, and pulls the mount's
 *          next-free hint back to the space just released.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] bmp_lba First LBA (volume-relative) of the bitmap.
 * @param[in] clus    First cluster of the run.
 * @param[in] count   Number of clusters to mark free.
 *
 * @return Error code.
 * @retval k_ra8_ok    All bits cleared and written.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m is non-NULL; the run is within the bitmap.
 * @pre The entries referencing the run are already retired.
 * @post The @p count bits for the run read as 0.
 * @post Only the affected bitmap sectors are rewritten.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_bitmap_clear(const ra8_fs_mount_t* m, uint32_t bmp_lba, uint32_t clus, uint32_t count);

/**
 * @brief Find @p need consecutive free entry slots inside one directory cluster.
 *
 * @details Walks @p dir cluster by cluster, scanning each for a run of reusable
 *          slots -- end-of-directory markers and retired entries both qualify.
 *          A set is never split across two clusters, so the run must fit inside
 *          one. A directory with no such run reports ::k_ra8_err_no_mem rather
 *          than growing: this driver does not extend a directory yet (#605).
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[in]  dir      Directory to search.
 * @param[in]  need     Number of consecutive free entries required.
 * @param[out] out_clus Receives the cluster holding the run.
 * @param[out] out_idx  Receives the run's first entry index in that cluster.
 *
 * @return Error code.
 * @retval k_ra8_ok         A run was found.
 * @retval k_ra8_err_no_mem The directory has no run of @p need slots.
 * @retval k_ra8_err_*      Backend read failure.
 *
 * @pre Every pointer argument is non-NULL; @p need >= 1.
 * @pre `m->type` is exFAT.
 * @post On success the run location is returned.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_find_dir_space(const ra8_fs_mount_t* m,
                                    const exfat_dir_t*    dir,
                                    uint32_t              need,
                                    uint32_t*             out_clus,
                                    uint32_t*             out_idx);

/* ===========================================================================
 * Retiring an entry set.
 * ===========================================================================
 */

/**
 * @brief Retire an entry set by clearing the in-use bit on every entry.
 *
 * @details exFAT's delete marker is bit 7 of the type byte, so an entry keeps
 *          its type and its contents and simply stops being live. Shared by
 *          ::priv_exfat_unlink_at and ::priv_exfat_rmdir, which differ in what
 *          they prove BEFORE reaching this point, not in how they retire a set.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] pos   Positions from ::priv_exfat_find_set.
 * @param[in] count Number of positions.
 *
 * @return Error code.
 * @retval k_ra8_ok    Every entry rewritten with its in-use bit clear.
 * @retval k_ra8_err_* Backend read/write failure.
 *
 * @pre @p m and @p pos are non-NULL; @p count is the set's entry count.
 * @pre @p pos came from ::priv_exfat_find_set on this volume.
 * @post The name no longer resolves.
 * @post No cluster is freed here.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_drop_set(const ra8_fs_mount_t* m, const exfat_setpos_t* pos, uint32_t count);

/**
 * @brief Delete a FILE named @p name from @p dir.
 *
 * @details The directory-scoped half of ::priv_exfat_unlink: locates the entry
 *          set, refuses one carrying ::k_exfat_attr_directory, retires it and
 *          frees its clusters. ::priv_exfat_create calls it directly, having
 *          already resolved the parent, so a create-over-an-existing-name
 *          replaces rather than duplicates (#603).
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] dir  Directory holding the name.
 * @param[in] name Leaf name (ASCII).
 *
 * @return Error code.
 * @retval k_ra8_ok              File deleted.
 * @retval k_ra8_err_invalid_arg @p name is a DIRECTORY; `rmdir` is the verb.
 * @retval k_ra8_err_not_found   No such name in @p dir.
 * @retval k_ra8_err_*           Bitmap or backend failure.
 *
 * @pre @p m, @p dir and @p name are non-NULL; `m->type` is exFAT.
 * @pre No open file handle refers to the name.
 * @post On success the name no longer resolves and its clusters are free.
 * @post On ::k_ra8_err_invalid_arg the volume is unchanged.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_unlink_at(const ra8_fs_mount_t* m, const exfat_dir_t* dir, const char* name);

/* ===========================================================================
 * Volume label (the exFAT half of ra8_fs_{get,set}_label).
 * ===========================================================================
 */

/**
 * @brief Read an exFAT volume's label into an ASCII buffer.
 *
 * @details Scans the root directory for the Volume Label directory entry
 *          (exFAT spec sec 7.7): an in-use entry (type 0x83) yields its UTF-16LE
 *          label decoded to ASCII (low byte of each code unit), truncated to
 *          @p out_len; a cleared entry (type 0x03) or no entry at all yields the
 *          empty string. The exFAT half of ::ra8_fs_get_label.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[out] out     Buffer receiving the NUL-terminated label.
 * @param[in]  out_len Capacity of @p out in bytes (at least 1).
 *
 * @return Error code.
 * @retval k_ra8_ok    @p out holds the label (possibly empty).
 * @retval k_ra8_err_* Backend read failure, or a volume with no directory.
 *
 * @pre @p m and @p out are non-NULL; `m->type` is exFAT; `out_len >= 1`.
 * @pre The mount is in use.
 * @post @p out is NUL-terminated.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_get_label(const ra8_fs_mount_t* m, char* out, uint32_t out_len);

/**
 * @brief Set (or clear) an exFAT volume's label in place.
 *
 * @details Rewrites the root Volume Label directory entry (exFAT spec sec 7.7)
 *          with the new UTF-16LE label and character count, creating one at the
 *          end-of-directory position when the volume carries none. A NULL or
 *          empty @p label writes a zero-length label entry (the unlabelled
 *          form). The exFAT half of ::ra8_fs_set_label.
 *
 * @param[in] m     Mounted exFAT volume.
 * @param[in] label New label (<= 11 characters), or NULL / "" to clear it.
 *
 * @return Error code.
 * @retval k_ra8_ok            Label written.
 * @retval k_ra8_err_not_found The root directory could not be scanned.
 * @retval k_ra8_err_*         Backend read/write failure.
 *
 * @pre @p m is non-NULL; `m->type` is exFAT; the mount is in use.
 * @pre @p label, when non-NULL, is at most 11 characters.
 * @post On success a later ::priv_exfat_get_label reports @p label.
 * @post Only the Volume Label entry is modified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_set_label(const ra8_fs_mount_t* m, const char* label);
