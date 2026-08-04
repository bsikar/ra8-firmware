/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cac.c
 * @brief Clock Frequency Accuracy Measurement Circuit driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the CAC block. The CAC measures one clock
 * source against another over a configurable time window and
 * fires a measurement-end / overflow / frequency-error IRQ when
 * the result is out of bounds. This driver exposes lifecycle,
 * polled measurement, async event dispatch, and power transition.
 * Every register write below carries a HUM Ch 10 citation.
 */

#include "ra8_cac.h"

#include <stdint.h>

#include "ra8_cac_regs.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "CAC";

typedef enum : uint8_t {
  k_ra8_cacr0_cfme  = 0U, /**< CFME: clock frequency measurement en. */
  k_ra8_castr_ferrf = 0U, /**< Frequency error flag (CASTR bit 0).   */
  k_ra8_castr_mendf = 1U, /**< Measurement end flag (CASTR bit 1).   */
  k_ra8_castr_ovff  = 2U, /**< Counter overflow flag (CASTR bit 2).  */
} ra8_cac_bit_t;

/**
 * @enum ra8_cac_misc_t
 * @brief Miscellaneous shifts and bounded-spin budgets.
 *
 * @details FSP r_cac.c uses `FSP_HARDWARE_REGISTER_WAIT(R_CAC->CACR0, x)` to
 * confirm that the start/stop write to CACR0 has propagated through the
 * peripheral clock-domain crossing before the next access; we replace the
 * unbounded wait with a bounded busy-spin (NASA Rule 2).
 */
typedef enum : uint32_t {
  k_ra8_cac_castr_to_caicr_shift = 4U,      /**< CASTR -> CAICR.*CL clear-bit shift. */
  k_ra8_cac_cfme_settle_iters    = 1024U,   /**< Bounded settle on CACR0 write-back. */
  k_ra8_cac_poll_limit           = 200000U, /**< Bounded MENDF poll budget.          */
} ra8_cac_misc_t;

/**
 * @enum ra8_cac_bits_w43_t
 * @brief Combined CASTR status mask for FERRF | MENDF | OVFF.
 */
typedef enum : uint8_t {
  k_ra8_cac_status_mask_all = k_ra8_cac_status_mendf | k_ra8_cac_status_ovff |
                              k_ra8_cac_status_ferrf, /**< RA8 cac status mask all. */
} ra8_cac_bits_w43_t;

/**
 * @brief Bounded busy-spin until `CACR0` reads back `expect`.
 *
 * @details Mirrors FSP `FSP_HARDWARE_REGISTER_WAIT(R_CAC->CACR0, expect)`
 * (HUM Ch 10.2.1 "CACR0" p 421 -- CFME write must propagate before next
 * CACR0 access). Returns regardless after `k_ra8_cac_cfme_settle_iters`
 * to satisfy NASA Rule 2 (fixed loop bound).
 *
 * @param[in] reg    CAC register block.
 * @param[in] expect Expected CACR0 value (0 = stopped, 1 = running).
 *
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static inline void internal_cac_wait_cfme(volatile r_cac_regs_t* reg, uint8_t expect)
{
  for (uint32_t i = 0U; i < k_ra8_cac_cfme_settle_iters; i++) { /* GCOVR_EXCL_BR_LINE */
    if (reg->CACR0 == expect) {                                 /* GCOVR_EXCL_BR_LINE */
      return;
    }
  }
}

ra8_err_t ra8_cac_init(uint16_t upper, uint16_t lower)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_cac);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "cac_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_cac_regs_t* reg = ra8_cac();
  /* HUM Ch 10.2.1 "CACR0 : CAC Control Register 0" p 421 -- CFME must be
   * cleared before CACR1/CACR2/limits are programmed (FSP r_cac.c
   * r_cac_hw_configure() does the same, then waits on CACR0 readback). */
  reg->CACR0 = 0U;
  internal_cac_wait_cfme(reg, 0U);
  /* HUM Ch 10.2.4 "CAICR" p 424 */ /* clear all stale W1C status flags. */
  reg->CAICR = (uint8_t)(k_ra8_cac_status_mask_all << (uint8_t)k_ra8_cac_castr_to_caicr_shift);
  /* HUM Ch 10.2.2 "CACR1 : CAC Control Register 1" p 422 */
  reg->CACR1 = 0U;
  /* HUM Ch 10.2.3 "CACR2 : CAC Control Register 2" p 423 */
  reg->CACR2 = 0U;
  /* HUM Ch 10.2.6 "CAULVR : CAC Upper Limit Value Register" p 426 */
  reg->CAULVR = upper;
  /* HUM Ch 10.2.7 "CALLVR : CAC Lower Limit Value Register" p 426 */
  reg->CALLVR = lower;
  ra8_log_info(s_tag, "cac_init");
  return k_ra8_ok;
}

