/**
 * @file ra_elc.c
 * @brief Event Link Controller helper implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_elc.h"

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "ELC";

typedef enum : uint8_t {
  k_ra_elcr_elcon    = 7U,  /**< ELCR.ELCON bit position. */
  k_ra_elsr_slot_max = 22U, /**< Last valid ELSR slot.  */
} ra_elc_limits_t;

static volatile uint8_t* internal_elcr(void)
{
  return (volatile uint8_t*)(k_ra_elc_base_addr + k_ra_elc_off_elcr);
}

static volatile uint16_t* internal_elsr(uint8_t index)
{
  return (volatile uint16_t*)(k_ra_elc_base_addr + k_ra_elc_off_elsr0 +
                              ((uintptr_t)index * sizeof(uint16_t)));
}

ra_err_t ra_elc_enable(bool enable)
{
  volatile uint8_t* elcr = internal_elcr();
  if (enable) {
    *elcr = (uint8_t)(1U << k_ra_elcr_elcon);
  } else {
    *elcr = 0U;
  }
  ra_log_info_val(s_tag, "elc_enable", enable ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_elc_link(uint8_t elsr_index, ra_elc_event_t event)
{
  if (elsr_index > k_ra_elsr_slot_max) {
    return k_ra_err_out_of_range;
  }
  *internal_elsr(elsr_index) = (uint16_t)event;
  return k_ra_ok;
}
