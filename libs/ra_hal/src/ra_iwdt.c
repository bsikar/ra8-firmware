/**
 * @file ra_iwdt.c
 * @brief Minimal IWDT driver implementation
 *
 * @details
 * The IWDT configuration (period, window, reset-vs-interrupt output)
 * lives in the OFS0 option-setting register, which is programmed at
 * flash-write time rather than at runtime. This driver therefore only
 * provides the runtime refresh call and a no-op init. A real project
 * should extend `ra_iwdt_init()` to publish the effective period by
 * reading OFS0.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iwdt.h"

#include <stdint.h>

#include "ra8d2_iwdt_regs.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IWDT";

/**
 * @brief Prepare the IWDT driver layer.
 *
 * @details
 * No-op in v0.x: the IWDT is armed by OFS0 at reset, so there is
 * nothing for the driver to programme. We just log that we are alive.
 *
 * @return `k_ra_ok`.
 */
[[nodiscard]] ra_err_t ra_iwdt_init(void)
{
  ra_log_info(s_tag, "iwdt_init (OFS0 controls period)");
  return k_ra_ok;
}

/**
 * @brief Refresh the IWDT counter.
 *
 * @details
 * Thin wrapper around the inline `ra_iwdt_refresh()` helper in the
 * register header. Provided as an extern so other compilation units
 * do not have to pull in `ra8d2_iwdt_regs.h`.
 */
void ra_iwdt_refresh_deferred(void)
{
  ra_iwdt_refresh();
}
