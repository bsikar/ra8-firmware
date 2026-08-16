/**
 * @file test_ra8_c6link_mdl_guards_internal.h
 * @brief Private runner for the media service's validation-decision vectors
 * @details Keeps the independence matrix out of the already-large service
 * suite while sharing its executable, backend seam, and protobuf codec.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run every validation-decision independence vector for the service
 * @details Owns its whole fixture -- backend, service, and packed request and
 * response scratch -- and resets it between scenarios, so each vector differs
 * from its control by exactly one condition.
 * @pre The private validation seams are linked into this executable.
 * @pre The generated media codec is linked into this executable.
 * @post The three pure validators and the three dispatch guards are each
 * exercised with N+1 independence vectors.
 * @post No scenario leaves an active job behind.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_mdl_guards_run(void);

#ifdef __cplusplus
}
#endif
