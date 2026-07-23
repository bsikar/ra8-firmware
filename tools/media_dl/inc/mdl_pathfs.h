/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_pathfs.h
 * @brief Guarded filesystem directory joins for the media downloader.
 *
 * @details
 * The download orchestration composes a chapter or combined output directory
 * under a series directory from an untrusted, scraped leaf name. This module
 * holds the one filesystem-touching join that couples the lexical
 * ::mdl_path_join guard (a `..`, separator-bearing, or absolute segment can
 * never compose a path) with a post-`mkdir` `realpath` containment check (a
 * symlinked component can never redirect the created directory outside the
 * series tree). It is kept out of the CLI's translation unit so the pure
 * lexical predicates in mdl_sanitize stay filesystem-free and this filesystem
 * policy has exactly one home.
 */
#pragma once

#include <stddef.h>

/**
 * @brief Join `seg` under `parent_abs`, create it, and verify it stays inside.
 *
 * @details
 * The single guarded directory join every `series_dir`/`chapter_dir` path
 * routes through. ::mdl_path_join refuses a traversal, separator-bearing, or
 * over-long `seg` before any `mkdir`, so a scraped `..` can never name a
 * directory; the post-`mkdir` `realpath` containment check is the runtime
 * belt-and-suspenders that also defeats a symlinked component. Any failure
 * prints a diagnostic and returns false so the caller aborts loudly rather
 * than operating on the wrong directory.
 *
 * @param[in]  parent_abs Absolute, resolved parent directory.
 * @param[in]  seg        Sanitised child segment (a leaf or a slug-derived name).
 * @param[out] out        Buffer receiving the joined directory path.
 * @param[in]  cap        Capacity of @p out in bytes.
 *
 * @return Whether @p out names a created directory contained under @p parent_abs.
 * @retval true  The directory exists and resolves inside @p parent_abs.
 * @retval false Unsafe/over-long @p seg, or the resolved path escaped the parent.
 *
 * @pre @p parent_abs is an existing absolute directory.
 * @pre @p seg and @p out are non-NULL; @p seg is NUL-terminated.
 * @post On true, @p out names an existing directory under @p parent_abs.
 * @post On false, no path outside @p parent_abs is created or used.
 *
 * @note Not thread-safe (shared cwd for the realpath resolution).
 * @see mdl_path_join  The lexical join that rejects the traversal segment.
 * @see mdl_path_contained  The lexical containment predicate paired with realpath.
 * @since 0.1.0
 */
bool mdl_join_dir_under(const char* parent_abs, const char* seg, char* out, size_t cap);
