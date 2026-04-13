/**
 * @file ra_eth_gptp.c
 * @brief Ethernet Generic PTP Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 6 driver for the RA8D2 GPTP block. Every register access
 * carries a HUM Ch 35 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth_gptp.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETHGPT";

static ra_eth_gptp_event_fn_t s_gptp_fn;
static void*                  s_gptp_ctx;

ra_err_t ra_eth_gptp_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gptp_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_gptp_regs_t* reg = ra_gptp();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->GPTP_CTRL = 0U;
  reg->GPTP_STS  = 0U;
  reg->GPTP_IE   = 0U;
  reg->GPTP_ICLR = 0U;
  ra_log_info(s_tag, "gptp_init");
  return k_ra_ok;
}

ra_err_t ra_eth_gptp_deinit(void)
{
  volatile r_gptp_regs_t* reg = ra_gptp();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->GPTP_CTRL = 0U;
  reg->GPTP_IE   = 0U;
  s_gptp_fn      = nullptr;
  s_gptp_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_gptp_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  *out_mask = ra_gptp()->GPTP_STS;
  return k_ra_ok;
}

ra_err_t ra_eth_gptp_clear_status(uint32_t mask)
{
  volatile r_gptp_regs_t* reg = ra_gptp();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->GPTP_ICLR = mask;
  reg->GPTP_STS  = reg->GPTP_STS & ~mask;
  return k_ra_ok;
}

ra_err_t ra_eth_gptp_attach_handler(ra_eth_gptp_event_fn_t fn, void* ctx)
{
  s_gptp_fn  = fn;
  s_gptp_ctx = ctx;
  return k_ra_ok;
}

void ra_eth_gptp_dispatch(void)
{
  volatile r_gptp_regs_t* reg = ra_gptp();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  const uint32_t               mask = reg->GPTP_STS;
  const ra_eth_gptp_event_fn_t fn   = s_gptp_fn;
  void* const                  ctx  = s_gptp_ctx;
  reg->GPTP_ICLR                    = mask;
  reg->GPTP_STS                     = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_eth_gptp_enter_stop(void)
{
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  ra_gptp()->GPTP_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_gptp_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}
