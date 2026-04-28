/**
 * @file ra_eth_mfwd.c
 * @brief Ethernet Message Forwarding Engine driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 MFWD block. The MFWD shares the
 * Ethernet Switch MSTP gate (ESWM) so the lifecycle calls below
 * piggyback on ``k_ra_mstp_eswm`` rather than a per-block bit.
 * Every register access carries a HUM Ch 30 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth_mfwd.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETHMFW";

static ra_eth_mfwd_event_fn_t s_mfwd_fn;
static void*                  s_mfwd_ctx;

ra_err_t ra_eth_mfwd_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "mfwd_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_mfwd_regs_t* reg = ra_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_CTRL = 0U;
  reg->MFWD_STS  = 0U;
  reg->MFWD_IE   = 0U;
  reg->MFWD_ICLR = 0U;
  ra_log_info(s_tag, "mfwd_init");
  return k_ra_ok;
}

ra_err_t ra_eth_mfwd_deinit(void)
{
  volatile r_mfwd_regs_t* reg = ra_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_CTRL = 0U;
  reg->MFWD_IE   = 0U;
  s_mfwd_fn      = nullptr;
  s_mfwd_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_mfwd_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  *out_mask = ra_mfwd()->MFWD_STS;
  return k_ra_ok;
}

ra_err_t ra_eth_mfwd_clear_status(uint32_t mask)
{
  volatile r_mfwd_regs_t* reg = ra_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  reg->MFWD_ICLR = mask;
  reg->MFWD_STS  = reg->MFWD_STS & ~mask;
  return k_ra_ok;
}

ra_err_t ra_eth_mfwd_attach_handler(ra_eth_mfwd_event_fn_t fn, void* ctx)
{
  s_mfwd_fn  = fn;
  s_mfwd_ctx = ctx;
  return k_ra_ok;
}

void ra_eth_mfwd_dispatch(void)
{
  volatile r_mfwd_regs_t* reg = ra_mfwd();
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  const uint32_t               mask = reg->MFWD_STS;
  const ra_eth_mfwd_event_fn_t fn   = s_mfwd_fn;
  void* const                  ctx  = s_mfwd_ctx;
  reg->MFWD_ICLR                    = mask;
  reg->MFWD_STS                     = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_eth_mfwd_enter_stop(void)
{
  /* HUM Ch 30 "Ethernet Message Forwarding Engine (MFWD)" p 1321 */
  ra_mfwd()->MFWD_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_mfwd_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}
