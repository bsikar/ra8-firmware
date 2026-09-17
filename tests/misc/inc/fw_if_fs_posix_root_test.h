/**
 * @file fw_if_fs_posix_root_test.h
 * @brief POSIX composition-root traversal security vectors
 *
 * @details
 * Declares the hosted test helper that validates descriptor-relative root
 * selection independently of portable paths resolved after initialization.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run composition-root traversal and descriptor-ownership scenarios
 *
 * @details
 * Covers absolute and relative roots, repeated and trailing separators, dot
 * and parent components, final and intermediate symbolic links, missing and
 * non-directory components, zero-initialized failure state, and lowest-free
 * descriptor reuse after every observed bind.
 *
 * @param[in] root Existing writable private native fixture root.
 * @pre @p root is a non-null NUL-terminated absolute directory path.
 * @pre The fixture contains none of the private leaves created by this helper.
 * @post Every created directory, file, and symbolic link is removed.
 * @post Every successful bind is deinitialized and no descriptor is retained.
 * @note Not thread-safe because relative cases temporarily change process cwd.
 * @par MC/DC:
 * No compound production decision or MC/DC citation is attributed to this
 * test helper.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_fw_if_fs_posix_roots(const char* root);
