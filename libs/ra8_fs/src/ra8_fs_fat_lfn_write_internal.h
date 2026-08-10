/**
 * @file ra8_fs_fat_lfn_write_internal.h
 * @brief Cross-TU prototypes of the VFAT long-name WRITE seam.
 * @ingroup grp_storage
 *
 * @details
 * The adapter's general cross-TU prototypes live in the two alphabetical
 * halves `ra8_fs_fat_protos_a_internal.h` and `ra8_fs_fat_protos_b_internal.h`.
 * The helpers declared here are kept apart from those because they are one
 * feature rather than one letter range: everything `open`, `mkdir`, `rename`,
 * `unlink` and `rmdir` need in order to FILE a name that is not
 * 8.3-representable, find it again, and take it away without leaving the chain
 * behind. Splitting them out also keeps the two halves inside the 1000-line
 * source cap, which they were already close to.
 *
 * The seam spans three translation units, so the declarations cannot live in
 * any one of them:
 *
 * - `ra8_fs_fat_name.c`      -- ::priv_name_classify(), ::priv_lfn_alias_basis()
 * - `ra8_fs_fat_lfn.c`       -- ::priv_sfn_checksum(), ::priv_lfn_fill_slot()
 * - `ra8_fs_fat_lfn_write.c` -- the directory-slot verbs
 *
 * Aggregated by the `ra8_fs_fat_internal.h` umbrella, like the other two.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_fs_fat_types_internal.h"

/* ===========================================================================
 * The name domain: what shape a leaf needs, and what alias it is filed under.
 * ===========================================================================
 */

/**
 * @brief Decide whether a leaf needs one entry, one entry plus case flags, or a chain.
 *
 * @details Decodes @p leaf from UTF-8 into the code units the on-disk chain
 *          carries, which is where a malformed encoding is refused -- an
 *          over-long form, a raw surrogate, a truncated sequence -- and then
 *          rejects anything a long name may not hold at all (an empty leaf, one
 *          longer than ::k_lfn_write_max CODE UNITS, a control character, or one
 *          of the FAT specification's illegal characters). Characters above
 *          ASCII are legal: the slots are UTF-16 and the reader returns them as
 *          such. What is left is an 8.3 name
 *          if it has a 1..8 character base, a 0 or 1..3 character extension, at
 *          most one dot, and no character an 8.3 field cannot hold -- a space
 *          and a second dot are both enough to disqualify it however short it
 *          is. An 8.3 name whose base or extension is all one case round-trips
 *          through `DIR_NTRes`; one that MIXES cases in either half does not,
 *          and is reported as needing a chain.
 *
 * @param[in]  leaf       Leaf component (no slashes), NUL-terminated UTF-8.
 * @param[out] out_units  Receives @p leaf as UTF-16LE code units.
 * @param[out] out_nunits Receives how many units that is.
 * @param[out] out83      Receives the packed 11-byte name on a short verdict.
 * @param[out] out_ntres  Receives the `DIR_NTRes` case flags (0 unless short).
 *
 * @return The on-disk shape required.
 * @retval k_name_kind_short   @p out83 and @p out_ntres are ready to write.
 * @retval k_name_kind_long    A chain plus a generated alias is required.
 * @retval k_name_kind_invalid Empty, over-long, illegal, or not valid UTF-8.
 *
 * @pre Every pointer is non-NULL.
 * @pre @p out_units addresses ::k_lfn_write_max writable units and @p out83
 *      addresses ::k_max_8_3_name writable bytes.
 * @post @p out_ntres and @p out_nunits are written on every outcome.
 * @post @p leaf is not modified.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_fs_name_kind_t priv_name_classify(const char* leaf,
                                      uint16_t*   out_units,
                                      uint32_t*   out_nunits,
                                      uint8_t*    out83,
                                      uint8_t*    out_ntres);

/**
 * @brief Derive the `LONGNA~N.TXT` 8.3 alias a long name is filed under.
 *
 * @details The Microsoft FAT specification's basis-name generation: spaces and
 *          all but the last interior dot are dropped, the remainder is
 *          upper-cased with anything an 8.3 field cannot hold replaced by `_`,
 *          and the base is truncated to leave room for `~` plus the decimal
 *          @p tail. A leaf that contributes no base characters at all (`"..."`)
 *          yields `_`, because an 8.3 name may not have an empty base.
 *
 *          Every unit above ASCII maps to `_`, which is what VFAT does with a
 *          character its OEM code page cannot express. Working from CODE UNITS
 *          rather than UTF-8 bytes is what makes that one underscore per
 *          character instead of one per byte.
 *
 * @param[in]  leaf  Long name being filed, as UTF-16 code units.
 * @param[in]  n     Number of units in @p leaf.
 * @param[in]  tail  Sequence number, 1..::k_lfn_alias_tail_max.
 * @param[out] out11 Receives the packed, space-padded 11-byte alias.
 *
 * @return Nothing.
 *
 * @pre @p leaf and @p out11 are non-NULL; @p out11 holds ::k_max_8_3_name bytes.
 * @pre @p tail is at least 1 and at most ::k_lfn_alias_tail_max.
 * @post Every byte of @p out11 is a legal 8.3 character or a space pad.
 * @post @p out11 always contains a `~` followed by @p tail in decimal.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_lfn_alias_basis(const uint16_t* leaf, uint32_t n, uint32_t tail, uint8_t* out11);

/* ===========================================================================
 * The entry domain: one long-name slot, and the checksum that binds the chain.
 * ===========================================================================
 */

