/**
 * @file fuzz_ra_net_arp.c
 * @brief libFuzzer harness for the ra_net ARP receive path.
 *
 * @details
 * The ARP receive path parses Ethernet frames and ARP packets coming
 * from the network. The parser must reject every malformed frame
 * without crashing or corrupting the ARP cache. The harness brings up
 * the stack with a fixed configuration, then injects the libFuzzer
 * payload as an Ethernet frame whose EtherType field is forced to
 * ARP (0x0806) so the dispatcher routes it to ra_net_arp_handle().
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
  k_fuzz_eth_min_frame   = 14U,
  k_fuzz_eth_max_frame   = 1518U,
  k_fuzz_eth_type_offset = 12U,
  k_fuzz_eth_type_arp_hi = 0x08U,
  k_fuzz_eth_type_arp_lo = 0x06U,
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
  /* Force the EtherType so the dispatcher routes to ARP. */
  s_frame[k_fuzz_eth_type_offset]      = k_fuzz_eth_type_arp_hi;
  s_frame[k_fuzz_eth_type_offset + 1U] = k_fuzz_eth_type_arp_lo;
  (void)ra_net_test_inject_frame(s_frame, (uint16_t)size);
  return 0;
}
