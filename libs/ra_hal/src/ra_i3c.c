/**
 * @file ra_i3c.c
 * @brief I3C Bus Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 5 driver for the RA8D2 I3C controller. Programmes the
 * baseline protocol/bus control register set and exposes the
 * lifecycle + status + IRQ + power-transition surface. The
 * higher-level CCC / IBI / HDR-DDR transfer engines are
 * intentionally left to the first card-stack consumer because
 * the protocol decisions (broadcast vs. directed CCC, target
 * address negotiation, in-band IRQ arbitration) belong to the
 * application stack. Every register access carries a HUM Ch 40
 * citation.
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
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  /* Reset every control + status register to a known baseline. */
  reg->PRTS  = 0U;
  reg->BCTL  = 0U;
  reg->INST  = 0U;
  reg->INSTE = 0U;
  reg->IE    = 0U;
  reg->BST   = 0U;
  reg->BSTE  = 0U;
  reg->BIE   = 0U;
  ra_log_info(s_tag, "i3c_init");
  return k_ra_ok;
}

ra_err_t ra_i3c_deinit(void)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  reg->IE   = 0U;
  reg->BIE  = 0U;
  reg->BCTL = 0U;
  s_i3c_fn  = nullptr;
  s_i3c_ctx = nullptr;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_set_address(uint32_t addr)
{
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  /* Master / slave device address register holds the active 7-bit. */
  ra_i3c()->MSDVAD = addr;
  return k_ra_ok;
}

ra_err_t ra_i3c_bus_enable(bool enable)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  /* BCTL.BUSE (bit 0) gates bus master operation. */
  if (enable) {
    reg->BCTL = reg->BCTL | 1UL;
  } else {
    reg->BCTL = reg->BCTL & ~1UL;
  }
  return k_ra_ok;
}

ra_err_t ra_i3c_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  *out_mask = ra_i3c()->INST;
  return k_ra_ok;
}

ra_err_t ra_i3c_clear_status(uint32_t mask)
{
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  reg->INST = reg->INST & ~mask;
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
  volatile r_i3c_regs_t* reg = ra_i3c();
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
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
  /* HUM Ch 40 "I3C Bus Interface (I3C)" p 2445 */
  ra_i3c()->BCTL = 0U;
  return ra_mstp_disable(k_ra_mstp_i3c);
}

ra_err_t ra_i3c_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_i3c);
}
