/**
 * @file ra_nsc_eth.c
 * @brief NSC veneer: ethernet frame TX / RX
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold. The veneers delegate to ``ra_net_pal``
 * which, in turn, delegates to ``ra_eth``. adds
 * ``__attribute__((cmse_nonsecure_entry))`` and runtime address
 * checks via ``cmse_check_address_range``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_net_pal.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"

static const char* s_tag = "NSCETH";

RA_NSC_VENEER ra_err_t ra_nsc_eth_send(const uint8_t* ns_frame, uint16_t len)
{
  RA_CHECK_NULL_PTR(ns_frame, s_tag, "eth_send: ns_frame");
  if ((len == 0U) || (len > (uint16_t)k_ra_nsc_eth_frame_max)) {
    return k_ra_err_invalid_arg;
  }
  RA_NSC_CHECK_NS_RANGE_R(ns_frame, len);
  return ra_net_pal_send_frame(ns_frame, len);
}

RA_NSC_VENEER ra_err_t ra_nsc_eth_recv(uint8_t* ns_buf, uint16_t* inout_len)
{
  RA_CHECK_NULL_PTR(ns_buf, s_tag, "eth_recv: ns_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "eth_recv: inout_len");
  if (*inout_len < (uint16_t)k_ra_nsc_eth_frame_max) {
    return k_ra_err_invalid_arg;
  }
  RA_NSC_CHECK_NS_RANGE_RW(ns_buf, *inout_len);
  RA_NSC_CHECK_NS_RANGE_RW(inout_len, sizeof(*inout_len));
  return ra_net_pal_recv_frame(ns_buf, inout_len);
}
