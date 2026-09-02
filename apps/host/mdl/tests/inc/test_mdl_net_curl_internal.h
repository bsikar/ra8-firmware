/**
 * @file test_mdl_net_curl_internal.h
 * @brief Private runner seam for curl failed-publication preservation.
 * @details Declares one test-target-private runner so responsibility-focused
 *          test cases can live in a capped companion translation unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the curl failed-publication preservation test group.
 * @details Drives the isolated curl failure regression and verifies that an
 *          existing destination and its transaction directory survive intact.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_net_curl_run(void);
