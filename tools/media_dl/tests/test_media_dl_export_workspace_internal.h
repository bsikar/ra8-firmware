/**
 * @file test_media_dl_export_workspace_internal.h
 * @brief Private runner seam for exporter workspace-capacity tests.
 * @details Keeps the allocation-free workspace refusal vectors in a focused
 *          companion translation unit without exposing a production symbol.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the exporter workspace-capacity test group.
 * @details Proves the exact CBZ and EPUB high-water marks, one-byte-short
 *          refusal, and preservation of an existing destination.
 * @pre The process-local downloader storage binding is initialized.
 * @pre The `/tmp` fixture root can be created and removed.
 * @post Normal return means every workspace assertion passed.
 * @post All temporary paths and caller-owned workspace state are released.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_workspace_run(void);
