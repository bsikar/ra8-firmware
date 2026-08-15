/**
 * @file test_media_dl_logic_cli_internal.h
 * @brief Private runner seam for media logic CLI validation.
 * @details Declares one test-target-private runner so responsibility-focused
 *          test cases can live in a capped companion translation unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the media logic CLI validation test group.
 * @details Executes the split mode, conflict, flag, and numeric-boundary cases
 *          through production CLI parsing and validation interfaces.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_logic_cli_run(void);
