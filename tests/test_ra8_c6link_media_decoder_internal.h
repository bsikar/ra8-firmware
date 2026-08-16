/**
 * @file test_ra8_c6link_media_decoder_internal.h
 * @brief Private runner seam for malformed C6 media responses.
 * @details Declares one test-target-private runner so response decoder vectors
 * remain separate from the capped transfer-coordinator test translation unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"

/**
 * @brief Run the malformed media-response decoder vectors.
 * @details Exercises generated Accepted field guards and outer CustomRpc
 * envelope/body guards without bypassing the modelled transport.
 * @return Nothing.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The shared C6 model fixture is not in a live transaction.
 * @post Normal return means every malformed response was rejected.
 * @post Every test-created session remains inactive after rejection.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_media_decoder_run(void);
