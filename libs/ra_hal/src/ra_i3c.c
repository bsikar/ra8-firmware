/**
 * @file ra_i3c.c
 * @brief I3C Bus Interface driver scaffold
 *
 * @details
 * Wave 5.5 scaffold -- covers lifecycle + status + IRQ + power
 * transition. Full CCC / IBI / HDR-DDR handling lands with the
 * first consumer driver. See HUM Ch 40 "I3C Bus Interface (I3C)"
 * (p 2445..2701) for the full programming model.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_i3c.h"

#include <stdint.h>

#include "ra8d2_i3c_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "I3C";

static ra_i3c_event_fn_t s_i3c_fn;
static void*             s_i3c_ctx;

ra_err_t ra_i3c_init(void)
{
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_i3c);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "i3c_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_i3c_regs_t* reg = ra_i3c();
  reg->PRTS                  = 0U;
  reg->BCTL                  = 0U;
  reg->INST                  = 0U;
  reg->INSTE                 = 0U;
  reg->IE                    = 0U;
  reg->BST                   = 0U;
  reg->BSTE                  = 0U;
  reg->BIE                   = 0U;
  ra_log_info(s_tag, "i3c_init");
  return k_ra_ok;
}

ra_err_t ra_i3c_deinit(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  reg->IE                    = 0U;
  reg->BIE                   = 0U;
  reg->BCTL                  = 0U;
  s_i3c_fn                   = nullptr;
  s_i3c_ctx                  = nullptr;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = ra_i3c()->INST;
  return k_ra_ok;
}

ra_err_t ra_i3c_clear_status(uint32_t mask)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  reg->INST                  = reg->INST & ~mask;
  return k_ra_ok;
}

ra_err_t ra_i3c_attach_handler(ra_i3c_event_fn_t fn, void* ctx)
{
  s_i3c_fn  = fn;
  s_i3c_ctx = ctx;
  return k_ra_ok;
}

void ra_i3c_dispatch(void)
{
  volatile r_i3c_regs_t*  reg  = ra_i3c();
  const uint32_t          mask = reg->INST;
  const ra_i3c_event_fn_t fn   = s_i3c_fn;
  void* const             ctx  = s_i3c_ctx;
  reg->INST                    = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_i3c_enter_stop(void)
{
  ra_i3c()->BCTL = 0U;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_i3c);
}
