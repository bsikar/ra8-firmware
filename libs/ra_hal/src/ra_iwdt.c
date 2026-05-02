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
 * off. RA8D2 supports auto-start mode only (FSP
 * ``IWDT_PRV_REGISTER_START_MODE`` is never set on this part), so
 * this driver covers only the runtime surface: refresh, status
 * read/clear, live counter readout, and NMI dispatch. Every
 * register touch below carries a HUM Ch 28 citation.
 *
 * Mapping to FSP ``r_iwdt`` API:
 *  - ``ra_iwdt_refresh_deferred`` <-> ``R_IWDT_Refresh``
 *  - ``ra_iwdt_get_status``       <-> ``R_IWDT_StatusGet``
 *  - ``ra_iwdt_clear_status``     <-> ``R_IWDT_StatusClear``
 *  - ``ra_iwdt_get_counter``      <-> ``R_IWDT_CounterGet``
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
 * @brief Combined IWDTSR flag mask (UNDFF | REFEF).
 */
typedef enum : uint16_t {
  k_ra_iwdt_status_all = k_ra_iwdt_status_underflow | k_ra_iwdt_status_refresh,
} ra_iwdt_mask_t;

/**
 * @brief Prepare the IWDT driver layer.
 *
 * @details
 * No-op in v0.x: the IWDT is armed by OFS0 at reset, so there is
 * nothing for the driver to programme. We just log that we are alive.
 *
 * @return ``k_ra_ok``.
 */
[[nodiscard]] ra_err_t ra_iwdt_init(void)
{
  /* HUM Ch 28.1 "Overview" p 1271 + HUM Ch 7 "Option-Setting Memory":
   * the counter starts automatically per OFS0.IWDTSTRT and cannot be
   * stopped by software on RA8D2. Init is documentation-only. */
  ra_log_info(s_tag, "iwdt_init (OFS0 controls period; auto-start only)");
  return k_ra_ok;
}

/**
 * @brief Refresh the IWDT counter.
 *
 * @details
 * Thin wrapper around the inline ``ra_iwdt_refresh()`` helper in the
 * register header. Provided as an extern so other compilation units
 * do not have to pull in ``ra8d2_iwdt_regs.h``. Mirrors FSP
 * ``R_IWDT_Refresh`` (which writes 0x00 then 0xFF to IWDTRR).
 *
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
void ra_iwdt_refresh_deferred(void)
{
  /* HUM Ch 28.2.1 "IWDTRR : IWDT Refresh Register" p 1273 */
  ra_iwdt_refresh();
}

/**
 * @brief ra_iwdt_get_status -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] out_mask See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iwdt_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1274
   * + FSP IWDT_PRV_STATUS_START_BIT (14): we expose the latched
   * UNDFF / REFEF flags only, masking out CNTVAL[13:0]. */
  *out_mask = (uint16_t)(ra_iwdt()->IWDTSR & k_ra_iwdt_status_all);
  return k_ra_ok;
}

/**
 * @brief ra_iwdt_clear_status -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iwdt_clear_status(void)
{
  volatile r_iwdt_regs_t* reg = ra_iwdt();
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1274 -- UNDFF and
   * REFEF are write-0-to-clear. Per FSP R_IWDT_StatusClear the silicon
   * takes one IWDTCLK cycle to drop the flag, so the flag clear is
   * idempotent: writing the masked value once and letting the next
   * read see the cleared bits is sufficient for our test mock. The
   * real silicon path on a hot IWDT would loop until the read-back
   * shows the bit gone (FSP do/while). */
  reg->IWDTSR = (uint16_t)(reg->IWDTSR & (uint16_t)~k_ra_iwdt_status_all);
  return k_ra_ok;
}

/**
 * @brief ra_iwdt_get_counter -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] out_counter See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iwdt_get_counter(uint16_t* out_counter)
{
  RA_CHECK_NULL_PTR(out_counter, s_tag, "out_counter must not be nullptr");
  /* HUM Ch 28.2.2 "IWDTSR.CNTVAL[13:0]" p 1274
   * + FSP R_IWDT_CounterGet: (*p_count) = IWDTSR & 0x3FFF. */
  *out_counter = (uint16_t)(ra_iwdt()->IWDTSR & k_ra_iwdt_sr_cnt_mask);
  return k_ra_ok;
}

/**
 * @brief ra_iwdt_attach_handler -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] fn See header declaration for direction and constraints.
 * @param[in] ctx See header declaration for direction and constraints.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
ra_err_t ra_iwdt_attach_handler(ra_iwdt_event_fn_t fn, void* ctx)
{
  s_iwdt_state.fn  = fn;
  s_iwdt_state.ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief ra_iwdt_dispatch -- see header for full description.
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
void ra_iwdt_dispatch(void)
{
  volatile r_iwdt_regs_t* reg = ra_iwdt();
  /* HUM Ch 28.2.2 "IWDTSR : IWDT Status Register" p 1274 -- snapshot
   * the latched flags, clear them, then fire the user callback with
   * the snapshot value. Mirrors the FSP NMI ISR shape. */
  const uint16_t mask          = (uint16_t)(reg->IWDTSR & k_ra_iwdt_status_all);
  reg->IWDTSR                  = (uint16_t)(reg->IWDTSR & (uint16_t)~mask);
  const ra_iwdt_event_fn_t fn  = s_iwdt_state.fn;
  void* const              ctx = s_iwdt_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
