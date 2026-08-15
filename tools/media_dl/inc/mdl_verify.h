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
#include "ra8_err.h"

typedef struct {
  mdl_format_t format;           /**< Format selected for validation.          */
  size_t       page_count;       /**< Image members found in the artifact.     */
  size_t       member_count;     /**< Total members reported by the container. */
  bool         metadata_present; /**< Required format metadata was present.    */
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
 * @post Failure for an unknown suffix stores ::k_mdl_fmt_invalid.
 * @note Thread-safe: reads arguments and writes caller-owned storage only.
 * @since 0.1.0
 */
ra8_err_t mdl_format_from_path(const char* path, mdl_format_t* out_format);

/**
 * @brief Report whether a format has an in-process structural validator
 *
 * @details Distinguishes advertised native formats from reserved enum values
 *          whose readers or writers are not yet exposed by this host tool.
 * @param[in] format Format enum value to query.
 * @return Whether ::mdl_verify_file implements the format.
 * @retval true The format can be structurally validated in process.
 * @retval false The format is invalid, loose, or currently unsupported.
 * @pre @p format is represented by ::mdl_format_t.
 * @pre The caller does not infer writer availability from this predicate.
 * @post No caller or global state is modified.
 * @post Repeated calls with the same value return the same result.
 * @note Thread-safe: this is a pure classifier.
 * @since 0.1.0
 */
bool mdl_format_is_verifiable(mdl_format_t format);

/**
 * @brief Validate a completed artifact using caller-owned scratch only
 *
 * @details Dispatches to the format-specific ZIP, tar, gzip, or JOF reader,
 *          rejects unsafe member paths and missing required metadata, and
 *          resets the workspace so `high_water` describes this call alone.
 * @param[in]     fmt    Expected artifact format.
 * @param[in]     path   NUL-terminated path to the completed artifact.
 * @param[in,out] ws     Caller-owned bounded validation workspace.
 * @param[out]    report Structural counts populated only on success.
 * @return Validation or argument status.
 * @retval k_ra8_ok The artifact is structurally valid for @p fmt.
 * @retval k_ra8_err_invalid_arg A pointer, workspace, or format is invalid.
 * @retval k_ra8_err_invalid_size The caller workspace is too small.
 * @retval k_ra8_err_validation_failed Container structure or metadata is bad.
 * @retval k_ra8_err_not_supported The reserved format has no validator.
 * @pre @p path is NUL-terminated and names a stable completed file.
 * @pre @p ws and @p report are exclusive to this call.
 * @post `ws->used` and `ws->high_water` describe this validation attempt.
 * @post On success @p report contains format, member, page, and metadata data.
 * @note Thread-safe across calls that use distinct workspaces and reports.
 * @since 0.1.0
 */
ra8_err_t mdl_verify_file(mdl_format_t            fmt,
                          const char*             path,
                          mdl_export_workspace_t* ws,
                          mdl_verify_report_t*    report);
