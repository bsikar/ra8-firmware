/**
 * @file test_media_dl_export_mtime_internal.h
 * @brief Private runner seam for exporter page-timestamp derivation tests.
 * @details Keeps the wall-clock-independent modification-time vectors in a
 *          focused companion translation unit without exposing a production
 *          symbol or growing the archive round-trip suite past its line cap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the exporter page-timestamp derivation test group.
 * @details Pins an explicit modification time onto every page fixture so the
 *          "this page is newer than every earlier page" comparison inside
 *          `internal_metadata_set_page_timestamp()` resolves the same way on
 *          every run, whatever the host load. Before these vectors existed the
 *          only thing reaching that comparison was the HTTP integration
 *          script, whose pages are written back to back and therefore usually
 *          share one wall-clock second -- so the true arm fired zero, one or
 *          two times depending on whether a fixture write happened to straddle
 *          a second boundary, and the tree-coverage ratchet measured a
 *          different number every run (#717).
 * @pre The process-local downloader storage binding is initialized.
 * @pre The `/tmp` fixture root can be created and removed.
 * @post Normal return means every derivation assertion passed.
 * @post Every temporary page, archive, and directory has been removed.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_mtime_run(void);
