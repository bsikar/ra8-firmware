/**
 * @file fuzz_ra8_etha.c
 * @brief libFuzzer harness for the Ethernet II header parse fixture
 *
 * @details
 * Pumps arbitrary bytes through ::eff_parse_eth_header, the tests-side
 * Ethernet II header parser in ``tests/eth_frame_fixture.h`` (moved out
 * of the ra8_etha driver by issue #238 -- it never touched driver state
 * or MMIO). The harness does not care about the parser output -- the
 * goal is to surface any out-of-bounds read or integer UB in the header
 * decode for malformed inputs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "eth_frame_fixture.h"
#include "fuzz_entry.h"
#include "ra8_err.h"

enum : uint32_t {
  k_fuzz_etha_max_input = 2048U, /**< Fuzz etha maximum input. */
};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_etha_max_input) {
    return 0;
  }
  uint8_t  dst[k_eff_mac_bytes] = {};
  uint8_t  src[k_eff_mac_bytes] = {};
  uint16_t etype                = 0U;
  (void)eff_parse_eth_header(data, (uint32_t)size, dst, src, &etype);
  return 0;
}
