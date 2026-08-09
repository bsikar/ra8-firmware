/**
 * @file ra8_fs_fat_protos_b_internal.h
 * @brief Cross-TU helper prototypes for the FAT/exFAT adapter (part B of 2).
 * @ingroup grp_storage
 *
 * @details
 * The second half of the FAT/exFAT adapter's cross-TU helper prototypes. Each
 * helper is defined `static`-free in exactly one FAT/exFAT translation unit and
 * called from at least one other. This part covers the alphabetical run from
 * `priv_exfat_write_dir_set()` through `priv_write_sector()`; the earlier
 * helpers live in `ra8_fs_fat_protos_a_internal.h`. Both are aggregated by the
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

/**
 * @brief Write a pre-built entry set into consecutive directory entries.
 *
 * @details Read-modify-writes each entry slot so neighbouring entries are preserved.
 *
 * @param[in] m       Mounted exFAT volume.
 * @param[in] cluster Directory cluster holding the run.
 * @param[in] idx     Entry index of the run start.
 * @param[in] set     Built entry-set bytes.
 * @param[in] bytes   Total size of the set.
 * @return Error code.
 * @retval k_ra8_ok    Entries written.
 * @retval k_ra8_err_* Backend read/write failure.
 * @pre @p m and @p set are non-NULL; the run fits in the cluster.
 * @pre @p bytes is a multiple of the entry size.
 * @post The directory holds the new entry set.
 * @post Read-modify-write preserves neighbouring entries.
 * @note Writes one entry (sector RMW) at a time.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_write_dir_set(const ra8_fs_mount_t* m,
                                   uint32_t              cluster,
                                   uint32_t              idx,
                                   const uint8_t*        set,
                                   uint32_t              bytes);

/**
 * @brief Write the canonical exFAT up-case table and return its checksum.
 *
 * @details Streams the 5836-byte Microsoft up-case table
 * (::k_exfat_fmt_upc_std_bytes, embedded in `ra8_fs_fat_exfat_upcase.c`) to the
 * device starting at absolute LBA @p abs_lba, one sector at a time across
 * ::k_exfat_fmt_upc_std_secs sectors, zero-padding the final partial sector.
 * The rotate-add checksum (::priv_exfat_csum32) is accumulated over exactly the
 * table bytes -- not the pad -- so it equals the well-known 0xE619D30D and can
 * be stamped into the root Up-case directory entry.
 *
 * @param[in]  backend  Block-device backend with a non-NULL `write_block`.
 * @param[in]  abs_lba  Absolute (partition-adjusted) first LBA of the up-case
 *                      table's cluster run.
 * @param[out] out_csum Receives the table checksum on success.
 * @return Error code from the backend.
 * @retval k_ra8_ok    Table written; @p out_csum populated.
 * @retval k_ra8_err_* Backend `write_block` failure; @p out_csum unspecified.
 * @pre @p backend and @p backend->write_block are non-NULL.
 * @pre @p out_csum is non-NULL; @p abs_lba's cluster run holds the table span.
 * @post On k_ra8_ok the up-case cluster run holds the canonical table.
 * @post On k_ra8_ok @p out_csum holds the checksum for the root Up-case entry.
 * @note Not thread-safe; uses the module scratch buffer.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t
priv_exfat_write_upcase(const ra8_fs_backend_t* backend, uint32_t abs_lba, uint32_t* out_csum);

/**
 * @brief Clear then set attribute bits in a 32-byte FAT directory entry.
 *
 * @details Rewrites the entry's `DIR_Attr` byte as
 *          `(attr & ~clear_mask) | set_mask`: bits in @p clear_mask are
 *          cleared, bits in @p set_mask are set, every other bit is left as it
 *          was. The two masks are applied in that order, so a bit named in both
 *          ends up set. This is the one place the attribute byte is patched --
 *          the archive-on-write convention (`priv_truncate_existing`,
 *          `priv_write_locked`) and `ra8_fs_set_attr()` both route through it,
 *          so the read-modify-write cast lives once.
 *
 * @param[in,out] entry      32-byte directory entry to update.
 * @param[in]     set_mask   Attribute bits to set.
 * @param[in]     clear_mask Attribute bits to clear.
 *
 * @pre `entry` is non-NULL and points to 32 writable bytes.
 * @pre Caller has staged the entry in a sector buffer to be written back.
 * @post `entry[DIR_Attr] == (old & ~clear_mask) | set_mask`.
 * @post No other byte of the entry is modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fat_entry_apply_attr(uint8_t* entry, uint8_t set_mask, uint8_t clear_mask);

/**
 * @brief Fetch the FAT entry for `cluster`, returning the next-cluster value.
 *
 * @details
 * On FAT12 a single entry can straddle two sectors, which is why we read
 * one sector at a time and re-read on overflow.
 *
 * @param[in]  m         Mount providing the FAT type and geometry.
 * @param[in]  cluster   Cluster whose FAT entry to read.
 * @param[out] out_value Receives the next-cluster value.
 *
 * @return Error code.
 * @retval k_ra8_ok    Entry read successfully.
 * @retval k_ra8_err_* Backend error from a sector read.
 *
 * @pre `m` and `out_value` are non-NULL.
 * @pre `cluster` is within the addressable cluster range.
 * @post On success, `*out_value` holds the FAT entry.
 * @post Stack buffers used; module scratch untouched.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fat_get(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t* out_value);

/**
 * @brief Write `value` into the FAT entry for `cluster` across every FAT copy.
 *
 * @details Walks `m->num_fats` FAT copies and dispatches to the
 *          appropriate FAT12/16/32 set helper.
 *
 * @param[in] m       Mount providing geometry, backend, and FAT type.
 * @param[in] cluster Cluster whose FAT entry to update.
 * @param[in] value   Value to write.
 *
 * @return Error code.
 * @retval k_ra8_ok    All FAT copies updated.
 * @retval k_ra8_err_* Backend or set-helper failure.
 *
 * @pre `m` is non-NULL with a valid backend and `num_fats >= 1`.
 * @pre `cluster` is within the addressable cluster range.
 * @post On success, every FAT copy reflects the new value.
 * @post On partial failure, FAT copies may be inconsistent.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fat_set(const ra8_fs_mount_t* m, uint32_t cluster, uint32_t value);

/**
 * @brief Pick the cluster size that lands the FAT cluster count in the right band.
 *
 * @details Sweeps `spc` upward through powers of two (1, 2, 4, ... up to
 *          `k_fmt_spc_max`). For FAT32 the count shrinks with larger clusters,
 *          so the first `spc` whose count is in-band (or below
 *          `k_fmt_fat32_clus_cap`) wins. For FAT12/16 a too-large count fails
 *          the lower clusters and a too-small count fails the larger ones, so
 *          the sweep accepts the first in-band hit. When `spc_hint` is zero the
 *          starting point for FAT32 comes from priv_fmt_fat32_default_spc(); for
 *          FAT12/16 it starts at 1. On success the geometry's
 *          `sectors_per_cluster`, `fat_size_sectors`, and `count_of_clusters`
 *          are populated.
 *
 * @param[in,out] g        Geometry with `type`, `total_sectors`, `reserved_sectors`,
 *                         `root_entries`, and `root_sectors` pre-filled.
 * @param[in]     spc_hint Caller-pinned cluster size (0 = auto-sweep).
 *
 * @return Error code.
 * @retval k_ra8_ok               Geometry chosen; output fields stored in @p g.
 * @retval k_ra8_err_invalid_size No cluster size yields a count in @p g->type's band.
 *
 * @pre @p g is non-NULL with the input fields (`type`, `total_sectors`,
 *      `reserved_sectors`, `root_entries`, `root_sectors`) already set.
 * @pre @p spc_hint is 0 or a power of two in the range [1, k_fmt_spc_max].
 * @post On k_ra8_ok, `g->sectors_per_cluster`, `g->fat_size_sectors`, and
 *       `g->count_of_clusters` are consistent and in-band for @p g->type.
 * @post On k_ra8_err_invalid_size, @p g is left partially written and must
 *       be discarded.
 *
 * @note Bounded loop (NASA Rule 2): at most k_fmt_spc_max+1 iterations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fmt_choose_geometry(ra8_fs_fmt_geom_t* g, uint32_t spc_hint);

/**
 * @brief Clear `count` blocks at `lba` to zero -- bulk-erase if the backend can.
 *
 * @details Tries `backend->erase_blocks` first: on flash media (SD) that erases
 *          a region internally in one operation, this avoids streaming tens of
 *          MB of zeros (~30 MB of FAT on a 128 GB FAT32 card). The erase path is
 *          only taken when the backend guarantees a zero read-back -- it signals
 *          inability with ::k_ra8_err_not_supported, on which (or when no erase
 *          hook is bound) this falls back to `priv_fmt_zero_run`. Any other
 *          backend error aborts.
 *
 * @param[in] backend Block-device backend.
 * @param[in] lba     First block to clear.
 * @param[in] count   Number of blocks to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok    The range now reads back as all-zero bytes.
 * @retval k_ra8_err_* A backend write/erase failure (other than not_supported).
 *
 * @pre @p backend is non-NULL with a non-NULL `write_block`.
 * @pre @p count blocks starting at @p lba lie within the device.
 * @post On success `[lba, lba+count)` reads back as zero.
 * @post No metadata is written; caller seeds the FAT afterwards.
 *
 * @note Not thread-safe; part of single-threaded format.
 *
 * @par MC/DC:
 * Decision: `if ((erase_blocks != nullptr) && (erase_blocks(...) == k_ra8_ok))`
 * (short-circuit AND). Erase is a pure optimization: any non-OK result (no hook,
 * not_supported, a card that erases to ones, or a hardware error) falls through
 * to the proven zero-write -- so erase failure is never fatal. Vectors:
 * - erase_blocks == NULL          -> C1 false (short-circuit) -> zero-run.
 * - erase_blocks returns k_ra8_ok  -> C1 true, C2 true         -> return ok (no zeroing).
 * - erase_blocks returns non-ok   -> C1 true, C2 false        -> zero-run.
 * (NULL,ok) prove C1 independence; (ok,non-ok) prove C2 independence.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fmt_clear_region(const ra8_fs_backend_t* backend, uint32_t lba, uint32_t count);

/**
 * @brief Lay down the boot sector, FAT seeds, FSInfo, and the empty root.
 *
 * @details The write phase of `ra8_fs_format()`, split out to keep the public
 *          entry under the NASA Rule 4 length budget. Clears the FAT + root
 *          region (bulk-erase or zero-write) first, then builds the
 *          type-specific BPB and persists it at LBA 0, seeds the FATs, and
 *          writes any FAT32 FSInfo + backup. The root region is left zero by the
 *          initial clear.
 *
 * @param[in] backend Block-device backend.
 * @param[in] g       Resolved geometry.
 * @param[in] label   Optional volume label.
 *
 * @return Error code.
 * @retval k_ra8_ok    Volume image fully written.
 * @retval k_ra8_err_* Backend write failure mid-format.
 *
 * @pre @p backend, @p g are non-NULL with geometry resolved.
 * @pre @p g->type is FAT12/FAT16/FAT32.
 * @post On success the device holds a mountable @p g->type volume.
 * @post On failure the device may be partially written.
 *
 * @note Not thread-safe; serialise with mounts on the same backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fmt_emit_volume(const ra8_fs_backend_t*  backend,
                               const ra8_fs_fmt_geom_t* g,
                               const char*              label);

/**
 * @brief Map a requested FAT type to its reserved-sector count.
 *
 * @details FAT32 reserves a 32-sector region (boot + FSInfo + backup);
 *          FAT12/16 reserve only the single boot sector.
 *
 * @param[in] type Requested FAT variant.
 *
 * @return Reserved-sector count for @p type.
 * @retval k_fmt_resv_f32 @p type is FAT32.
 * @retval k_fmt_resv_f16 @p type is FAT12 or FAT16.
 *
 * @pre @p type is one of FAT12/FAT16/FAT32.
 * @pre Caller has rejected exFAT/unknown beforehand.
 * @post No state modified.
 * @post Result is purely a function of @p type.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_fmt_reserved_for(ra8_fs_type_t type);

/**
 * @brief Pad an ASCII volume label into an 11-byte BS_VolLab / label field.
 *
 * @details Copies @p label up to its NUL (or 11 characters) then space-fills
 *          the remainder, matching the BS_VolLab convention. A NULL or empty
 *          @p label resolves to the FAT specification's unlabelled sentinel
 *          `"NO NAME    "` -- never zeros and never a bare run of spaces, both
 *          of which `fsck.fat` treats as a corrupt label and strips (#634).
 *          Shared by the formatter (`ra8_fs_format()`) and the runtime label
 *          writer (`ra8_fs_set_label()`), so both lay the field identically.
 *
 * @param[out] dst   Destination 11-byte label field.
 * @param[in]  label Source label, or NULL / "" for the unlabelled sentinel.
 *
 * @return Nothing.
 *
 * @pre @p dst is non-NULL and addresses at least ::k_fmt_label_len bytes.
 * @pre @p label is NUL-terminated when non-NULL.
 * @post @p dst holds the padded 11-byte label (or the `"NO NAME    "` sentinel).
 * @post No byte past offset 10 of @p dst is touched.
 *
 * @note Bounded loop (NASA Rule 2): exactly ::k_fmt_label_len iterations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fmt_label_field(uint8_t* dst, const char* label);

/**
 * @brief Validate a caller-pinned sectors-per-cluster value.
 *
 * @details A zero value defers to the auto-sweep. A non-zero value must be a
 *          power of two in the closed range 1..`k_fmt_spc_max`.
 *
 * @param[in] spc Requested cluster size.
 *
 * @return Whether @p spc is acceptable.
 * @retval true  @p spc is 0 (auto) or a power of two in 1..128.
 * @retval false @p spc is non-zero and not a valid power of two.
 *
 * @pre None (total function over uint8_t).
 * @pre Caller treats false as `k_ra8_err_invalid_arg`.
 * @post No state modified.
 * @post Result is purely a function of @p spc.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_fmt_spc_valid(uint8_t spc);

/**
 * @brief Free an entire cluster chain starting at `start`.
 *
 * @details Walks the chain via `priv_fat_get`, marking each cluster
 *          free. A guard counter bounds the loop against on-disk loops.
 *
 * @param[in] m     Mount providing FAT access.
 * @param[in] start First cluster of the chain.
 *
 * @return Error code.
 * @retval k_ra8_ok                 All clusters freed.
 * @retval k_ra8_err_protocol_error Loop detected in chain.
 * @retval k_ra8_err_*              Backend error.
 *
 * @pre `m` is non-NULL with a valid backend.
 * @pre `start` is a valid cluster number or sentinel.
 * @post On success, every cluster in the chain has FAT entry = 0.
 * @post On failure, FAT may be partially updated.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_free_chain(const ra8_fs_mount_t* m, uint32_t start);

/**
 * @brief Test whether `value` is an end-of-chain marker for this FAT type.
 *
 * @details EOC markers differ across FAT12/16/32.
 *
 * @param[in] m     Mount providing the FAT type.
 * @param[in] value FAT entry value to test.
 *
 * @return 1 if EOC, 0 otherwise.
 * @retval 1  `value` indicates end-of-chain.
 * @retval 0  `value` is a normal next-cluster pointer.
 *
 * @pre `m` is non-NULL.
 * @pre `value` was obtained from a FAT entry read.
 * @post No state modified.
 * @post Result is purely a function of inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_is_eoc(const ra8_fs_mount_t* m, uint32_t value);

/**
 * @brief Fold one LFN directory entry's 13 UTF-16LE code units into the state.
 *
 * @details Reads the sequence number from @p ent (low 5 bits of LDIR_Ord) to
 *          locate the unit group within the assembled name, then copies each of
 *          the 13 units at their VFAT byte offsets (LDIR_Name1/2/3) VERBATIM. A
 *          zero unit or the padding value (0xFFFF) terminates the group early.
 *          The stored checksum is updated from LDIR_Chksum. Out-of-range
 *          sequence numbers are silently ignored to tolerate a corrupt chain.
 *
 *          Units above 0x7F used to become `?`, which made the reported name
 *          one the caller could not hand back to `ra8_fs_open()` -- the file was
 *          listed and then unopenable, and two names differing only in an accent
 *          collided (#606).
 *
 * @param[in,out] s   Reassembly state being accumulated.
 * @param[in]     ent 32-byte raw LFN directory entry (attribute byte == 0x0F).
 *
 * @return Nothing.
 *
 * @pre @p s is non-NULL and was initialised by priv_lfn_reset().
 * @pre @p ent is non-NULL and points to exactly 32 valid bytes.
 * @post If the sequence number is in range, @p s->units and @p s->checksum
 *       reflect the units from this entry.
 * @post If the sequence number is out of range, @p s is unchanged.
 *
 * @note Not thread-safe; the caller serialises directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_lfn_add(lfn_state_t* s, const uint8_t* ent);

/**
 * @brief Code units of the chain that precedes @p name83, or NULL if none.
 *
 * @details Returns the reassembled name only when a chain was accumulated and
 *          its checksum matches @p name83, so a stray chain never aliases an
 *          entry. The units are returned rather than text because that is the
 *          domain a lookup compares in and the domain the up-case table folds;
 *          only the listing path converts, and only at the API boundary.
 *
 *          The length is the run of non-zero units, exactly as the NUL used to
 *          end the string: a group that never arrived leaves zeros, and the
 *          name stops there instead of running into another chain's characters.
 *
 * @param[in]  s         Reassembly state carried across the directory walk.
 * @param[in]  name83    The 8.3 entry the chain is claimed to belong to.
 * @param[out] out_units Receives the unit count (0 when there is no name).
 *
 * @return Pointer to the units, or nullptr.
 * @retval s->units The chain is present and binds to @p name83.
 * @retval nullptr  No chain, an empty one, or a checksum mismatch.
 *
 * @pre @p s and @p out_units are non-NULL; @p name83 addresses 11 bytes.
 * @pre @p s was initialised by priv_lfn_reset() before the walk.
 * @post `*out_units` is written on both outcomes.
 * @post @p s is not modified.
 *
 * @note Not thread-safe; the caller serialises directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
const uint16_t*
priv_lfn_units_for(const lfn_state_t* s, const uint8_t* name83, uint32_t* out_units);

/**
 * @brief Reset the LFN reassembly state so a fresh chain can start.
 *
 * @details Clears the accumulated unit array, then resets the
 *          stored checksum and the "have" flag to zero. Called at the start of
 *          a directory walk and whenever a deleted or consumed 8.3 entry breaks
 *          an in-progress chain.
 *
 * @param[in,out] s Reassembly state to reset.
 *
 * @return Nothing.
 *
 * @pre @p s is non-NULL.
 * @pre @p s was previously initialised (e.g. via zero-init or a prior reset).
 * @post @p s->units is all zero.
 * @post @p s->have and @p s->checksum are both zero.
 *
 * @note Not thread-safe; the caller serialises directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_lfn_reset(lfn_state_t* s);

/**
 * @brief Take the library lock, if the caller installed one.
 *
 * @details Invokes the installed ::ra8_fs_lock_t::acquire with its cookie. With
 *          no binding installed -- the bare-metal default -- this is a load and
 *          a branch and nothing else, which is why the seam costs the default
 *          world nothing. Called only by the public entry-point wrappers: an
 *          internal helper taking it a second time would deadlock a
 *          non-recursive mutex, and the `RA8_EXPECTS_LOCK("ra8_fs_lock")` tag
 *          on every guarded implementation is what enforces that.
 *
 * @return Nothing.
 *
 * @pre The calling thread does not already hold the lock.
 * @pre The installed binding (if any) is complete -- guaranteed by
 *      ::ra8_fs_set_lock, which rejects a half-filled one.
 * @post The lock is held, or no binding is installed.
 * @post No library state other than the caller's lock is touched.
 *
 * @note Pairs 1:1 with ::priv_lock_release on every return path.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_lock_acquire(void);

/**
 * @brief Drop the library lock taken by ::priv_lock_acquire.
 *
 * @details Invokes the installed ::ra8_fs_lock_t::release with its cookie, or
 *          does nothing when no binding is installed. This is the release half
 *          the annotation checker looks for when it decides whether a public
 *          wrapper discharged the ownership it took.
 *
 * @return Nothing.
 *
 * @pre A matching ::priv_lock_acquire ran on this thread.
 * @pre The binding has not changed since that call.
 * @post The lock is no longer held.
 * @post No library state other than the caller's lock is touched.
 *
 * @note Called on the success path and every error path of each wrapper.
 *
 * @since 0.1.0
 */
