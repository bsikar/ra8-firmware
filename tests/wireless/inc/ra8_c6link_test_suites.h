/**
 * @file ra8_c6link_test_suites.h
 * @brief Cohesive suite runners for the C6 link facade host test.
 * @details Declares the session-facing and transport-facing suite partitions
 * invoked by the small shared test executable entry point.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Run the session, Wi-Fi, and event facade scenarios.
 * @return Nothing.
 * @pre The bounded C6 model fixture is linked into this test executable.
 * @post Every session-facing facade scenario has completed its assertions.
 * @since 0.1.0
 */
void ra8_test_c6link_session(void);

/**
 * @brief Run the transport, framing, and RPC failure scenarios.
 * @return Nothing.
 * @pre The bounded C6 model fixture is linked into this test executable.
 * @post Every transport-facing facade scenario has completed its assertions.
 * @since 0.1.0
 */
void ra8_test_c6link_transport(void);
