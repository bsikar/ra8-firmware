/**
 * @file ra_wdt.c
 * @brief Software Watchdog Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The WDT period and reset-vs-NMI choice live in the ``OFS0``
 * option-setting register, programmed at flash time and locked
 * once the boot ROM hands off. This driver therefore covers only
 * the runtime surface: refresh, status read/clear, and NMI
 * dispatch. Every register write below carries a HUM Ch 27
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_wdt.h"

#include <stdint.h>

#include "ra8d2_wdt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "WDT";

typedef struct {
  ra_wdt_event_fn_t fn;
  void*             ctx;
} ra_wdt_state_t;

static ra_wdt_state_t s_wdt_state;

typedef enum : uint16_t {
  k_ra_wdt_status_all = (uint16_t)k_ra_wdt_status_underflow | (uint16_t)k_ra_wdt_status_refresh,
} ra_wdt_mask_t;

ra_err_t ra_wdt_init(void)
{
  /* HUM Ch 27 "Watchdog Timer (WDT)" p 1256 -- the period, mode
   * (timer / window), and reset-vs-NMI selector are bits in OFS0
   * which the boot ROM latches before this code can touch them.
   * The driver therefore has nothing to programme at runtime; we
   * still log so the operator sees the layer is alive. */
  ra_log_info(s_tag, "wdt_init (no-op; OFS0 controls period)");
  return k_ra_ok;
}

void ra_wdt_refresh_deferred(void)
{
  ra_wdt_refresh();
}

ra_err_t ra_wdt_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1262 */
  *out_mask = (uint16_t)(ra_wdt()->WDTSR & (uint16_t)k_ra_wdt_status_all);
  return k_ra_ok;
}

ra_err_t ra_wdt_clear_status(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1262 -- the
   * UNDFF / REFEF flag bits are write-0-to-clear. */
  reg->WDTSR = (uint16_t)(reg->WDTSR & (uint16_t)~(uint16_t)k_ra_wdt_status_all);
  return k_ra_ok;
}

ra_err_t ra_wdt_attach_handler(ra_wdt_event_fn_t fn, void* ctx)
{
  s_wdt_state.fn  = fn;
  s_wdt_state.ctx = ctx;
  return k_ra_ok;
}

void ra_wdt_dispatch(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1262 */
  const uint16_t mask         = (uint16_t)(reg->WDTSR & (uint16_t)k_ra_wdt_status_all);
  reg->WDTSR                  = (uint16_t)(reg->WDTSR & (uint16_t)~mask);
  const ra_wdt_event_fn_t fn  = s_wdt_state.fn;
  void* const             ctx = s_wdt_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
