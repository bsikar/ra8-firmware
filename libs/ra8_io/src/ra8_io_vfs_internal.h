/**
 * @file ra8_io_vfs_internal.h
 * @brief Module-private mount-slot layout and path resolvers for the ra8_io VFS.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The named-mount dispatcher is implemented by two translation units. The mount
 * table, its fixed storage, and the open-stream facade live in `ra8_io_vfs.c`;
 * the path-namespace operations (removal, rename, metadata, listing, directory
 * cursors, and directory creation) live in `ra8_io_vfs_namespace.c`. Both need
 * the fixed slot layout and the bounded name/path resolvers declared here.
 *
 * The mount and open-file tables themselves stay private to `ra8_io_vfs.c`: the
 * namespace unit reaches a mount only through ::priv_ra8_io_vfs_find and
 * ::priv_ra8_io_vfs_resolve, so there is exactly one owner of that state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"

/** @brief One fixed named-mount slot. */
typedef struct {
  /** NUL-terminated mount name. */
  char name[(uint32_t)k_ra8_io_vfs_name_max];
  /** Selected format descriptor. */
  const ra8_io_fsfmt_t* format;
  /** Format-private mounted context. */
  void* mount_ctx;
  /** VFS must invoke format unmount. */
  bool owned;
  /** Context is a native ra8_fs mount. */
  bool native;
  /** Slot is occupied. */
  bool in_use;
} vfs_slot_t;

/**
 * @brief Test-only entry point for `internal_vfs_init_slot()`.
 * @details Exists so the reset-and-conditionally-unmount decision can be
 *          driven directly against a caller-built ::vfs_slot_t, without
 *          routing through ::ra8_io_vfs_mount / ::ra8_io_vfs_mount_auto to
 *          reach every `in_use` / `owned` combination.
 * @param[in,out] slot Mount slot to tear down and reset.
 * @return The owned unmount's status, or k_ra8_ok when no unmount was needed.
 * @retval k_ra8_ok The slot was already idle, borrowed, or unmounted cleanly.
 * @retval other The bound format's unmount reported a failure.
 * @pre @p slot is non-NULL.
 * @pre When `slot->in_use && slot->owned`, `slot->format->ops->unmount` is
 *      non-NULL.
 * @post @p slot is zero-initialized.
 * @post An owned mount's format unmount is invoked exactly once.
 * @note Not thread-safe; single-threaded host test use only.
 * @par MC/DC:
 * Exposes `internal_vfs_init_slot()`'s `slot->in_use && slot->owned` decision
 * (`libs/ra8_io/src/ra8_io_vfs.c@internal_vfs_init_slot`) for direct N+1
 * vectors; see `internal_test_vfs_init_slot_mcdc` in
 * `tests/test_ra8_io_vfs.c`.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER ra8_err_t ra8_io_vfs_init_slot_test(vfs_slot_t* slot);

/**
 * @brief Compare two mount names within the fixed bound.
 * @details Stops at the first mismatch or shared terminator.
 * @param[in] a First name.
 * @param[in] b Second name.
 * @return bool Equality result.
 * @retval true Names are equal.
 * @retval false Names differ.
 * @pre @p a and @p b are non-NULL.
 * @pre Both names terminate within the VFS name bound.
 * @post No state is modified.
 * @post At most the fixed name bound is inspected.
 * @note Pure bounded comparison.
 * @since 0.1.0
 */
RA8_PRIV bool priv_ra8_io_vfs_streq(const char* a, const char* b);

/**
 * @brief Find an occupied mount slot by name and optionally report its index.
 * @details Searches the fixed mount table in ascending slot order.
 * @param[in] name Valid mount name.
 * @param[out] out_index Optional slot-index destination.
 * @return vfs_slot_t* Matching slot or NULL.
 * @retval non-NULL Occupied matching slot.
 * @retval NULL No slot matched.
 * @pre @p name is non-NULL and bounded.
 * @pre @p out_index is NULL or writable.
 * @post On a match the optional index identifies the returned slot.
 * @post Mount-table state is unchanged.
 * @note Not thread-safe with concurrent mount mutation.
 * @since 0.1.0
 */
RA8_PRIV vfs_slot_t* priv_ra8_io_vfs_find(const char* name, uint8_t* out_index);

/**
 * @brief Split `"name:sub"` into a bounded name and sub-path pointer.
 * @details Finds the colon within the name bound and copies only the prefix.
 * @param[in] path Qualified VFS path.
 * @param[out] out_name Fixed mount-name buffer.
 * @param[out] out_sub Receives a pointer after the colon.
 * @return ra8_err_t Split result.
 * @retval k_ra8_ok Path split.
 * @retval k_ra8_err_invalid_arg No bounded colon exists.
 * @pre All pointers are non-NULL.
 * @pre @p out_name has VFS-name capacity.
 * @post On success both outputs are assigned.
 * @post No mount-table state is modified.
 * @note The sub-path aliases the caller's input string.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_io_vfs_split(const char* path, char* out_name, const char** out_sub);

/**
 * @brief Resolve `"name:sub"` to a mount slot and sub-path.
 * @details Splits the prefix then looks up the named fixed-table slot.
 * @param[in] path Qualified VFS path.
 * @param[out] out_slot Receives the mounted slot.
 * @param[out] out_index Optional slot-index destination.
 * @param[out] out_sub Receives the volume-relative sub-path.
 * @return ra8_err_t Resolution result.
 * @retval k_ra8_ok Mount resolved.
 * @retval k_ra8_err_not_found Name is not mounted.
 * @retval k_ra8_err_invalid_arg Path cannot be split.
 * @pre @p path, @p out_slot, and @p out_sub are non-NULL.
 * @pre @p out_index is NULL or writable.
 * @post On success the slot and sub-path outputs are assigned.
 * @post Mount-table state is unchanged.
 * @note Not thread-safe with concurrent unmount.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_io_vfs_resolve(const char*  path,
                                           vfs_slot_t** out_slot,
                                           uint8_t*     out_index,
                                           const char** out_sub);
