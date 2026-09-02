/**
 * @file mdl_verify_rabook_internal.h
 * @brief Private strict RBKC validator seam for media downloader artifacts.
 *
 * @details Keeps the caller-workspace RBKC reader behind the downloader's
 * borrowed-file verifier contract. No filesystem path, allocation, or
 * publication policy crosses this seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "fw_if_fs.h"
#include "mdl_export.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Strictly validate one borrowed RBKC `.rabook` file.
 * @details Opens the callback-driven chunk reader, validates every compressed
 *          stream and the complete inner RABOOK1 structure/CRC, then reports
 *          bounded chapter and image counts.
 * @param[in,out] file Borrowed readable and seekable file.
 * @param[in] size_bytes Immutable complete file extent.
 * @param[in,out] workspace Exclusive caller-owned validation arena.
 * @param[out] report Candidate report populated only on success.
 * @return Strict reader, workspace, or portable-file status.
 * @retval k_ra8_ok Every outer and inner byte passed strict validation.
 * @retval k_ra8_err_invalid_size A wire or caller-workspace bound was exceeded.
 * @retval k_ra8_err_validation_failed A zlib stream or RABOOK1 structure failed.
 * @pre All pointers are non-NULL and @p file remains open for the call.
 * @pre @p workspace was reset by the owning verifier and is exclusively mutable.
 * @post Success initializes RABOOK counts in @p report.
 * @post The borrowed file remains open; its final cursor is unspecified.
 * @note Not thread-safe for a shared file or workspace.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_verify_rabook(fw_fs_file_t*           file,
                                          uint64_t                size_bytes,
                                          mdl_export_workspace_t* workspace,
                                          mdl_verify_report_t*    report);
