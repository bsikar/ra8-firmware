/**
 * @file mdl_host_credentials_internal.h
 * @brief Private host credential-snapshot hardening helpers.
 * @details Exposes only the pure metadata-stability decision so host tests can
 *          prove subsecond mutation rejection; no path enters portable policy.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <sys/stat.h>

#include "ra8_attributes.h"

/**
 * @brief Compare identity, size, mode, and nanosecond mutation metadata.
 * @param[in] before Metadata captured immediately after opening a regular file.
 * @param[in] after Metadata captured after the exact bounded read and EOF probe.
 * @return Whether the descriptor-backed source remained unchanged.
 * @pre @p before and @p after are non-NULL.
 * @post No state is modified.
 * @note Uses Darwin `st_*timespec` and POSIX/Linux `st_*tim` fields explicitly.
 * @since 0.1.0

 * @details Compares the complete security-relevant snapshots supplied by the caller.
 *          Neither input snapshot is modified or retained.
 * @retval true The documented predicate holds or the requested operation completed.
 * @retval false The predicate does not hold or validation rejected the operation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_PRIV bool priv_mdl_host_credential_stat_unchanged(const struct stat* before,
                                                      const struct stat* after);
