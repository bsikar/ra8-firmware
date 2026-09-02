/**
 * @file test_ra8_c6link_transfer_coordinator_internal.h
 * @brief Private runner for transactional-coordinator MC/DC vectors
 * @details Keeps the pull-budget, remote-cancellation, and terminal-metadata
 * matrix out of the already-large media suite while sharing its executable.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run every isolated transfer-coordinator sequencing vector
 * @details Owns its whole fixture -- temporary storage, streaming hash, and
 * terminal chunk -- so each scenario differs from its control by exactly one
 * operand, and brings the shared C6 model up before every scenario.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The link the model fixture publishes is addressable.
 * @post The pull budget, remote cancellation, and terminal-metadata guards are
 * each exercised in both directions.
 * @post No scenario leaves a committed object behind after a failure.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_transfer_coordinator_run(void);

#ifdef __cplusplus
}
#endif
