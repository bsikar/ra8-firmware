/**
 * @file test_ra8_c6link_mdl_decode_internal.h
 * @brief Private runner for the media client's response-validator vectors
 * @details Keeps the independence matrix out of the already-large media suite
 * while sharing its executable, model fixture, and generated codec.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run every response-validator independence vector for the client
 * @details Owns its whole fixture -- body bytes, digest, and decoded chunk
 * templates -- so each vector differs from its control by exactly one field,
 * and brings the shared C6 model up before the public Start vectors.
 * @pre The private validation seams are linked into this executable.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @post Every decoded-response predicate and both Start guards are exercised
 * with N+1 independence vectors.
 * @post No scenario leaves an active session behind.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_mdl_decode_run(void);

#ifdef __cplusplus
}
#endif
