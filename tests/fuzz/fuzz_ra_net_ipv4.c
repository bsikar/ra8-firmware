/**
 * @file fuzz_ra_net_ipv4.c
 * @brief libFuzzer harness for the ra_net IPv4 receive path.
 *
 * @details
 * Drives the IPv4 receive path (which then dispatches to ICMP / UDP /
 * TCP) with arbitrary Ethernet frames whose EtherType is forced to
 * IPv4 (0x0800). The parser must reject every malformed frame without
 * crashing, reading out of bounds, or corrupting socket state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_net.h"
#include "ra_net_pal.h"

enum : uint16_t {
  k_fuzz_eth_min_frame    = 14U,
  k_fuzz_eth_max_frame    = 1518U,
  k_fuzz_eth_type_offset  = 12U,
  k_fuzz_eth_type_ipv4_hi = 0x08U,
  k_fuzz_eth_type_ipv4_lo = 0x00U,
};

static const ra_net_config_t k_fuzz_cfg = {
  .mac        = {.bytes = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U}},
  .ip         = {.bytes = {192U, 168U, 1U, 10U}},
  .netmask    = {.bytes = {255U, 255U, 255U, 0U}},
  .gateway    = {.bytes = {192U, 168U, 1U, 1U}},
  .dns_server = {.bytes = {8U, 8U, 8U, 8U}},
};

static uint8_t s_frame[k_fuzz_eth_max_frame];
static uint8_t s_initialized;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (s_initialized == 0U) {
    (void)ra_mstp_init();
    if (ra_net_open(&k_fuzz_cfg) != k_ra_ok) {
      return 0;
    }
    s_initialized = 1U;
  }
  if (size < k_fuzz_eth_min_frame || size > sizeof s_frame) {
    return 0;
  }
  memcpy(s_frame, data, size);
  s_frame[k_fuzz_eth_type_offset]      = k_fuzz_eth_type_ipv4_hi;
  s_frame[k_fuzz_eth_type_offset + 1U] = k_fuzz_eth_type_ipv4_lo;
  (void)ra_net_test_inject_frame(s_frame, (uint16_t)size);
  return 0;
}
