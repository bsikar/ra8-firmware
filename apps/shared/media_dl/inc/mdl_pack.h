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
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format     Output container/format (never ::k_ra8_mdl_format_loose here).
 * @param[in] series_dir Absolute, resolved series directory.
 * @param[in] chap_id    Sanitised chapter identifier (the page folder leaf).
 * @param[in,out] ws     Caller-owned bounded exporter workspace.
 * @param[in,out] output Borrowed stream receiving successful package paths.
 * @param[in,out] diagnostic Borrowed stream receiving failure diagnostics.
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
size_t mdl_pack_one(mdl_storage_t*          storage,
                    ra8_mdl_format_t        format,
                    const char*             series_dir,
                    const char*             chap_id,
                    mdl_export_workspace_t* ws,
                    ra8_io_stream_t*        output,
                    ra8_io_stream_t*        diagnostic);

/**
 * @brief Package one downloaded chapter folder into @p format with rich metadata.
 *
 * @details Uses @p meta instead of auto-loading the chapter metadata files,
 *          while retaining guarded path composition and atomic export behavior.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output container format.
 * @param[in] series_dir Absolute, resolved series directory.
 * @param[in] chap_id Sanitized chapter directory leaf.
 * @param[in] meta Metadata to embed, or NULL to auto-load it.
 * @param[in,out] ws Caller-owned bounded exporter workspace.
 * @param[in,out] output Borrowed stream receiving successful package paths.
 * @param[in,out] diagnostic Borrowed stream receiving failure diagnostics.
 * @return The number of failures from this one packaging operation.
 * @retval 0U Packaging succeeded.
 * @retval 1U Path validation or export failed.
 * @pre @p series_dir and @p chap_id are non-NULL and NUL-terminated.
 * @pre @p ws owns writable arena storage for the duration of the call.
 * @post Success creates the selected container or per-page artifacts.
 * @post Failure is counted once and diagnosed without replacing a good output.
 * @note Not thread-safe when callers share a workspace or output path.
 * @since 0.1.0
 */
size_t mdl_pack_one_meta(mdl_storage_t*           storage,
                         ra8_mdl_format_t         format,
                         const char*              series_dir,
                         const char*              chap_id,
                         const mdl_export_meta_t* meta,
                         mdl_export_workspace_t*  ws,
                         ra8_io_stream_t*         output,
                         ra8_io_stream_t*         diagnostic);

/**
 * @brief Package a combined page directory when completion policy permits
 *
 * @details Delegates to the metadata-aware variant and auto-loads metadata.
 *          Incomplete runs are skipped unless explicitly allowed, in which
 *          case the output filename is marked `INCOMPLETE`.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output container format.
 * @param[in] allow_incomplete Whether a partial run may be packaged.
 * @param[in] series_dir Absolute, resolved series directory.
 * @param[in] combined_rel Sanitized combined-directory leaf.
 * @param[in] stats Completed run statistics controlling the policy decision.
 * @param[in,out] ws Caller-owned bounded exporter workspace.
 * @param[in,out] output Borrowed stream receiving successful package paths.
 * @param[in,out] diagnostic Borrowed stream receiving policy and failure diagnostics.
 * @return The number of packaging failures.
 * @retval 0U Nothing required packaging, policy skipped it, or export succeeded.
 * @retval 1U Path validation or export failed.
 * @pre All pointer arguments are non-NULL and NUL strings are terminated.
 * @pre @p ws is exclusive to this call and owns writable arena bytes.
 * @post A disallowed incomplete run creates no combined archive.
 * @post An allowed incomplete export is visibly marked in its filename.
 * @note Not thread-safe when callers share a workspace or output path.
 * @since 0.1.0
 */
size_t mdl_pack_combined(mdl_storage_t*           storage,
                         ra8_mdl_format_t         format,
                         bool                     allow_incomplete,
                         const char*              series_dir,
                         const char*              combined_rel,
                         const mdl_fetch_stats_t* stats,
                         mdl_export_workspace_t*  ws,
                         ra8_io_stream_t*         output,
                         ra8_io_stream_t*         diagnostic);

/**
 * @brief Package a combined page folder into @p format with rich metadata.
 *
 * @details Applies the same incomplete-run policy as ::mdl_pack_combined but
 *          embeds caller-supplied metadata instead of auto-loading it.
 * @param[in,out] storage Injected portable file reader.
 * @param[in] format Output container format.
 * @param[in] allow_incomplete Whether a partial run may be packaged.
 * @param[in] series_dir Absolute, resolved series directory.
 * @param[in] combined_rel Sanitized combined-directory leaf.
 * @param[in] stats Completed run statistics controlling the policy decision.
 * @param[in] meta Metadata to embed, or NULL to auto-load it.
 * @param[in,out] ws Caller-owned bounded exporter workspace.
 * @param[in,out] output Borrowed stream receiving successful package paths.
 * @param[in,out] diagnostic Borrowed stream receiving policy and failure diagnostics.
 * @return The number of packaging failures.
 * @retval 0U Nothing required packaging, policy skipped it, or export succeeded.
 * @retval 1U Path validation or export failed.
 * @pre All pointer arguments are non-NULL except optional @p meta.
 * @pre @p ws is exclusive to this call and owns writable arena bytes.
 * @post A disallowed incomplete run creates no combined archive.
 * @post Successful partial output carries the `INCOMPLETE` marker.
 * @note Not thread-safe when callers share a workspace or output path.
 * @since 0.1.0
 */
size_t mdl_pack_combined_meta(mdl_storage_t*           storage,
                              ra8_mdl_format_t         format,
                              bool                     allow_incomplete,
                              const char*              series_dir,
                              const char*              combined_rel,
                              const mdl_fetch_stats_t* stats,
                              const mdl_export_meta_t* meta,
                              mdl_export_workspace_t*  ws,
                              ra8_io_stream_t*         output,
                              ra8_io_stream_t*         diagnostic);
