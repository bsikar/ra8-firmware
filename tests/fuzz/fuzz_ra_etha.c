/**
 * @file fuzz_ra_etha.c
 * @brief libFuzzer harness for the ra_etha RX frame parser
 *
 * @details
 * Pumps arbitrary bytes through ::ra_etha_test_inject_rx, the test
 * veneer that mirrors the descriptor-ISR's early-rejection logic.
 * The harness does not care about the parser output -- the goal is
 * to surface any out-of-bounds read or integer UB in the header
 * decode for malformed inputs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_etha.h"

enum : uint32_t {
  k_fuzz_etha_max_input = 2048U,
};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_etha_max_input) {
    return 0;
  }
  uint8_t  dst[6] = {};
  uint8_t  src[6] = {};
  uint16_t etype  = 0U;
  (void)ra_etha_test_inject_rx(data, (uint32_t)size, dst, src, &etype);
  return 0;
}
