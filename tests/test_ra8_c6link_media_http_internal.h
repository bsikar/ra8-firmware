/**
 * @file test_ra8_c6link_media_http_internal.h
 * @brief Private runner seam for the client HTTP policy and metadata vectors.
 * @details Declares one test-target-private runner and the shared pre-terminal
 * fixture step so the protocol-v3 HTTP vectors stay out of the capped primary
 * media test translation unit.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"

/**
 * @brief Start a modelled job and consume its one non-terminal data frame.
 * @details Leaves the session active at offset six so the caller's next pull
 * is the terminal response.
 * @param[in,out] session Caller-owned correlation state to activate.
 * @return Nothing.
 * @pre The shared C6 model fixture has been brought up.
 * @pre @p session is writable and not already active.
 * @post The session is active with one consumed six-byte data frame.
 * @post The modelled backend cursor sits at the end of the artifact.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_media_before_terminal(ra8_mdl_session_t* session);

/**
 * @brief Run the client HTTP policy and response-metadata vectors.
 * @details Exercises every protocol-v3 request-policy operand of Start and
 * every terminal and nonterminal response-metadata operand of the chunk
 * decoder, through the modelled transport.
 * @return Nothing.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The shared C6 model fixture is not in a live transaction.
 * @post Normal return means every malformed policy and response was rejected.
 * @post Every accepted control job is cancelled before return.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_c6link_media_http_run(void);
