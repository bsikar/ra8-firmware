/**
 * @file test_mdl_cli_internal.h
 * @brief Module-private entry point for exhaustive CLI matrix qualification.
 * @details Separates the exhaustive option/mode matrix from the focused CLI
 *          fixture while exposing only one private test-runner seam.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run every independent mode-by-option matrix qualification.
 * @details Executes the cli matrix run scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre The public CLI parser and validator are linked into the test binary.
 * @pre Standard error is available for expected rejection diagnostics.
 * @post All 520 option/mode cells, repetitions, pick conjunctions, and help
 *       spellings have been asserted.
 * @post No filesystem or network resource remains open.
 * @note Host-only and not thread-safe because usage capture redirects stderr.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_cli_matrix_run(void);