RA8_PRIV
RA8_RELEASES_RESOURCE("ra8_fs_lock")
void priv_lock_release(void);

/**
 * @brief Open a file by path -- the guarded body of ::ra8_fs_open().
 *
 * @details Carries the whole contract documented for ::ra8_fs_open() in
 *          `ra8_fs.h`; the public symbol is the wrapper that brackets this
 *          call with ::priv_lock_acquire / ::priv_lock_release. Exposed across
 *          translation units because ::ra8_fs_write_file()'s guarded body has
 *          to reach it without taking the lock a second time.
 *
 * @param[in]  handle   Mount handle.
 * @param[in]  path     NUL-terminated path.
 * @param[in]  mode     Open mode.
 * @param[out] out_file Receives the open file handle.
 *
 * @return Error code.
 * @retval k_ra8_ok            File opened.
 * @retval k_ra8_err_null_ptr  Any pointer argument was NULL.
 * @retval k_ra8_err_*         As documented for ::ra8_fs_open().
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle`, `path`, and `out_file` are non-NULL.
 * @post On success `*out_file` is a valid open handle.
 * @post On failure no file slot is marked in use.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_PRIV
RA8_EXPECTS_LOCK("ra8_fs_lock")
ra8_err_t priv_open_locked(ra8_fs_mount_t* handle,
                           const char*     path,
                           ra8_fs_mode_t   mode,
                           ra8_fs_file_t** out_file);

/**
 * @brief Parse the BPB layout fields out of `s_scratch` into `m`.
 *
 * @details Validates the boot signature (0x55AA) and reads the BPB
 *          fields out of the boot sector scratch buffer.
 *
 * @param[in,out] m Mount to populate; backend already plugged in.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Fields parsed successfully.
 * @retval k_ra8_err_validation_failed  Bad signature or sanity-check fail.
 *
 * @pre `m` is non-NULL.
 * @pre `s_scratch` holds the boot sector (LBA 0).
 * @post On success, the relevant `m->*` fields are populated.
 * @post On failure, `m` may be partially updated.
 *
 * @note Not thread-safe -- uses module-level scratch.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_parse_bpb_into_mount(ra8_fs_mount_t* m);

/**
 * @brief Parse the volume at the current base: exFAT first, then FAT BPB.
 *
 * @details Dispatches to the exFAT parser when the VBR carries the exFAT
 * signature, else to the FAT BPB parser.
 *
 * @param[in,out] m Mount with sector 0 already read into ::s_scratch.
 * @return Error code from the chosen parser.
 * @retval k_ra8_ok    Volume parsed (FAT or exFAT).
 * @retval k_ra8_err_* No recognizable volume at this base.
 * @pre @p m is non-NULL and ::s_scratch holds the base sector 0.
 * @pre ``m->backend`` is bound.
 * @post On success ``m`` holds the volume geometry + type.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_parse_volume(ra8_fs_mount_t* m);

/**
 * @brief Convert a "/FILE.TXT"-style path to packed 11-byte 8.3 form.
 *
 * @details Strips leading `/`, pre-pads `out11` with spaces, calls the
 *          base/extension packers, and rewrites a leading 0xE5 byte to
 *          the kanji escape 0x05.
 *
 * @param[in]  path  NUL-terminated input path. Must be non-NULL.
 * @param[out] out11 11-byte output buffer. Must be non-NULL.
 *
 * @return 1 on success, 0 on invalid name.
 * @retval 1  Name packed into `out11`.
 * @retval 0  NULL input or name violates 8.3 rules.
 *
 * @pre `path` and `out11` are non-NULL when valid.
 * @pre `out11` has at least `k_max_8_3_name` writable bytes.
 * @post On success, `out11` holds the on-disk 8.3 representation.
 * @post On failure, `out11` content is unspecified.
 *
 * @note Pure ASCII upcase; no locale support.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_path_to_83(const char* path, uint8_t* out11);

/**
 * @brief Decode a little-endian uint16_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler. Avoids `memcpy` so
 *          clang-tidy's strict-alias check stays happy.
 *
 * @param[in] p Pointer to two bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT16_MAX  Value assembled from `p[0]` and `p[1]`.
 *
 * @pre `p` is non-NULL and points to at least 2 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1] << 8)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_rd16(const uint8_t* p);

/**
 * @brief Decode a little-endian uint32_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler for 4 bytes.
 *
 * @param[in] p Pointer to four bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT32_MAX  Value assembled from `p[0..3]`.
 *
 * @pre `p` is non-NULL and points to at least 4 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_rd32(const uint8_t* p);

/**
 * @brief Read a single sector into the module scratch buffer.
 *
 * @details Forwards to the mount's `backend.read_block` callback.
 *
 * @param[in]  m   Mount whose backend to use.
 * @param[in]  lba Logical block address to read.
 * @param[out] buf Destination of `k_ra8_fs_bytes_per_sector` bytes.
 *
 * @return Backend-supplied error code.
 * @retval k_ra8_ok    Sector read successfully.
 * @retval k_ra8_err_* Whatever the backend returned.
 *
 * @pre `m`, `m->backend.read_block`, and `buf` are non-NULL.
 * @pre `lba` is within the volume's addressable range.
 * @post On success, `buf` holds the sector contents.
 * @post On failure, `buf` content is undefined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_read_sector(const ra8_fs_mount_t* m, uint32_t lba, uint8_t* buf);

/**
 * @brief Resolve a whole path to the directory it names.
 *
 * @details The empty path or `"/"` resolves to the volume root; otherwise the
 *          parent is resolved and the final component is entered as a
 *          subdirectory. Used by `ra8_fs_listdir` to walk any directory.
 *
 * @param[in]  m    Mount providing geometry and backend.
 * @param[in]  path NUL-terminated directory path.
 * @param[out] out  Receives the resolved directory location.
 *
 * @return Error code.
 * @retval k_ra8_ok              Directory resolved.
 * @retval k_ra8_err_invalid_arg A component is not a directory / not 8.3.
 * @retval k_ra8_err_not_found   A component does not exist.
 * @retval k_ra8_err_*           Backend error.
 *
 * @pre `m`, `path`, and `out` are non-NULL.
 * @pre `path` is NUL-terminated.
 * @post On success `out` locates the named directory.
 * @post On failure `out` is unspecified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_resolve_dir(const ra8_fs_mount_t* m, const char* path, dir_loc_t* out);

/**
 * @brief Resolve all-but-the-last path component to a parent directory.
 *
 * @details Splits @p path on `/`, descending through each intermediate
 *          component (which must be an existing subdirectory) and returning the
 *          final component as @p out_leaf. A flat name (no embedded `/`) yields
 *          the root as the parent and the whole name as the leaf -- the legacy
 *          root-only behaviour. The walk is bounded by ::k_path_max_depth.
 *
 * @param[in]  m          Mount providing geometry and backend.
 * @param[in]  path       NUL-terminated path (leading slashes ignored).
 * @param[out] out_parent Receives the resolved parent directory location.
 * @param[out] out_leaf   Receives a pointer into @p path at the final component.
 *
 * @return Error code.
 * @retval k_ra8_ok              Parent resolved; @p out_leaf set.
 * @retval k_ra8_err_invalid_arg A component is not 8.3, or the path is too deep.
 * @retval k_ra8_err_not_found   An intermediate component does not exist.
 * @retval k_ra8_err_*           Backend error.
 *
 * @pre `m`, `path`, `out_parent`, and `out_leaf` are non-NULL.
 * @pre `path` is NUL-terminated.
 * @post On success `out_parent` locates the leaf's parent directory.
 * @post On failure the out parameters are unspecified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_resolve_parent(const ra8_fs_mount_t* m,
                              const char*           path,
                              dir_loc_t*            out_parent,
                              const char**          out_leaf);

/**
 * @brief Length of a NUL-terminated string.
 *
 * @details Counts bytes up to the NUL terminator.
 *
 * @param[in] s NUL-terminated string.
 * @return Character count before the terminator.
 * @retval 0..UINT32_MAX String length.
 * @pre @p s is non-NULL.
 * @pre @p s is NUL-terminated.
 * @post No state modified.
 * @post @p s is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_strlen(const char* s);

/**
 * @brief Upper-case ASCII conversion (returns input unchanged if not lowercase).
 *
 * @details Locale-independent ASCII upcase.
 *
 * @param[in] c Input character.
 *
 * @return Upper-case form of `c` if it was lower-case ASCII, else `c`.
 * @retval 'A'..'Z'   Upper-cased input.
 * @retval c          Otherwise unchanged.
 *
 * @pre None.
 * @pre Caller wants ASCII-only handling.
 * @post No state modified.
 * @post Result is purely a function of `c`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
char priv_to_upper(char c);

/**
 * @brief Encode a little-endian uint16_t into a byte buffer.
 *
 * @details Inverse of `priv_rd16`. Writes the low byte first.
 *
 * @param[out] p Pointer to two writable bytes.
 * @param[in]  v Value to encode.
 *
 * @pre `p` is non-NULL and points to at least 2 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0]` and `p[1]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_wr16(uint8_t* p, uint16_t v);

/**
 * @brief Encode a little-endian uint32_t into a byte buffer.
 *
 * @details Inverse of `priv_rd32`. Writes lowest byte first.
 *
 * @param[out] p Pointer to four writable bytes.
 * @param[in]  v Value to encode.
 *
 * @pre `p` is non-NULL and points to at least 4 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0..3]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_wr32(uint8_t* p, uint32_t v);

/**
 * @brief Merge @p put bytes into one sector at @p lba, at @p off_in_sector.
 *
 * @details Read-modify-write of a single sector. Every partial-sector update in
 *          this adapter goes through it, so a write that does not start or end
 *          on a sector boundary cannot destroy the neighbouring bytes -- which,
 *          inside a cluster, belong to the same file, and at a cluster edge may
 *          belong to another.
 *
 * @param[in] m             Mount providing the backend.
 * @param[in] lba           Volume-relative sector to update.
 * @param[in] off_in_sector Byte offset within the sector.
 * @param[in] src           Source bytes.
 * @param[in] put           Number of bytes to write.
 *
 * @return Error code.
 * @retval k_ra8_ok    Sector updated.
 * @retval k_ra8_err_* Backend read or write failure.
 *
 * @pre `m` and `src` are non-NULL.
 * @pre `off_in_sector + put <= k_ra8_fs_bytes_per_sector`.
 * @post On success the sector reflects the merged content.
 * @post On failure the sector content is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_write_into_sector(const ra8_fs_mount_t* m,
                                 uint32_t              lba,
                                 uint32_t              off_in_sector,
                                 const uint8_t*        src,
                                 uint32_t              put);

/**
 * @brief Write a single sector from a caller-provided buffer.
 *
 * @details Forwards to the mount's `backend.write_block` callback.
 *
 * @param[in] m   Mount whose backend to use.
 * @param[in] lba Logical block address to write.
 * @param[in] buf Source of `k_ra8_fs_bytes_per_sector` bytes.
 *
 * @return Backend-supplied error code.
 * @retval k_ra8_ok    Sector written successfully.
 * @retval k_ra8_err_* Whatever the backend returned.
 *
 * @pre `m`, `m->backend.write_block`, and `buf` are non-NULL.
 * @pre `lba` is within the volume's addressable range.
 * @post On success, the underlying backend has the new sector contents.
 * @post On failure, backend state is implementation-defined.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_write_sector(const ra8_fs_mount_t* m, uint32_t lba, const uint8_t* buf);
