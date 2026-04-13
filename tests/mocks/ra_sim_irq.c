/**
 * @file ra_sim_irq.c
 * @brief Host-test interrupt-injection shim implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA_SIMULATOR_MODE

#include "ra_sim_irq.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_isr.h"

ra_err_t ra_sim_irq_fire(ra_elc_event_t event)
{
  uint16_t       slot = (uint16_t)k_ra_isr_slot_none;
  const ra_err_t err  = ra_isr_lookup_slot(event, &slot);
  if (err != k_ra_ok) {
    return err;
  }
  if (slot == (uint16_t)k_ra_isr_slot_none) {
    return k_ra_err_not_found;
  }
  ra_isr_dispatch(slot);
  return k_ra_ok;
}

#else
/* Non-simulator build: this translation unit is empty. */
typedef int ra_sim_irq_placeholder_t;
#endif /* RA_SIMULATOR_MODE */
