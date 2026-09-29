/**
 * @file test_ra8_c6link_mdl_contract_internal.h
 * @brief Private runner for the published media dispatch contract vectors
 * @details Keeps the contract-binding vectors out of the already-large service
 * suite while sharing its executable, backend seam, and generated codec.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drive the media service through its published dispatch contract type
 * @details Owns its whole fixture -- backend, service, and packed request and
 * response scratch -- and calls every operation through a
 * ::ra8_mdl_service_dispatch_fn value rather than through the function name,
 * so the contract has a real user and its documented status set is executed.
 * @pre The portable media service is linked into this executable.
 * @pre The generated media codec is linked into this executable.
 * @post Start, Next, and Cancel have each been dispatched through the contract.
 * @post Every status the contract documents for a bad call was observed.
 * @post No scenario leaves an active job behind.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_mdl_contract_run(void);

#ifdef __cplusplus
}
#endif
