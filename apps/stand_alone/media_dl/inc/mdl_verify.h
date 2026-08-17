/**
 * @file mdl_verify.h
 * @brief Bounded, no-heap structural validation of media_dl artifacts.
 * @details Declares path-based format inference and structural validation that
 *          reports artifact contents while using caller-owned scratch storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "mdl_export.h"
#include "mdl_storage.h"
#include "ra8_err.h"

typedef struct {
  ra8_mdl_format_t format;           /**< Format selected for validation.          */
  size_t           page_count;       /**< Image members found in the artifact.     */
  size_t           member_count;     /**< Total members reported by the container. */
  bool             metadata_present; /**< Required format metadata was present.    */
} mdl_verify_report_t;

/**
 * @brief Infer an artifact format from its complete path suffix
 *
 * @details Matches the complete, case-insensitive suffix so multi-dot formats
 *          such as `.cbt.gz` are not misclassified by their last extension.
 *
 * @param[in]  path       NUL-terminated artifact path to classify.
 * @param[out] out_format Receives the recognized format or invalid sentinel.
 * @return Classification status.
 * @retval k_ra8_ok A supported artifact suffix was recognized.
 * @retval k_ra8_err_invalid_arg Either pointer is NULL.
 * @retval k_ra8_err_not_supported The suffix is not supported.
 * @pre @p path, when non-NULL, is NUL-terminated.
 * @pre @p out_format, when non-NULL, addresses writable storage.
 * @post Success stores a verifiable format in @p out_format.
 * @post Failure for an unknown suffix stores ::k_ra8_mdl_format_invalid.
 * @note Thread-safe: reads arguments and writes caller-owned storage only.
 * @since 0.1.0
 */
ra8_err_t mdl_format_from_path(const char* path, ra8_mdl_format_t* out_format);

/**
 * @brief Report whether a format has an in-process structural validator
 *
 * @details Distinguishes advertised native formats from reserved enum values
 *          whose readers or writers are not yet exposed by this host tool.
 * @param[in] format Format enum value to query.
 * @return Whether ::mdl_verify_file implements the format.
 * @retval true The format can be structurally validated in process.
 * @retval false The format is invalid, loose, or currently unsupported.
 * @pre @p format is represented by ::ra8_mdl_format_t.
 * @pre The caller does not infer writer availability from this predicate.
 * @post No caller or global state is modified.
 * @post Repeated calls with the same value return the same result.
 * @note Thread-safe: this is a pure classifier.
 * @since 0.1.0
 */
bool mdl_format_is_verifiable(ra8_mdl_format_t format);

/**
 * @brief Validate an artifact through a borrowed open filesystem handle
 *
 * @details Dispatches to the same ZIP, tar, gzip, JOF, or strict RBKC validator used by
 *          ::mdl_verify_file after seeking @p file to offset zero. This entry
 *          point lets an exporter validate a staged transaction before commit
 *          without publishing or reopening the stage by name. The handle is
 *          borrowed: this function never closes it, and its final offset is
 *          unspecified. The size is an immutable caller-supplied snapshot.
 * @param[in,out] storage Injected storage buffers used by streaming readers.
 * @param[in]     format Expected artifact format.
 * @param[in,out] file Borrowed readable and seekable open handle.
 * @param[in]     size_bytes Stable artifact extent in bytes.
 * @param[in,out] workspace Caller-owned bounded validation workspace.
 * @param[out]    report Structural counts populated only on success.
 * @return Validation, argument, or stream status.
 * @retval k_ra8_ok The staged artifact is structurally valid for @p format.
 * @retval k_ra8_err_invalid_arg A pointer, workspace, or format is invalid.
 * @retval k_ra8_err_invalid_size The caller workspace is too small.
 * @retval k_ra8_err_validation_failed Container structure or metadata is bad.
 * @retval k_ra8_err_not_supported The reserved format has no validator.
 * @retval other A seek or read failure was propagated.
 * @pre @p file remains exclusively borrowed for the complete call.
 * @pre @p size_bytes is the stable size of the staged artifact.
 * @post @p file remains open and owned by the caller.
 * @post On failure @p report retains its entry value.
 * @post `workspace->high_water` describes this validation attempt.
 * @note Thread-safe across distinct handles, workspaces, buffers, and reports.
 * @since 0.1.0
 */
ra8_err_t mdl_verify_open_file(mdl_storage_t*          storage,
                               ra8_mdl_format_t        format,
                               fw_fs_file_t*           file,
                               uint64_t                size_bytes,
                               mdl_export_workspace_t* workspace,
                               mdl_verify_report_t*    report);

/**
 * @brief Validate a completed artifact using caller-owned scratch only
 *
 * @details Dispatches to the format-specific ZIP, tar, gzip, JOF, or strict RBKC reader,
 *          rejects unsafe member paths and missing required metadata, and
 *          resets the workspace so `high_water` describes this call alone.
 *          ZIP and JOF use positioned reads; TAR and gzip are streamed through
 *          bounded chunks, so no complete compressed or decoded archive is
 *          retained. Every opened stream is closed before return.
 * @param[in,out] storage Injected filesystem and exclusive file workspace.
 * @param[in]     format    Expected artifact format.
 * @param[in]     path   NUL-terminated path to the completed artifact.
 * @param[in,out] workspace     Caller-owned bounded validation workspace.
 * @param[out]    report Structural counts populated only on success.
 * @return Validation or argument status.
 * @retval k_ra8_ok The artifact is structurally valid for @p format.
 * @retval k_ra8_err_invalid_arg A pointer, workspace, or format is invalid.
 * @retval k_ra8_err_invalid_size The caller workspace is too small.
 * @retval k_ra8_err_validation_failed Container structure or metadata is bad.
 * @retval k_ra8_err_not_supported The reserved format has no validator.
 * @retval other A filesystem open/read/seek/size/close failure was propagated.
 * @pre @p path is canonical, NUL-terminated, and names a stable completed file.
 * @pre @p storage, @p workspace, and @p report are exclusive to this call.
 * @post `workspace->used` and `workspace->high_water` describe this validation attempt.
 * @post On success @p report contains format, member, page, and metadata data.
 * @post On failure @p report retains its entry value.
 * @note Thread-safe across calls that use distinct workspaces and reports.
 * @since 0.1.0
 */
ra8_err_t mdl_verify_file(mdl_storage_t*          storage,
                          ra8_mdl_format_t        format,
                          const char*             path,
                          mdl_export_workspace_t* workspace,
                          mdl_verify_report_t*    report);
