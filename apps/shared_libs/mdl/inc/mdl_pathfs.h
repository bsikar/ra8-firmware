/**
 * @file mdl_pathfs.h
 * @brief Guarded filesystem directory joins for the media downloader.
 *
 * @details
 * The download orchestration composes a chapter or combined output directory
 * under a series directory from an untrusted, scraped leaf name. This module
 * holds the one filesystem-touching join that couples the lexical
 * ::mdl_path_join guard (a `..`, separator-bearing, or absolute segment can
 * never compose a path) with the injected filesystem's confined-root and
 * no-symlink-walk guarantees. It is kept out of the CLI's translation unit so
 * the pure lexical predicates in mdl_sanitize stay filesystem-free and this
 * filesystem policy has exactly one home.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "mdl_storage.h"

/**
 * @brief Join `seg` under `parent_abs`, create it, and verify it stays inside.
 *
 * @details
 * The single guarded directory join every `series_dir`/`chapter_dir` path
 * routes through. ::mdl_path_join refuses a traversal, separator-bearing, or
 * over-long `seg` before any `mkdir`, so a scraped `..` can never name a
 * directory. The filesystem adapter then confines the path beneath its bound
 * root and refuses symbolic-link traversal. Any stat or create failure returns
 * false without printing; the composition layer owns diagnostics.
 *
 * @param[in,out] storage Initialized portable filesystem binding.
 * @param[in]  parent_abs Canonical portable parent directory.
 * @param[in]  seg        Sanitised child segment (a leaf or a slug-derived name).
 * @param[out] out        Buffer receiving the joined directory path.
 * @param[in]  cap        Capacity of @p out in bytes.
 *
 * @return Whether @p out names a directory contained under @p parent_abs.
 * @retval true  The child directory exists beneath @p parent_abs.
 * @retval false Invalid input, unsafe/over-long @p seg, missing parent, or a
 *               backend stat/create failure.
 *
 * @pre @p parent_abs is an existing canonical directory below the bound root.
 * @pre @p seg and @p out are non-NULL; @p seg is NUL-terminated.
 * @post On true, @p out names an existing directory under @p parent_abs.
 * @post On false, no path outside @p parent_abs is created or used.
 *
 * @note Not thread-safe because one storage binding owns shared adapter state.
 * @see mdl_path_join  The lexical join that rejects the traversal segment.
 * @since 0.1.0
 */
bool mdl_join_dir_under(mdl_storage_t* storage,
                        const char*    parent_abs,
                        const char*    seg,
                        char*          out,
                        size_t         cap);
