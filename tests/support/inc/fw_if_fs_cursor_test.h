/**
 * @file fw_if_fs_cursor_test.h
 * @brief Shared incremental-directory conformance vector declaration.
 * @details
 * Declares the cross-translation-unit test vector that checks independent
 * directory cursors and namespace operations through the portable facade.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "fw_if_fs.h"
#include "ra8_attributes.h"

/**
 * @brief Exercise independent cursors and lock-free nested namespace calls.
 * @param[in] fs Initialized portable filesystem facade with an empty root.
 * @pre @p fs is non-NULL and supports files and directories.
 * @post All cursor fixtures are removed and both cursors are consumed.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since Version 0.1.0 @details Implements the bounded fw if fs test directory cursors fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_TEST_HELPER void fw_if_fs_test_directory_cursors(const fw_fs_t* fs);
