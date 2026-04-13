/**
 * @file ra_dtc.c
 * @brief Data Transfer Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Wave 3 driver for the RA8D2 DTC block. Every register access
 * carries a HUM Ch 18 citation. Shares MSTPA22 with DMAC0 via
 * ra_mstp's reference counter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dtc.h"

#include <stdint.h>

#include "ra8d2_dtc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "DTC";

typedef enum : uint8_t {
  k_ra_dtcst_enable = 1U,       /**< DTCST.DTCST (bit 0).        */
  k_ra_dtccr_rrs    = 1U << 4U, /**< DTCCR.RRS (read-skip en). */
} ra_dtc_bit_t;

static ra_dtc_event_fn_t s_dtc_fn;
static void*             s_dtc_ctx;

ra_err_t ra_dtc_init(void* vector_base)
{
  RA_CHECK_NULL_PTR(vector_base, s_tag, "vector_base must not be nullptr");

  /* DTC0 + DMAC0 share MSTPA22; ra_mstp keeps the ref count so
   * a follow-up ra_dmac_start does not flip the bit again.
   * HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A" p 443 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dtc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_dtc_regs_t* reg = ra_dtc();
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  reg->DTCCR  = 0U;
  reg->DTCVBR = (uint32_t)(uintptr_t)vector_base;
  reg->DTCST  = 0U;

  ra_log_info(s_tag, "dtc_init");
  return k_ra_ok;
}

ra_err_t ra_dtc_deinit(void)
{
  volatile r_dtc_regs_t* reg = ra_dtc();
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  reg->DTCST  = 0U;
  reg->DTCCR  = 0U;
  reg->DTCVBR = 0U;
  s_dtc_fn    = nullptr;
  s_dtc_ctx   = nullptr;
  return ra_mstp_disable(k_ra_mstp_dmac0_dtc0);
}

ra_err_t ra_dtc_enable(void)
{
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  ra_dtc()->DTCST = (uint8_t)k_ra_dtcst_enable;
  return k_ra_ok;
}

ra_err_t ra_dtc_disable(void)
{
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  ra_dtc()->DTCST = 0U;
  return k_ra_ok;
}

ra_err_t ra_dtc_reconfigure(void* vector_base)
{
  RA_CHECK_NULL_PTR(vector_base, s_tag, "vector_base must not be nullptr");
  volatile r_dtc_regs_t* reg = ra_dtc();
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  reg->DTCST  = 0U;
  reg->DTCVBR = (uint32_t)(uintptr_t)vector_base;
  reg->DTCCR  = (uint8_t)k_ra_dtccr_rrs;
  reg->DTCCR  = 0U;
  return k_ra_ok;
}

ra_err_t ra_dtc_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  *out_mask = ra_dtc()->DTCSTS;
  return k_ra_ok;
}

ra_err_t ra_dtc_clear_status(uint16_t mask)
{
  volatile r_dtc_regs_t* reg = ra_dtc();
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  reg->DTCSTS = (uint16_t)(reg->DTCSTS & ~mask);
  return k_ra_ok;
}

ra_err_t ra_dtc_attach_handler(ra_dtc_event_fn_t fn, void* ctx)
{
  s_dtc_fn  = fn;
  s_dtc_ctx = ctx;
  return k_ra_ok;
}

void ra_dtc_dispatch(void)
{
  volatile r_dtc_regs_t* reg = ra_dtc();
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  const uint16_t          mask = reg->DTCSTS;
  const ra_dtc_event_fn_t fn   = s_dtc_fn;
  void* const             ctx  = s_dtc_ctx;
  reg->DTCSTS                  = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_dtc_enter_stop(void)
{
  /* HUM Ch 18 "Data Transfer Controller (DTC)" p 784 */
  ra_dtc()->DTCST = 0U;
  return ra_mstp_disable(k_ra_mstp_dmac0_dtc0);
}

ra_err_t ra_dtc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
}
