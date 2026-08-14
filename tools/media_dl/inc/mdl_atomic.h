/**
 * @file mdl_atomic.h
 * @brief Write-to-temp-then-rename so a failed re-fetch cannot destroy good data.
 *
 * @details
 * Every writer in this tool used to open its destination directly and
 * `remove()` it when anything went wrong. That is a data-loss bug, not merely
 * untidy: re-fetching a page, re-exporting a chapter, or re-running any of it
 * TRUNCATES the previously-good file the moment `fopen(path, "wb")` succeeds,
 * and then deletes the remains when the network request fails. The user loses a
 * file they already had because a server returned 503.
 *
 * These three calls are the fix and the whole of it. A writer builds a sibling
 * temp path, writes the new bytes there, and renames it over the destination
 * only once a COMPLETE good copy exists. `rename()` is atomic within a
 * filesystem, so a reader sees either the old file or the new one and never a
 * half-written one -- and on any failure path the destination is never touched
 * at all.
 *
 * The temp name is a `.mdl-tmp-<unique>-` PREFIX on the leaf, not a suffix,
 * deliberately:
 *
 *   * It keeps the destination's extension intact, which matters because two
 *     export paths shell out to tools that sniff it -- `rar` appends `.rar` to
 *     an argument it does not recognise as already having one.
 *   * A leading dot is what `list_pages()` and the AppleDouble filter already
 *     skip, so a temp file living briefly in a chapter directory cannot be
 *     packaged as a page.
 *   * It is a sibling, so the `rename()` stays within one filesystem. A temp
 *     in `/tmp` would cross a mount point and fail with `EXDEV`.
 *
 * The unique token is created atomically with `mkstemps`, so concurrent runs
 * never share a partial file and an abandoned file from a reused PID cannot be
 * truncated. The original extension is retained for tools that inspect it.
 *
 * @see mdl_state_save() The pattern these calls generalise; it has always been
 *      correct and is what the rest of the tool now matches.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

/**
 * @brief Build the sibling temp path a writer should produce its output at.
 *
 * @details
 * Atomically creates `<dir>/.mdl-tmp-<unique>-<leaf>` for a `<dir>/<leaf>`
 * destination, or `.mdl-tmp-<unique>-<leaf>` for a bare leaf. The reserved
 * empty regular file is in the destination's own directory so
 * ::mdl_atomic_commit can rename it without crossing a filesystem boundary.
 *
 * @param[in]  final_path Destination the caller ultimately wants (never NULL).
 * @param[out] out        Buffer receiving the temp path (never NULL).
 * @param[in]  cap        Capacity of @p out in bytes.
 *
 * @return Whether a temp path was built.
 * @retval true  @p out names a newly-created, unique empty regular file.
 * @retval false An argument was invalid, the path did not fit, or unique file
 *               creation failed.
 *
 * @pre @p final_path and @p out are non-NULL; @p final_path is NUL-terminated.
 * @pre @p cap is large enough for the destination plus the ~24-byte prefix.
 * @post On true, @p out is NUL-terminated, differs from @p final_path, and
 *       names a caller-owned empty file that must be committed or aborted.
 * @post On false, no temp remains and the caller must not write anything.
 *
 * @note Thread-safe: `mkstemps` reserves a distinct file for every caller.
 * @warning A false return must ABORT the write. Falling back to writing
 *          @p final_path directly reintroduces exactly the data-loss bug this
 *          file exists to remove.
 *
 * @par Example:
 * @code
 * char tmp[PATH_MAX];
 * if (!mdl_atomic_tmp_path(out_path, tmp, sizeof(tmp))) {
 *   return k_ra8_fail;
 * }
 * FILE* fp = fopen(tmp, "wb");
 * ...
 * if (failed) {
 *   mdl_atomic_abort(tmp);
 *   return k_ra8_fail;
 * }
 * return mdl_atomic_commit(tmp, out_path) ? k_ra8_ok : k_ra8_fail;
 * @endcode
 *
 * @see mdl_atomic_commit
 * @see mdl_atomic_abort
 * @since 0.1.0
 */
bool mdl_atomic_tmp_path(const char* final_path, char* out, size_t cap);

/**
 * @brief Rename a completed temp file over its destination.
 *
 * @details
 * The commit half. Call this ONLY once the temp file holds a complete, good
 * copy -- everything before this point is reversible, and everything after it
 * has replaced the user's data. On a failed rename the temp file is removed so
 * no debris is left behind; the destination is untouched either way, because
 * `rename()` that fails does not modify its target. Before the rename this
 * function verifies the temp is a regular non-symlink file and fsyncs it;
 * after the rename it fsyncs the containing directory.
 *
 * @param[in] tmp_path   Temp file from ::mdl_atomic_tmp_path (never NULL).
 * @param[in] final_path Destination to replace (never NULL).
 *
 * @return Whether the destination now holds the new bytes.
 * @retval true  The bytes and directory entry are durably committed; @p tmp_path no longer exists.
 * @retval false Publication or durability failed. When failure precedes the
 *               rename, @p final_path is unchanged; when only the directory
 *               fsync fails, the complete new file is visible but crash
 *               durability could not be proven.
 *
 * @pre @p tmp_path and @p final_path are non-NULL and NUL-terminated.
 * @pre @p tmp_path names a COMPLETE output -- every stream is closed and every
 *      write has been checked.
 * @post No file named @p tmp_path remains, on either outcome.
 * @post On false before rename, @p final_path is byte-for-byte unchanged. If
 *       the directory fsync itself fails after a successful rename, false
 *       reports that crash durability could not be proven although the new
 *       complete file is visible.
 *
 * @note Thread-safe: touches only the two named paths.
 * @see mdl_atomic_tmp_path
 * @since 0.1.0
 */
bool mdl_atomic_commit(const char* tmp_path, const char* final_path);

/**
 * @brief Discard a failed attempt, leaving the destination untouched.
 *
 * @details
 * The abort half, for every failure path between ::mdl_atomic_tmp_path and
 * ::mdl_atomic_commit. Removing the temp is a courtesy -- the correctness
 * property is that the destination was never opened -- so a removal that itself
 * fails is not reported: there is no useful recovery, and the caller is already
 * returning an error for the real problem.
 *
 * @param[in] tmp_path Temp file to discard, or NULL (then a no-op).
 *
 * @return Nothing.
 *
 * @pre @p tmp_path is NULL or a NUL-terminated path from
 *      ::mdl_atomic_tmp_path.
 * @pre Every stream that was writing @p tmp_path is already closed.
 * @post Removal of @p tmp_path has been attempted; cleanup is best effort.
 * @post The corresponding destination has not been modified.
 *
 * @note Thread-safe: touches only the named path.
 * @see mdl_atomic_commit
 * @since 0.1.0
 */
void mdl_atomic_abort(const char* tmp_path);
