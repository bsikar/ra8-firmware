/**
 * @file test_ra8_bench_output.c
 * @brief Deterministic golden for the descriptor-backed benchmark CSV writer
 * @details Writes one fixed header and result row into an injected RAM sink so
 * timing-independent output shape and two-decimal rounding remain exact.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_bench.h"

/** @brief Deterministic benchmark-golden buffer capacity. */
typedef enum : size_t {
  k_bench_golden_capacity = 256U, /**< Complete fixed CSV fixture capacity. */
} bench_golden_limit_t;

/** @brief Caller-owned RAM sink used by the benchmark golden. */
typedef struct {
  uint8_t bytes[k_bench_golden_capacity]; /**< Accepted output.     */
  size_t  length;                         /**< Accepted byte count. */
} bench_golden_sink_t;

/**
 * @brief Append one bounded chunk to the benchmark-golden RAM sink.
 * @details Implements the injected callback contract over fixed caller-owned
 * storage so the renderer can be tested without descriptors or allocation.
 * @param[in,out] context Bound ::bench_golden_sink_t fixture.
 * @param[in] bytes Source bytes to append.
 * @param[in] length Requested source byte count.
 * @return Exact accepted count or a capacity failure.
 * @retval k_ra8_test_output_ok In `status` when the complete chunk was appended.
 * @retval k_ra8_test_output_short In `status` when the chunk exceeded capacity.
 * @pre @p context points to a writable ::bench_golden_sink_t.
 * @pre @p bytes spans @p length readable bytes.
 * @post Success appends exactly @p length bytes and advances the sink length.
 * @post Failure leaves the sink bytes and length unchanged.
 * @note The caller ensures the existing sink length is within its capacity.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_test_output_result_t
internal_bench_golden_write(void* context, const uint8_t* bytes, size_t length)
{
  bench_golden_sink_t*     sink      = (bench_golden_sink_t*)context;
  const size_t             available = sizeof(sink->bytes) - sink->length;
  ra8_test_output_result_t result    = {};
  if (length > available) {
    result.status = k_ra8_test_output_short;
    return result;
  }
  (void)memcpy(&sink->bytes[sink->length], bytes, length);
  sink->length += length;
  result.status   = k_ra8_test_output_ok;
  result.accepted = length;
  return result;
}

/** @brief Emit and compare the deterministic benchmark CSV fixture. */
int main(void)
{
  static const char   k_expected[] = "# ra8_bench: bench_fixture\n"
                                     "name,iterations,ns_per_op,MB_per_s\n"
                                     "fixture,4,250.00,1000.00\n";
  bench_golden_sink_t sink         = {};
  ra8_test_output_t   output       = {};
  if (!internal_test_output_init(&output, internal_bench_golden_write, &sink) ||
      (internal_bench_print_header(&output, "bench_fixture") != k_ra8_test_output_ok) ||
      (internal_bench_report(&output, "fixture", 4U, 1000U, 250U) != k_ra8_test_output_ok)) {
    return 1;
  }
  return ((sink.length == (sizeof(k_expected) - 1U)) &&
          (memcmp(sink.bytes, k_expected, sizeof(k_expected) - 1U) == 0))
           ? 0
           : 1;
}
