/**
 * @file test_ra8_log_host_safety.c
 * @brief Hosted safety regression for the default RA8 log backend.
 * @details Exercises an unbound logger without installing fake target MMIO so
 * sanitizers prove that off-target diagnostics never dereference ITM.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @brief Emit hosted diagnostics while the optional sink is unbound.
 * @details Calls both plain and valued public emitters after explicitly
 * selecting the default backend; successful return proves hosted fail-closed
 * behavior without target register access.
 * @pre The test target is compiled with `RA8_OFF_TARGET`.
 * @pre No fake architectural MMIO range is mapped by this process.
 * @post Every public call returns without a signal or memory access failure.
 * @post The optional byte sink remains unbound.
 * @note ASan execution is the authoritative target-address safety check.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_unbound_host_logger_drops(void)
{
  TEST_BEGIN("unbound hosted logger drops without ITM access");
  ra8_log_set_byte_sink(nullptr, nullptr);
  ra8_log_init();
  ra8_log_emit_error("HOST", "plain");
  ra8_log_emit_warn_val("HOST", "valued", 1U);
  TEST_END("unbound hosted logger drops without ITM access");
}

/**
 * @brief Run the hosted logger safety regression.
 * @return Zero after the vector passes; assertion failure exits nonzero.
 * @pre Raw-descriptor Unity output is available.
 * @pre The linked logger was compiled with `RA8_OFF_TARGET`.
 * @post The hosted logger safety vector has executed exactly once.
 * @post The process returns without touching target-only MMIO.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_unbound_host_logger_drops();
  return 0;
}
