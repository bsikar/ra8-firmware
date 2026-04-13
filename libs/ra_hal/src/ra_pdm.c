/**
 * @file ra_pdm.c
 * @brief Pulse Density Modulation Interface (PDM-IF) driver scaffold
 *
 * @details
 * Wave 6.1 scaffold -- covers lifecycle + status + IRQ + power
 * transition. Full PCM decimation / FIR filter / stereo capture
 * lands with the first audio consumer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_pdm.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_pdm_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "PDM";

static ra_pdm_event_fn_t s_pdm_fn;
static void*             s_pdm_ctx;

ra_err_t ra_pdm_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_pdmif);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "pdm_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_pdm_regs_t* reg = ra_pdm();
  reg->PDM_CTRL              = 0U;
  reg->PDM_CFG               = 0U;
  reg->PDM_STAT              = 0U;
  reg->PDM_IER               = 0U;
  ra_log_info(s_tag, "pdm_init");
  return k_ra_ok;
}

ra_err_t ra_pdm_deinit(void)
{
  volatile r_pdm_regs_t* reg = ra_pdm();
  reg->PDM_CTRL              = 0U;
  reg->PDM_IER               = 0U;
  s_pdm_fn                   = nullptr;
  s_pdm_ctx                  = nullptr;
  return ra_mstp_disable(k_ra_mstp_pdmif);
}

ra_err_t ra_pdm_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = ra_pdm()->PDM_STAT;
  return k_ra_ok;
}

ra_err_t ra_pdm_clear_status(uint32_t mask)
{
  volatile r_pdm_regs_t* reg = ra_pdm();
  reg->PDM_STAT              = reg->PDM_STAT & ~mask;
  return k_ra_ok;
}

ra_err_t ra_pdm_attach_handler(ra_pdm_event_fn_t fn, void* ctx)
{
  s_pdm_fn  = fn;
  s_pdm_ctx = ctx;
  return k_ra_ok;
}

void ra_pdm_dispatch(void)
{
  volatile r_pdm_regs_t*  reg  = ra_pdm();
  const uint32_t          mask = reg->PDM_STAT;
  const ra_pdm_event_fn_t fn   = s_pdm_fn;
  void* const             ctx  = s_pdm_ctx;
  reg->PDM_STAT                = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_pdm_enter_stop(void)
{
  ra_pdm()->PDM_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_pdmif);
}

ra_err_t ra_pdm_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_pdmif);
}
