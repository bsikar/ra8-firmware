/**
 * @file ra_batt.c
 * @brief Implementation of the low-battery nag policy.
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Edge-triggered band detection with hysteresis. See ra_batt.h for the
 * contract. Pure logic: no loop, no recursion, no allocation, no MMIO.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_batt.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Component tag for diagnostic logging. */
static const char* const s_tag = "ra_batt";

ra_err_t ra_batt_monitor_init(ra_batt_monitor_t* mon)
{
  RA_CHECK_NULL_PTR(mon, s_tag, "mon must not be nullptr");
  mon->low_raised      = false;
  mon->critical_raised = false;
  return k_ra_ok;
}

ra_err_t
ra_batt_update(ra_batt_monitor_t* mon, uint8_t soc_pct, bool charging, ra_batt_nag_t* out_nag)
{
  RA_CHECK_NULL_PTR(mon, s_tag, "mon must not be nullptr");
  RA_CHECK_NULL_PTR(out_nag, s_tag, "out_nag must not be nullptr");

  const uint8_t soc = (soc_pct > (uint8_t)k_ra_batt_pct_max) ? (uint8_t)k_ra_batt_pct_max : soc_pct;
  const uint8_t low_rearm = (uint8_t)((uint8_t)k_ra_batt_low_pct + (uint8_t)k_ra_batt_rearm_margin);
  const uint8_t crit_rearm =
    (uint8_t)((uint8_t)k_ra_batt_critical_pct + (uint8_t)k_ra_batt_rearm_margin);

  /* Re-arm a band once charging, or once SOC has recovered above its margin. */
  if (charging || (soc > low_rearm)) {
    mon->low_raised = false;
  }
  if (charging || (soc > crit_rearm)) {
    mon->critical_raised = false;
  }

  ra_batt_nag_t nag = k_ra_batt_nag_none;
  if (!charging) {
    if ((soc <= (uint8_t)k_ra_batt_low_pct) && !mon->low_raised) {
      mon->low_raised = true;
      nag             = k_ra_batt_nag_low;
    }
    if ((soc <= (uint8_t)k_ra_batt_critical_pct) && !mon->critical_raised) {
      mon->critical_raised = true;
      nag                  = k_ra_batt_nag_critical;
    }
  }
  *out_nag = nag;
  return k_ra_ok;
}

const char* ra_batt_nag_str(ra_batt_nag_t nag)
{
  const char* label;
  switch (nag) {
    case k_ra_batt_nag_none:
      label = "OK";
      break;
    case k_ra_batt_nag_low:
      label = "LOW";
      break;
    case k_ra_batt_nag_critical:
      label = "CRITICAL";
      break;
    default:
      label = "?";
      break;
  }
  RA_ASSERT(label != nullptr, "nag label must be non-null");
  return label;
}
