/**
 * @file mdl_library.h
 * @brief Library-wide operations over a directory of tracked series.
 *
 * @details
 * A "library" is the output root (`--out`): every immediate subdirectory that
 * carries a `.mdl_state` file is a tracked series. This module provides the two
 * filesystem primitives the CLI's library commands (`--list`, `--update-all`,
 * `--remove`) are built from -- enumerating those series, and deleting one
 * series' directory tree -- kept out of `main.c` so the traversal has one home
 * and `main` stays a thin dispatcher.
 *
 * Enumeration is a visitor: the caller supplies a callback and an opaque
 * context, so the same walk drives listing (load + print each series' coverage),
 * bulk update (re-run an incremental fetch per series), and any future
 * per-series command, without this module depending on the download or
 * state-printing machinery.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "ra8_err.h"

/**
 * @brief Per-series visitor callback for ::mdl_library_for_each.
 *
 * @param[in] series_dir Absolute path of one tracked series directory.
 * @param[in] state_path Absolute path of that series' `.mdl_state` file.
 * @param[in] ctx        Opaque context the caller passed to the walk.
 *
 * @return ::k_ra8_ok to continue, any other code to stop the walk early.
 * @retval k_ra8_ok Continue enumerating the remaining series.
 *
 * @pre @p series_dir and @p state_path are non-NULL and NUL-terminated.
 * @pre The callback does not delete the directory it is currently visiting.
 * @post The callback may read/print but must leave enumeration state intact.
 *
 * @note Not thread-safe: invoked serially during the walk.
 * @since 0.1.0
 */
typedef ra8_err_t (*mdl_library_fn)(const char* series_dir, const char* state_path, void* ctx);

/**
 * @brief Visit every tracked series under a library root.
 *
 * @details
 * Scans the immediate subdirectories of @p out_dir and, for each that contains
 * a `.mdl_state` file, invokes @p cb with that series' directory and state-file
 * paths. Enumeration stops early if @p cb returns anything other than
 * ::k_ra8_ok. A missing @p out_dir is not an error -- an empty library visits
 * nothing.
 *
 * @param[in]     out_dir Library root directory (never NULL).
 * @param[in]     cb      Per-series callback (never NULL).
 * @param[in,out] ctx     Opaque context forwarded to @p cb, or NULL.
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok              Every tracked series was visited (or none exist).
 * @retval k_ra8_err_invalid_arg @p out_dir or @p cb was NULL.
 * @retval other                 The code @p cb returned to stop the walk.
 *
 * @pre @p out_dir and @p cb are non-NULL.
 * @pre @p cb tolerates being called zero times (an empty library).
 * @post Directory entries are visited at most once each.
 * @post No directory is modified by the walk itself.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t mdl_library_for_each(const char* out_dir, mdl_library_fn cb, void* ctx);

/**
 * @brief Delete a directory tree (a whole series) depth-first.
 *
 * @details
 * Removes @p dir and everything under it, files before their parent
 * directories, so a tracked series (its pages, archives and state file) is gone
 * in one call. Symlinks are removed as links, never followed, so a series
 * directory cannot be used to delete outside itself.
 *
 * @param[in] dir Directory tree to remove (never NULL).
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok              The tree was removed (or already absent).
 * @retval k_ra8_err_invalid_arg @p dir was NULL or empty.
 * @retval k_ra8_fail            A file or directory could not be removed.
 *
 * @pre @p dir is non-NULL and NUL-terminated.
 * @pre The caller has resolved @p dir to the intended series directory.
 * @post On ::k_ra8_ok no path under @p dir remains.
 * @post Nothing outside @p dir is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t mdl_library_remove_tree(const char* dir);
