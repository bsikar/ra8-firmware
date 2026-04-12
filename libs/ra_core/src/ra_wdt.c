/**
 * @file ra_wdt.c
 * @brief Software WDT driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_wdt.h"

#include "ra8d2_wdt_regs.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "WDT";

ra_err_t ra_wdt_init(void)
{
  ra_log_info(s_tag, "wdt_init (no-op; OFS0 controls period)");
  return k_ra_ok;
}

void ra_wdt_refresh_deferred(void)
{
  ra_wdt_refresh();
}
