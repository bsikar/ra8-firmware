/**
 * @file ra_dtc.c
 * @brief DTC driver implementation
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
  k_ra_dtcst_enable = 1U, /**< DTCST.DTCST (bit 0). */
} ra_dtcst_bit_t;

ra_err_t ra_dtc_init(void* vector_base)
{
  RA_CHECK_NULL_PTR(vector_base, s_tag, "vector_base must not be nullptr");

  /* DTC0 + DMAC0 share MSTPA22; ra_mstp keeps the ref count so
   * a follow-up ra_dmac_start does not flip the bit again.
   * HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dtc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_dtc_regs_t* reg = ra_dtc();

  reg->DTCCR  = 0U;
  reg->DTCVBR = (uint32_t)(uintptr_t)vector_base;
  reg->DTCST  = 0U;

  ra_log_info(s_tag, "dtc_init");
  return k_ra_ok;
}

ra_err_t ra_dtc_enable(void)
{
  ra_dtc()->DTCST = (uint8_t)k_ra_dtcst_enable;
  return k_ra_ok;
}

ra_err_t ra_dtc_disable(void)
{
  ra_dtc()->DTCST = 0U;
  return k_ra_ok;
}
