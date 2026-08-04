/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_irq.c
 * @brief Host-test interrupt-injection shim implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 */

#ifdef RA8_OFF_TARGET

#include "ra8_fake_irq.h"

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_isr.h"

ra8_err_t ra8_fake_irq_fire(ra8_elc_event_t event)
{
  uint16_t        slot = (uint16_t)k_ra8_isr_slot_none;
  const ra8_err_t err  = ra8_isr_lookup_slot(event, &slot);
  if (err != k_ra8_ok) {
    return err;
  }
  if (slot == (uint16_t)k_ra8_isr_slot_none) {
    return k_ra8_err_not_found;
  }
  ra8_isr_dispatch(slot);
  return k_ra8_ok;
}

#else
/* On-target build: this translation unit is empty. */
typedef int ra8_fake_irq_placeholder_t;
#endif /* RA8_OFF_TARGET */
