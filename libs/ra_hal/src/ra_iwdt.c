/**
 * @file ra_iwdt.c
 * @brief Independent Watchdog Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The IWDT configuration (period, window, reset-vs-interrupt
 * output) lives in the ``OFS0`` option-setting register, which is
 * programmed at flash time and locked once the boot ROM hands
 * off. This driver therefore covers only the runtime surface:
 * refresh, status read/clear, and NMI dispatch. Every register
 * write below carries a HUM Ch 28 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iwdt.h"

#include <stdint.h>

#include "ra8d2_iwdt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IWDT";

/**
 * @struct ra_iwdt_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_iwdt_event_fn_t fn;
  void*              ctx;
} ra_iwdt_state_t;

static ra_iwdt_state_t s_iwdt_state;

/**
 * @enum ra_iwdt_mask_t
 * @brief Combined IWDTSR mask.
 */
typedef enum : uint16_t {
  k_ra_iwdt_status_all = (uint16_t)k_ra_iwdt_status_underflow | (uint16_t)k_ra_iwdt_status_refresh,
} ra_iwdt_mask_t;

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
  /* HUM Ch 28 "Independent Watchdog Timer (IWDT)" p 1271 -- the
   * counter starts automatically per OFS0.IWDTSTRT and cannot be
   * stopped by software. Init is documentation-only. */
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

ra_err_t ra_iwdt_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1278 */
  *out_mask = (uint16_t)(ra_iwdt()->IWDTSR & (uint16_t)k_ra_iwdt_status_all);
  return k_ra_ok;
}

ra_err_t ra_iwdt_clear_status(void)
{
  volatile r_iwdt_regs_t* reg = ra_iwdt();
  /* IWDTSR is write-0-to-clear for the flag bits.
   * HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1278 */
  reg->IWDTSR = (uint16_t)(reg->IWDTSR & (uint16_t)~(uint16_t)k_ra_iwdt_status_all);
  return k_ra_ok;
}

ra_err_t ra_iwdt_attach_handler(ra_iwdt_event_fn_t fn, void* ctx)
{
  s_iwdt_state.fn  = fn;
  s_iwdt_state.ctx = ctx;
  return k_ra_ok;
}

void ra_iwdt_dispatch(void)
{
  volatile r_iwdt_regs_t* reg = ra_iwdt();
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1278 */
  const uint16_t mask          = (uint16_t)(reg->IWDTSR & (uint16_t)k_ra_iwdt_status_all);
  reg->IWDTSR                  = (uint16_t)(reg->IWDTSR & (uint16_t)~mask);
  const ra_iwdt_event_fn_t fn  = s_iwdt_state.fn;
  void* const              ctx = s_iwdt_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
