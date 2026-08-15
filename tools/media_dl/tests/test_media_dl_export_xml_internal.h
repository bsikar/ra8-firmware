/**
 * @file test_media_dl_export_xml_internal.h
 * @brief Private runner seam for export XML escaping.
 * @details Declares one test-target-private runner so responsibility-focused
 *          test cases can live in a capped companion translation unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the export XML escaping test group.
 * @details Verifies exact metacharacter escaping, ordinary-name preservation,
 *          and refusal when the destination cannot hold the complete result.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_xml_run(void);
