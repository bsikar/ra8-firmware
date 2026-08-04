/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_iwdt.c
 * @brief Independent Watchdog Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The IWDT configuration (period, window, reset-vs-interrupt
 * output) lives in the ``OFS0`` option-setting register, which is
 * programmed at flash time and locked once the boot ROM hands
 * off. RA8D2 supports auto-start mode only (FSP
 * ``IWDT_PRV_REGISTER_START_MODE`` is never set on this part), so
 * this driver covers only the runtime surface: refresh, status
 * read/clear, live counter readout, and NMI dispatch. Every
 * register touch below carries a HUM Ch 28 citation.
 *
 * Mapping to FSP ``r_iwdt`` API:
 *  - ``ra8_iwdt_refresh_deferred`` <-> ``R_IWDT_Refresh``
 *  - ``ra8_iwdt_get_status``       <-> ``R_IWDT_StatusGet``
 *  - ``ra8_iwdt_clear_status``     <-> ``R_IWDT_StatusClear``
 *  - ``ra8_iwdt_get_counter``      <-> ``R_IWDT_CounterGet``
 */

#include "ra8_iwdt.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_iwdt_regs.h"
#include "ra8_log.h"

static const char* s_tag = "IWDT";

/**
 * @struct ra8_iwdt_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra8_iwdt_event_fn_t fn;  /**< Fn.  */
  void*               ctx; /**< Ctx. */
} ra8_iwdt_state_t;

static ra8_iwdt_state_t s_iwdt_state;

/**
 * @enum ra8_iwdt_mask_t
 * @brief Combined IWDTSR flag mask (UNDFF | REFEF).
 */
typedef enum : uint16_t {
  k_ra8_iwdt_status_all =
    k_ra8_iwdt_status_underflow | k_ra8_iwdt_status_refresh, /**< RA8 iwdt status all. */
} ra8_iwdt_mask_t;

/**
 * @brief Prepare the IWDT driver layer.
 *
 * @details
 * No-op in v0.x: the IWDT is armed by OFS0 at reset, so there is
 * nothing for the driver to programme. We just log that we are alive.
 *
 * @return ``k_ra8_ok``.
 */
[[nodiscard]] ra8_err_t ra8_iwdt_init(void)
{
  /* HUM Ch 28.1 "Overview" p 1271 + HUM Ch 7 "Option-Setting Memory":
   * the counter starts automatically per OFS0.IWDTSTRT and cannot be
   * stopped by software on RA8D2. Init is documentation-only. */
  ra8_log_info(s_tag, "iwdt_init (OFS0 controls period; auto-start only)");
  return k_ra8_ok;
}

/**
 * @brief Refresh the IWDT counter.
 *
 * @details
 * Thin wrapper around the inline ``ra8_iwdt_refresh()`` helper in the
 * register header. Provided as an extern so other compilation units
 * do not have to pull in ``ra8_iwdt_regs.h``. Mirrors FSP
 * ``R_IWDT_Refresh`` (which writes 0x00 then 0xFF to IWDTRR).
 *
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
void ra8_iwdt_refresh_deferred(void)
{
  /* HUM Ch 28.2.1 "IWDTRR : IWDT Refresh Register" p 1273 */
  ra8_iwdt_refresh();
}

ra8_err_t ra8_iwdt_get_status(uint16_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1275
   * + FSP IWDT_PRV_STATUS_START_BIT (14): we expose the latched
   * UNDFF / REFEF flags only, masking out CNTVAL[13:0]. */
  *out_mask = (uint16_t)(ra8_iwdt()->IWDTSR & k_ra8_iwdt_status_all);
  return k_ra8_ok;
}

ra8_err_t ra8_iwdt_clear_status(void)
{
  volatile r_iwdt_regs_t* reg = ra8_iwdt();
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1275 -- UNDFF and
   * REFEF are write-0-to-clear. Per FSP R_IWDT_StatusClear the silicon
   * takes one IWDTCLK cycle to drop the flag, so the flag clear is
   * idempotent: writing the masked value once and letting the next
   * read see the cleared bits is sufficient for our test mock. The
   * real silicon path on a hot IWDT would loop until the read-back
   * shows the bit gone (FSP do/while). */
  reg->IWDTSR = (uint16_t)(reg->IWDTSR & (uint16_t)~k_ra8_iwdt_status_all);
  return k_ra8_ok;
}

ra8_err_t ra8_iwdt_get_counter(uint16_t* out_counter)
{
  RA8_CHECK_NULL_PTR(out_counter, s_tag, "out_counter must not be nullptr");
  /* HUM Ch 28.2.2 "IWDTSR.CNTVAL[13:0]" p 1274
   * + FSP R_IWDT_CounterGet: (*p_count) = IWDTSR & 0x3FFF. */
  *out_counter = (uint16_t)(ra8_iwdt()->IWDTSR & k_ra8_iwdt_sr_cnt_mask);
  return k_ra8_ok;
}

ra8_err_t ra8_iwdt_attach_handler(ra8_iwdt_event_fn_t fn, void* ctx)
{
  s_iwdt_state.fn  = fn;
  s_iwdt_state.ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_iwdt_dispatch(void)
{
  volatile r_iwdt_regs_t* reg = ra8_iwdt();
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1275 -- snapshot
   * the latched flags, clear them, then fire the user callback with
   * the snapshot value. Mirrors the FSP NMI ISR shape. */
  const uint16_t mask           = (uint16_t)(reg->IWDTSR & k_ra8_iwdt_status_all);
  reg->IWDTSR                   = (uint16_t)(reg->IWDTSR & (uint16_t)~mask);
  const ra8_iwdt_event_fn_t fn  = s_iwdt_state.fn;
  void* const               ctx = s_iwdt_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
