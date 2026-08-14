/**
 * @file mdl_pack.h
 * @brief Archive packaging of downloaded page folders for the media_dl CLI.
 *
 * @details
 * The download loop only puts verified page bytes on disk; turning a chapter or
 * a combined page folder into a reader-openable container (or per-page JOF
 * siblings) is this module's job. It is kept separate from the run
 * orchestration so the "when do we package" policy -- in particular the refusal
 * to emit a complete-looking archive from an incomplete run -- lives in one
 * place, and so `main.c` stays a thin dispatcher. Every path is composed through
 * the guarded ::mdl_path_join, so an untrusted leaf can never escape the series
 * directory.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "mdl_export.h"
#include "mdl_fetch.h"

/**
 * @brief Package one downloaded chapter folder into @p format.
 *
 * @details
 * Composes `<series_dir>/<chap_id>` as the source page folder and writes the
 * container beside it (or, for a directory-output format such as JOF, per-page
 * siblings inside the folder). A path that would escape the series directory,
 * or an export error, is reported and counted as one failure.
 *
 * @param[in] format     Output container/format (never ::k_mdl_fmt_loose here).
 * @param[in] series_dir Absolute, resolved series directory.
 * @param[in] chap_id    Sanitised chapter identifier (the page folder leaf).
 *
 * @return The number of export failures (0 on success, 1 on any failure).
 * @retval 0U The chapter was packaged.
 * @retval 1U A path was rejected or the export failed (diagnostic printed).
 *
 * @pre @p series_dir and @p chap_id are non-NULL and NUL-terminated.
 * @pre @p series_dir names an existing directory.
 * @post On success a container (or JOF siblings) exists for the chapter.
 * @post On failure a diagnostic naming the chapter was written to stderr.
 *
 * @note Not thread-safe (shared cwd during path resolution).
 * @since 0.1.0
 */
size_t mdl_pack_one(mdl_format_t            format,
                    const char*             series_dir,
                    const char*             chap_id,
                    mdl_export_workspace_t* ws);

/**
 * @brief Package one downloaded chapter folder into @p format with rich metadata.
 */
size_t mdl_pack_one_meta(mdl_format_t             format,
                         const char*              series_dir,
                         const char*              chap_id,
                         const mdl_export_meta_t* meta,
                         mdl_export_workspace_t*  ws);

size_t mdl_pack_combined(mdl_format_t             format,
                         bool                     allow_incomplete,
                         const char*              series_dir,
                         const char*              combined_rel,
                         const mdl_fetch_stats_t* stats,
                         mdl_export_workspace_t*  ws);

/**
 * @brief Package a combined page folder into @p format with rich metadata.
 */
size_t mdl_pack_combined_meta(mdl_format_t             format,
                              bool                     allow_incomplete,
                              const char*              series_dir,
                              const char*              combined_rel,
                              const mdl_fetch_stats_t* stats,
                              const mdl_export_meta_t* meta,
                              mdl_export_workspace_t*  ws);
