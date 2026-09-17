/**
 * @file mdl_net_internal.h
 * @brief Private HTTP classification shared by downloader network backends.
 * @details Keeps backend-neutral response policy in one portable seam while
 * concrete backends retain ownership of their transport-specific failures.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "mdl_net.h"
#include "ra8_attributes.h"

/**
 * @brief Classify one observed HTTP response status.
 * @details Applies the downloader's shared throttle, server-error, and
 * client-error rules after a concrete backend completes its transport.
 * @param[in] status Finished HTTP status in the inclusive range 100..599.
 * @return Canonical downloader result for the response status.
 * @retval k_ra8_err_busy Status 429 or 503 requires governor backoff.
 * @retval k_ra8_fail Any other status at or above 500.
 * @retval k_ra8_err_not_found Any other status at or above 400.
 * @retval k_ra8_ok Status below 400.
 * @pre @p status was obtained from a syntactically valid HTTP response.
 * @pre Transport, capacity, and sink failures were handled before this call.
 * @post The status is classified without mutating caller or backend state.
 * @post Equal statuses always produce equal canonical results.
 * @note This is private policy shared by curl and C6link backends.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_net_classify_http(long status);
