/**
 * @file ra_eth_gwca.c
 * @brief Ethernet CPU Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 GWCA block. Every register access
 * carries a HUM Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth_gwca.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETHGWC";

static ra_eth_gwca_event_fn_t s_gwca_fn;
static void*                  s_gwca_ctx;

/* Implementation of ra_eth_gwca_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "gwca_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_STS  = 0U;
  reg->GWCA_IE   = 0U;
  reg->GWCA_ICLR = 0U;
  ra_log_info(s_tag, "gwca_init");
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_deinit(void)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_CTRL = 0U;
  reg->GWCA_IE   = 0U;
  s_gwca_fn      = nullptr;
  s_gwca_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_gwca_get_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  *out_mask = ra_gwca()->GWCA_STS;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_clear_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_clear_status(uint32_t mask)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  reg->GWCA_ICLR = mask;
  reg->GWCA_STS  = reg->GWCA_STS & ~mask;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_attach_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_attach_handler(ra_eth_gwca_event_fn_t fn, void* ctx)
{
  s_gwca_fn  = fn;
  s_gwca_ctx = ctx;
  return k_ra_ok;
}

/* Implementation of ra_eth_gwca_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_eth_gwca_dispatch(void)
{
  volatile r_gwca_regs_t* reg = ra_gwca();
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  const uint32_t               mask = reg->GWCA_STS;
  const ra_eth_gwca_event_fn_t fn   = s_gwca_fn;
  void* const                  ctx  = s_gwca_ctx;
  reg->GWCA_ICLR                    = mask;
  reg->GWCA_STS                     = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/* Implementation of ra_eth_gwca_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_enter_stop(void)
{
  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  ra_gwca()->GWCA_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_gwca_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_gwca_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}
