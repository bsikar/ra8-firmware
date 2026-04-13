/**
 * @file ra_nsc_eth.c
 * @brief NSC veneer: ethernet frame TX / RX
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wave 7.3 scaffold. The veneers delegate to ``ra_net_pal``
 * which, in turn, delegates to ``ra_eth``. Wave 9.2 adds
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

static const char* s_tag = "NSCETH";

ra_err_t ra_nsc_eth_send(const uint8_t* ns_frame, uint16_t len)
{
  RA_CHECK_NULL_PTR((void*)ns_frame, s_tag, "eth_send: ns_frame");
  if ((len == 0U) || (len > (uint16_t)k_ra_nsc_eth_frame_max)) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 9.2 retrofit point: cmse_check_address_range(ns_frame, len, R). */
  return ra_net_pal_send_frame(ns_frame, len);
}

ra_err_t ra_nsc_eth_recv(uint8_t*  ns_buf,    // NOLINT(readability-non-const-parameter)
                         uint16_t* inout_len) // NOLINT(readability-non-const-parameter)
{
  RA_CHECK_NULL_PTR(ns_buf, s_tag, "eth_recv: ns_buf");
  RA_CHECK_NULL_PTR(inout_len, s_tag, "eth_recv: inout_len");
  if (*inout_len < (uint16_t)k_ra_nsc_eth_frame_max) {
    return k_ra_err_invalid_arg;
  }
  /* Wave 9.2 retrofit point:
   *   cmse_check_address_range(ns_buf, *inout_len, RW);
   *   cmse_check_address_range(inout_len, sizeof(*inout_len), RW); */
  return ra_net_pal_recv_frame(ns_buf, inout_len);
}
