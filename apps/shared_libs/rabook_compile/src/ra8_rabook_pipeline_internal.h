/**
 * @file ra8_rabook_pipeline_internal.h
 * @brief Private validation seam shared by RABOOK pipeline entry points.
 * @details Keeps the buffer compiler independent of the optional ra8_fs
 *          publication wrapper while preserving one argument/profile policy.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include "ra8_attributes.h"
#include "ra8_rabook_pipeline.h"

/**
 * @brief Validate arguments common to both RABOOK pipeline entry points.
 * @details Checks the reader lifecycle plus every builder and scratch span
 *          required by the buffer compiler before it mutates output state.
 * @param[in] epub Open streamed or resident EPUB reader.
 * @param[in] bufs Complete caller-owned builder arenas.
 * @param[in] scr Complete caller-owned parser/raster scratch.
 * @return Shared argument and lifecycle status.
 * @retval k_ra8_ok The compiler may consume every supplied view.
 * @retval k_ra8_err_null_ptr A required pointer or nested span is NULL.
 * @retval k_ra8_err_invalid_state The EPUB reader is not open.
 * @pre Pointer arguments may be NULL because this is the validating seam.
 * @pre Non-NULL nested spans carry their declared writable capacities.
 * @post Success changes no caller state.
 * @post Failure performs no builder or filesystem mutation.
 * @note Thread-safe across distinct immutable argument objects.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_rabook_pipeline_check_common(const epub_book_t*                   epub,
                                                     const ra8_rabook_buffers_t*          bufs,
                                                     const ra8_rabook_pipeline_scratch_t* scr);
