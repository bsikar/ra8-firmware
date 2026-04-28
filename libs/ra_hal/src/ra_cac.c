/**
 * @file ra_cac.c
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_cac.h"

#include <stdint.h>

#include "ra8d2_cac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "CAC";

typedef enum : uint8_t {
  k_ra_cacr0_cfme  = 0U, /**< CFME: clock frequency measurement en.*/
  k_ra_castr_mendf = 1U, /**< Measurement end flag. */
} ra_cac_bit_t;

ra_err_t ra_cac_init(uint16_t upper, uint16_t lower)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_cac);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "cac_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_cac_regs_t* reg = ra_cac();
  /* HUM Ch 10.2.1 "CACR0 : CAC Control Register 0" p 421 */
  reg->CACR0 = 0U;
  /* HUM Ch 10.2.2 "CACR1 : CAC Control Register 1" p 421 */
  reg->CACR1 = 0U;
  /* HUM Ch 10.2.3 "CACR2 : CAC Control Register 2" p 422 */
  reg->CACR2 = 0U;
  /* HUM Ch 10.2.6 "CAULVR : CAC Upper Limit Value Register" p 425 */
  reg->CAULVR = upper;
  /* HUM Ch 10.2.7 "CALLVR : CAC Lower Limit Value Register" p 425 */
  reg->CALLVR = lower;
  ra_log_info(s_tag, "cac_init");
  return k_ra_ok;
}

ra_err_t ra_cac_measure(uint16_t* out_count)
{
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  volatile r_cac_regs_t* reg = ra_cac();

  /* HUM Ch 10.2.1 "CACR0 : CAC Control Register 0" p 421 -- CFME=1 starts
   * a measurement; CASTR.MENDF asserts when the window completes. */
  reg->CACR0 = (uint8_t)(1U << k_ra_cacr0_cfme);

  enum : uint32_t { k_ra_cac_poll_limit = 200000U };
  for (uint32_t i = 0U; i < k_ra_cac_poll_limit; i++) {
    /* HUM Ch 10.2.5 "CASTR : CAC Status Register" p 424 */
    if ((reg->CASTR & (uint8_t)(1U << k_ra_castr_mendf)) != 0U) {
      /* HUM Ch 10.2.4 "CACNTBR : CAC Counter Buffer Register" p 423 */
      *out_count = reg->CACNTBR;
      reg->CACR0 = 0U;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

/**
 * @enum ra_cac_bits_w43_t
 * @brief combined mask.
 */
typedef enum : uint8_t {
  k_ra_cac_status_mask_all = k_ra_cac_status_mendf | k_ra_cac_status_ovff | k_ra_cac_status_ferrf,
} ra_cac_bits_w43_t;

/**
 * @struct ra_cac_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_cac_event_fn_t fn;
  void*             ctx;
} ra_cac_state_t;

static ra_cac_state_t s_cac_state;

ra_err_t ra_cac_deinit(void)
{
  volatile r_cac_regs_t* reg = ra_cac();
  reg->CACR0                 = 0U;
  reg->CACR1                 = 0U;
  reg->CACR2                 = 0U;
  reg->CAICR                 = 0U;
  s_cac_state.fn             = nullptr;
  s_cac_state.ctx            = nullptr;
  return ra_mstp_disable(k_ra_mstp_cac);
}

ra_err_t ra_cac_get_status(uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra_cac()->CASTR & k_ra_cac_status_mask_all);
  return k_ra_ok;
}

ra_err_t ra_cac_clear_status(uint8_t mask)
{
  volatile r_cac_regs_t* reg = ra_cac();
  /* HUM Ch 26.2.4 CAICR bit positions (FERRFCL/MENDFCL/OVFFCL) are
   * offset by 4 from CASTR bits -- write 1 to clear. */
  reg->CAICR = (uint8_t)(mask & k_ra_cac_status_mask_all);
  return k_ra_ok;
}

ra_err_t ra_cac_attach_handler(ra_cac_event_fn_t fn, void* ctx)
{
  s_cac_state.fn  = fn;
  s_cac_state.ctx = ctx;
  return k_ra_ok;
}

void ra_cac_dispatch(void)
{
  volatile r_cac_regs_t* reg  = ra_cac();
  const uint8_t          mask = (uint8_t)(reg->CASTR & k_ra_cac_status_mask_all);
  reg->CAICR                  = mask;
  const ra_cac_event_fn_t fn  = s_cac_state.fn;
  void* const             ctx = s_cac_state.ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_cac_enter_stop(void)
{
  ra_cac()->CACR0 = 0U;
  return ra_mstp_disable(k_ra_mstp_cac);
}

ra_err_t ra_cac_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_cac);
}
