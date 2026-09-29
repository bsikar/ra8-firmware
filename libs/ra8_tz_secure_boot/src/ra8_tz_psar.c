/**
 * @file ra8_tz_psar.c
 * @brief PRCR-gated peripheral security attribution (PSARn)
 * @ingroup grp_system
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tz_psar.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_register_protection.h"
#include "ra8_system_regs.h"

ra8_err_t ra8_tz_psar_set_ns(uintptr_t psar_addr, uint32_t ns_mask, uint32_t* out_seen)
{
  if (psar_addr == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (ns_mask == 0U) {
    /* Nothing to hand over: do not open the gate at all, so a no-op call
     * cannot be the thing that leaves PRC4 unlocked. */
    if (out_seen != NULL) {
      *out_seen = *(volatile const uint32_t*)psar_addr;
    }
    return k_ra8_ok;
  }

  volatile uint32_t* const psar = (volatile uint32_t*)psar_addr;
  uint32_t                 seen = 0U;
  uint32_t                 want = 0U;

  /* PSARn sits behind PRC4 along with SRAMSABARn (HUM Ch 13.2.1 p 521): issued
   * with the gate locked the store is discarded with no fault and no flag.
   * Leave this scope by falling off the end, never by `return`: the re-lock is
   * the macro loop's increment clause and a `return` would jump past it. */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar)
  {
    want  = *psar | ns_mask;
    *psar = want;
    /* HUM "Security or Privilege Bit Write Timing" p 3301: read back until the
     * value matches before relying on the new attribution. */
    for (uint32_t spin = 0U; spin < (uint32_t)k_ra8_tz_psar_readback_spins; spin += 1U) {
      seen = *psar;
      if (seen == want) {
        break;
      }
    }
  }

  if (out_seen != NULL) {
    *out_seen = seen;
  }
  return (seen == want) ? k_ra8_ok : k_ra8_err_timeout;
}
