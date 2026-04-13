/**
 * @file ra_nsc_xspi.c
 * @brief NSC veneer: external XSPI flash read + status
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wave 7.3 scaffold. Runs as ordinary code in the single-world
 * build; gains ``__attribute__((cmse_nonsecure_entry))`` and the
 * ``cmse_check_address_range`` call site in Wave 9.2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"
#include "ra_xspi.h"

static const char* s_tag = "NSCXSPI";

RA_NSC_VENEER ra_err_t ra_nsc_xspi_read(uint32_t flash_off,
                                        uint8_t* ns_dst, // NOLINT(readability-non-const-parameter)
                                        uint32_t len)
{
  RA_CHECK_NULL_PTR(ns_dst, s_tag, "xspi_read: ns_dst");
  if ((len == 0U) || (len > (uint32_t)k_ra_nsc_xspi_max_read)) {
    return k_ra_err_invalid_arg;
  }
  RA_NSC_CHECK_NS_RANGE_RW(ns_dst, len);
  (void)flash_off;
  /* Wave 7.3 stub: ra_xspi exposes init/status/erase/read_id but no
   * generic flash-read primitive yet; that lands in Wave 5.1c. */
  return k_ra_err_not_supported;
}

RA_NSC_VENEER ra_err_t ra_nsc_xspi_status(uint8_t instance, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "xspi_status: out_mask");
  RA_NSC_CHECK_NS_RANGE_RW(out_mask, sizeof(*out_mask));
  return ra_xspi_get_status(instance, out_mask);
}
