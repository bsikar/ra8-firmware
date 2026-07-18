/**
 * @file ra8_eth_coma.c
 * @brief Ethernet Common Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 COMA block. Shares the ESWM MSTP
 * gate with the rest of the ethernet subsystem. Every register
 * access carries a HUM Ch 31 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_eth_coma.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

static const char* s_tag = "ETHCMA";

static ra8_eth_coma_event_fn_t s_coma_fn;
static void*                   s_coma_ctx;

ra8_err_t ra8_eth_coma_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "coma_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_STS  = 0U;
  reg->COMA_IE   = 0U;
  reg->COMA_ICLR = 0U;
  ra8_log_info(s_tag, "coma_init");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_deinit(void)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_IE   = 0U;
  s_coma_fn      = nullptr;
  s_coma_ctx     = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_coma_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *out_mask = ra8_coma()->COMA_STS;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_clear_status(uint32_t mask)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_ICLR = mask;
  reg->COMA_STS  = reg->COMA_STS & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_coma_attach_handler(ra8_eth_coma_event_fn_t fn, void* ctx)
{
  s_coma_fn  = fn;
  s_coma_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_eth_coma_dispatch(void)
{
  volatile r_coma_regs_t* reg = ra8_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  const uint32_t                mask = reg->COMA_STS;
  const ra8_eth_coma_event_fn_t fn   = s_coma_fn;
  void* const                   ctx  = s_coma_ctx;
  reg->COMA_ICLR                     = mask;
  reg->COMA_STS                      = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_eth_coma_enter_stop(void)
{
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  ra8_coma()->COMA_CTRL = 0U;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_coma_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_eswm);
}
