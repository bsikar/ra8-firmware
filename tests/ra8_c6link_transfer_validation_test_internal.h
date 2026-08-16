/**
 * @file ra8_c6link_transfer_validation_test_internal.h
 * @brief Private runner for transfer-configuration validation vectors
 * @details Keeps the validation matrix out of the already-large media suite.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"
#include "ra8_c6link_mdl_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run every isolated transfer-validation fault vector
 * @details Reuses a complete caller configuration while a private companion
 * replaces only storage begin and the operand selected by each vector.
 * @param[in,out] link Valid link used only after validation succeeds.
 * @param[in] base Complete known-good transfer configuration.
 * @pre @p link and @p base are non-null and remain valid for the call.
 * @pre Every required callback and context in @p base is non-null.
 * @post The all-valid control reaches storage begin exactly once.
 * @post Every single-fault variant is rejected before storage begin.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_transfer_validation_run(ra8_c6link_t*                    link,
                                                       const ra8_mdl_transfer_config_t* base);

#ifdef __cplusplus
}
#endif