ra8_err_t ra8_cac_measure(uint16_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  volatile r_cac_regs_t* reg = ra8_cac();

  /* HUM Ch 10.2.1 "CACR0 : CAC Control Register 0" p 421 -- CFME=1 starts
   * a measurement; CASTR.MENDF asserts when the window completes. FSP
   * r_cac.c R_CAC_StartMeasurement() reads CACR0 back to confirm CFME
   * propagation before relying on the result. */
  const uint8_t cfme_set = (uint8_t)(1U << k_ra8_cacr0_cfme);
  reg->CACR0             = cfme_set;
  internal_cac_wait_cfme(reg, cfme_set);

  for (uint32_t i = 0U; i < k_ra8_cac_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 10.2.5 "CASTR : CAC Status Register" p 425 */
    if ((reg->CASTR & (uint8_t)(1U << k_ra8_castr_mendf)) != 0U) { /* GCOVR_EXCL_BR_LINE */
      /* HUM Ch 10.2.8 "CACNTBR : CAC Counter Buffer Register" p 426 */
      *out_count = reg->CACNTBR;
      reg->CACR0 = 0U;
      internal_cac_wait_cfme(reg, 0U);
      return k_ra8_ok;
    }
  }
  reg->CACR0 = 0U;
  internal_cac_wait_cfme(reg, 0U);
  return k_ra8_err_hw_timeout;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

/**
 * @struct ra8_cac_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra8_cac_event_fn_t fn;  /**< Fn.  */
  void*              ctx; /**< Ctx. */
} ra8_cac_state_t;

static ra8_cac_state_t s_cac_state;

ra8_err_t ra8_cac_deinit(void)
{
  volatile r_cac_regs_t* reg = ra8_cac();
  /* HUM Ch 10.2.1 "CACR0" p 421 -- stop measurement; FSP R_CAC_Close()
   * confirms CFME cleared before powering down via MSTP. */
  reg->CACR0 = 0U;
  internal_cac_wait_cfme(reg, 0U);
  reg->CACR1      = 0U;
  reg->CACR2      = 0U;
  reg->CAICR      = 0U;
  s_cac_state.fn  = nullptr;
  s_cac_state.ctx = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_cac);
}

ra8_err_t ra8_cac_get_status(uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra8_cac()->CASTR & k_ra8_cac_status_mask_all);
  return k_ra8_ok;
}

ra8_err_t ra8_cac_clear_status(uint8_t mask)
{
  volatile r_cac_regs_t* reg = ra8_cac();
  /* HUM Ch 10.2.4 "CAICR : CAC Interrupt Control Register" p 424 -- the
   * write-1-to-clear bits are FERRFCL=bit4, MENDFCL=bit5, OVFFCL=bit6,
   * which sit `k_ra8_cac_castr_to_caicr_shift` (= 4) bits above the
   * matching CASTR FERRF/MENDF/OVFF flags. Cross-verified against FSP
   * r_cac.c CAC_PRV_CAICR_FERRFCL_OFFSET=4 / MENDFCL=5 / OVFFCL=6. */
  const uint8_t castr_bits = (uint8_t)(mask & k_ra8_cac_status_mask_all);
  const uint8_t caicr_clr  = (uint8_t)(castr_bits << (uint8_t)k_ra8_cac_castr_to_caicr_shift);
  reg->CAICR               = caicr_clr;
  return k_ra8_ok;
}

ra8_err_t ra8_cac_attach_handler(ra8_cac_event_fn_t fn, void* ctx)
{
  s_cac_state.fn  = fn;
  s_cac_state.ctx = ctx;
  return k_ra8_ok;
}

void ra8_cac_dispatch(void)
{
  volatile r_cac_regs_t* reg  = ra8_cac();
  const uint8_t          mask = (uint8_t)(reg->CASTR & k_ra8_cac_status_mask_all);
  /* HUM Ch 10.2.4 "CAICR" p 424 -- write 1 to FERRFCL/MENDFCL/OVFFCL
   * (bits 4/5/6) to clear the matching CASTR flag, mirroring the FSP
   * ISR pattern `R_CAC->CAICR |= CAC_PRV_CAICR_*FCL_MASK`. */
  reg->CAICR                   = (uint8_t)(mask << (uint8_t)k_ra8_cac_castr_to_caicr_shift);
  const ra8_cac_event_fn_t fn  = s_cac_state.fn;
  void* const              ctx = s_cac_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_cac_enter_stop(void)
{
  volatile r_cac_regs_t* reg = ra8_cac();
  /* HUM Ch 10.2.1 "CACR0" p 421 */ /* stop measurement before MSTP gate. */
  reg->CACR0 = 0U;
  internal_cac_wait_cfme(reg, 0U);
  return ra8_mstp_disable(k_ra8_mstp_cac);
}

ra8_err_t ra8_cac_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_cac);
}