/**
 * @brief Compute the 8.3 short-name checksum carried in each LFN directory entry.
 *
 * @details Implements the rotate-right-add algorithm from the Microsoft FAT
 *          spec section 7 ("Long File Name Directory Entries"). Each of the
 *          eleven bytes of the 8.3 name field is folded into a running sum: the
 *          previous sum is rotated right by one bit (preserving the low bit)
 *          and the next byte is added. The result binds a set of LFN slots to
 *          their trailing 8.3 entry so that a chain cannot alias a different
 *          short name.
 *
 * @param[in] name83 Pointer to the 11-byte 8.3 name field (DIR_Name in the FAT
 *                   spec, space-padded, not NUL-terminated).
 *
 * @return Computed 8-bit checksum.
 * @retval 0..255 The folded checksum; all values are possible.
 *
 * @pre @p name83 is non-NULL and points to at least 11 valid bytes.
 * @pre The 11-byte field is the raw DIR_Name from a valid FAT directory entry.
 * @post Return value matches the Checksum field in every associated LFN entry.
 * @post No bytes of @p name83 are modified (read-only access).
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_sfn_checksum(const uint8_t* name83);

/**
 * @brief Fill one 32-byte slot with the @p order -th group of a long name.
 *
 * @details Writes the thirteen UTF-16LE code units of group @p order into the
 *          three character runs of an attr-0x0F entry, exactly as
 *          ::priv_lfn_add() reads them back. The group that ends the name
 *          carries the NUL terminator immediately after its last character and
 *          0xFFFF in every slot past it -- the padding a scanner uses to know
 *          the name stopped short of the group boundary. A name whose length
 *          is an exact multiple of thirteen therefore carries no terminator at
 *          all, which is what the specification requires and what the
 *          reassembler already handles by running out of groups.
 *
 * @param[out] ent     32-byte slot to fill; zeroed by this function first.
 * @param[in]  name    The long name, as UTF-16LE code units.
 * @param[in]  nlen    Its length in code units.
 * @param[in]  order   1-based group index (1 = the first thirteen characters).
 * @param[in]  is_last Non-zero for the LAST logical group, which is the
 *                     physically FIRST slot and carries ::k_lfn_seq_last.
 * @param[in]  csum    ::priv_sfn_checksum() of the 8.3 entry closing the chain.
 *
 * @return Nothing.
 *
 * @pre @p ent and @p name are non-NULL; @p ent addresses 32 writable bytes.
 * @pre @p order is at least 1 and `(order - 1) * 13` is at most @p nlen.
 * @post `ent[k_dir_off_attr]` is ::k_ra8_fs_attr_lfn and `LDIR_FstClusLO` is 0.
 * @post No byte of @p ent outside the 32-byte slot is written.
 *
 * @note Not thread-safe against the same @p ent; trivially so otherwise.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_lfn_fill_slot(uint8_t*        ent,
                        const uint16_t* name,
                        uint32_t        nlen,
                        uint32_t        order,
                        uint8_t         is_last,
                        uint8_t         csum);

/* ===========================================================================
 * The directory domain: reserve, commit, look up, erase.
 * ===========================================================================
 */

