/**
 * @file ra_eth.c
 * @brief Ethernet Switch Module (ESWM) unified scaffold driver
 *
 * @details
 * Wave 6.3 scaffold. See ``libs/ra_hal/inc/ra_eth.h`` for the
 * public surface and the roadmap note on why mfwd / coma / gwca /
 * gptp sub-drivers are deferred to Wave 7.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETH";

static ra_eth_event_fn_t s_eth_fn;
static void*             s_eth_ctx;

ra_err_t ra_eth_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "eth_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_eswm_regs_t* reg = ra_eswm();
  reg->ESWM_CTRL              = 0U;
  reg->ESWM_STS               = 0U;
  reg->ESWM_IE                = 0U;
  reg->ESWM_ICLR              = 0U;
  ra_log_info(s_tag, "eth_init (ESWM)");
  return k_ra_ok;
}

ra_err_t ra_eth_deinit(void)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  reg->ESWM_CTRL              = 0U;
  reg->ESWM_IE                = 0U;
  s_eth_fn                    = nullptr;
  s_eth_ctx                   = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = ra_eswm()->ESWM_STS;
  return k_ra_ok;
}

ra_err_t ra_eth_clear_status(uint32_t mask)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  reg->ESWM_ICLR              = mask;
  reg->ESWM_STS               = reg->ESWM_STS & ~mask;
  return k_ra_ok;
}

ra_err_t ra_eth_attach_handler(ra_eth_event_fn_t fn, void* ctx)
{
  s_eth_fn  = fn;
  s_eth_ctx = ctx;
  return k_ra_ok;
}

void ra_eth_dispatch(void)
{
  volatile r_eswm_regs_t* reg  = ra_eswm();
  const uint32_t          mask = reg->ESWM_STS;
  const ra_eth_event_fn_t fn   = s_eth_fn;
  void* const             ctx  = s_eth_ctx;
  reg->ESWM_ICLR               = mask;
  reg->ESWM_STS                = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_eth_enter_stop(void)
{
  ra_eswm()->ESWM_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}
