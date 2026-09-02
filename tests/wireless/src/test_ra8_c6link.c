/**
 * @file test_ra8_c6link.c
 * @brief Entry point for the complete C6 link facade test executable.
 * @details Runs the split session and transport suites in a deterministic
 * order while preserving one registered host-test target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_test_suites.h"

/**
 * @brief Run every C6 link facade scenario in its established order.
 * @return Zero after all assertions complete.
 * @since 0.1.0
 */
int main(void)
{
  ra8_test_c6link_session();
  ra8_test_c6link_transport();
  return 0;
}