/**
 * @brief Locate @p need consecutive free entry slots in a given directory.
 *
 * @details Walks @p loc and returns a cursor on the first slot of the first run
 *          of @p need slots whose name field is 0x00 (never used) or 0xE5
 *          (deleted). A long name occupies its chain and its 8.3 entry in one
 *          unbroken run, which is why the free slots have to be found together
 *          rather than one at a time; `need == 1` is the ordinary short-name
 *          case and behaves like a first-free-slot search.
 *
 * @param[in]  m    Mount providing geometry and backend.
 * @param[in]  loc  Directory to search (root or a subdirectory).
 * @param[in]  need Consecutive slots required; at least 1.
 * @param[out] out  Receives a cursor addressing the run's first slot.
 *
 * @return Error code.
 * @retval k_ra8_ok         Run found; @p out is positioned on it.
 * @retval k_ra8_err_no_mem The directory holds no run that long.
 * @retval k_ra8_err_*      Backend error.
 *
 * @pre All pointers are non-NULL and @p need is at least 1.
 * @pre @p m is mounted with valid geometry.
 * @post On success every slot of the run is free.
 * @post No on-disk state is modified.
 *
 * @note Thread-safety inherited from the backend.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_find_free_run(const ra8_fs_mount_t* m,
                                 const dir_loc_t*      loc,
                                 uint32_t              need,
                                 dir_slot_t*           out);

/**
 * @brief Decide how a name will be stored and set aside the slots for it.
 *
 * @details Classifies @p leaf, generates a unique `~N` alias when it needs a
 *          long-name chain, and reserves a run of free slots long enough for
 *          the chain plus its 8.3 entry -- growing the directory by a cluster
 *          when no run is long enough, which a FAT12/16 fixed root cannot do.
 *          Nothing is written: this is the half that is allowed to fail, so a
 *          caller that must allocate something first (`mkdir` needs a cluster
 *          for the new directory) can fail before it does.
 *
 * @param[in]  m    Mounted FAT12/16/32 volume.
 * @param[in]  loc  Directory the entry is going into.
 * @param[in]  leaf Leaf component (no slashes); must outlive the commit.
 * @param[out] out  Receives the reservation.
 *
 * @return Error code.
 * @retval k_ra8_ok              Slots reserved; @p out is ready to commit.
 * @retval k_ra8_err_invalid_arg @p leaf is empty, over-long, or illegal.
 * @retval k_ra8_err_no_mem      No run long enough and the directory cannot grow.
 * @retval k_ra8_err_*           Backend or FAT error.
 *
 * @pre All pointers are non-NULL; @p m is a mounted FAT volume.
 * @pre @p loc names a directory that exists on @p m.
 * @post On success no other entry in @p loc carries `out->name83`.
 * @post On failure no directory entry has been written.
 *
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_reserve(const ra8_fs_mount_t* m,
                           const dir_loc_t*      loc,
                           const char*           leaf,
                           dir_insert_t*         out);

/**
 * @brief Write a reserved run: the long-name chain, then the 8.3 entry.
 *
 * @details Fills the slots ::priv_dir_reserve() set aside. The chain is written
 *          back to front -- the physically first slot carries the HIGHEST order
 *          number with the 0x40 "last logical group" flag -- because that is
 *          the order a scanner reassembles them in, and every slot carries the
 *          checksum of the 8.3 name that closes the run. The 8.3 entry itself
 *          is @p tmpl with its name and `DIR_NTRes` fields replaced, so the
 *          caller decides the attribute, the first cluster and the size, and
 *          `rename` can carry an existing entry across unchanged.
 *
 * @param[in]  m       Mounted FAT12/16/32 volume.
 * @param[in]  plan    Reservation produced by ::priv_dir_reserve().
 * @param[in]  tmpl    32-byte entry template; name and NTRes are overwritten.
 * @param[out] out_lba Sector the 8.3 entry landed in.
 * @param[out] out_off Byte offset of the 8.3 entry within that sector.
 *
 * @return Error code.
 * @retval k_ra8_ok         The entry (and any chain) is on disk.
 * @retval k_ra8_err_no_mem The reserved run ran past the end of the directory.
 * @retval k_ra8_err_*      Backend read/write error.
 *
 * @pre All pointers are non-NULL; @p tmpl addresses 32 readable bytes.
 * @pre @p plan came from a ::priv_dir_reserve() that returned `k_ra8_ok`, and
 *      the directory has not been modified since.
 * @post On success a scan of the directory finds the new name.
 * @post On a mid-run failure the directory may hold a partial chain, which a
 *       scanner skips because it is not closed by a matching 8.3 entry.
 *
 * @note A run that fits inside one sector -- which is every name of 15 groups
 *       or fewer that does not straddle a boundary, so nearly all of them --
 *       reaches the backend as a SINGLE write. The chain and the entry that
 *       closes it then appear together or not at all, which is the best any
 *       FAT directory update can do: the format has no journal, and a run
 *       spanning two sectors is two writes with a window between them.
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_commit(const ra8_fs_mount_t* m,
                          const dir_insert_t*   plan,
                          const uint8_t*        tmpl,
                          uint64_t*             out_lba,
                          uint32_t*             out_off);

/**
 * @brief Resolve one leaf name by 8.3 first and by long name second.
 *
 * @details The two-step every verb has to use, in one place so they cannot
 *          drift apart: pack the leaf to 8.3 and look that up, and if it is not
 *          representable -- or the lookup misses because the file is filed
 *          under a generated `~N` alias -- reassemble the directory's VFAT
 *          chains and match the long name instead.
 *
 * @param[in]  m         Mounted FAT12/16/32 volume.
 * @param[in]  loc       Directory to search.
 * @param[in]  leaf      Leaf component (no slashes).
 * @param[out] out_lba   Sector containing the matched 8.3 entry.
 * @param[out] out_off   Byte offset within that sector.
 * @param[out] out_entry 32 bytes of the matched entry.
 *
 * @return Error code.
 * @retval k_ra8_ok            Found; out parameters populated.
 * @retval k_ra8_err_not_found Neither lookup matched.
 * @retval k_ra8_err_*         Backend error.
 *
 * @pre All pointers are non-NULL.
 * @pre @p loc names a directory that exists on @p m.
 * @post On success the out parameters identify one on-disk 8.3 entry.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_lookup_any(const ra8_fs_mount_t* m,
                              const dir_loc_t*      loc,
                              const char*           leaf,
                              uint64_t*             out_lba,
                              uint32_t*             out_off,
                              uint8_t               out_entry[k_ra8_fs_dir_entry_bytes]);

/**
 * @brief Delete a directory entry together with its long-name chain.
 *
 * @details Marking only the 8.3 entry 0xE5 -- which is all `unlink` used to do
 *          -- leaves the attr-0x0F slots in front of it on disk, pointing at a
 *          checksum nothing answers to any more. This driver's own reader skips
 *          them, but `fsck.fat` and `chkdsk` report them as orphaned long-name
 *          entries, so the chain is taken away with the entry it belonged to.
 *          Only slots that are contiguous with the entry AND carry the checksum
 *          of @p name83 are touched, so an unrelated chain left in front of it
 *          by an earlier deletion is not swept up as well.
 *
 * @param[in] m      Mounted FAT12/16/32 volume.
 * @param[in] loc    Directory holding the entry.
 * @param[in] lba    Sector of the 8.3 entry.
 * @param[in] off    Byte offset of the 8.3 entry within that sector.
 * @param[in] name83 The entry's packed 11-byte name (its checksum source).
 *
 * @return Error code.
 * @retval k_ra8_ok            Entry and chain marked deleted.
 * @retval k_ra8_err_not_found The walk did not reach (@p lba, @p off).
 * @retval k_ra8_err_*         Backend read/write error.
 *
 * @pre All pointers are non-NULL; (@p lba, @p off) name an entry in @p loc.
 * @pre @p name83 is the name field of the entry at (@p lba, @p off).
 * @post On success no long-name slot in @p loc references that entry.
 * @post The entry's cluster chain is NOT touched; freeing it is the caller's job.
 *
 * @note Not thread-safe; callers serialise directory access.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_dir_erase_chain(const ra8_fs_mount_t* m,
                               const dir_loc_t*      loc,
                               uint64_t              lba,
                               uint32_t              off,
                               const uint8_t*        name83);
