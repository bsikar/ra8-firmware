/**
 * @file ra_cac.c
 * @brief Clock Accuracy Check driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_cac.h"

#include <stdint.h>

#include "ra8d2_cac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "CAC";

typedef enum : uint8_t {
  k_ra_cacr0_cfme  = 0U, /**< CFME: clock frequency measurement en.*/
  k_ra_castr_mendf = 1U, /**< Measurement end flag.                */
} ra_cac_bit_t;

ra_err_t ra_cac_init(uint16_t upper, uint16_t lower)
{
  volatile r_cac_regs_t* reg = ra_cac();
  reg->CACR0                 = 0U;
  reg->CACR1                 = 0U;
  reg->CACR2                 = 0U;
  reg->CAULVR                = upper;
  reg->CALLVR                = lower;
  ra_log_info(s_tag, "cac_init");
  return k_ra_ok;
}

ra_err_t ra_cac_measure(uint16_t* out_count)
{
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  volatile r_cac_regs_t* reg = ra_cac();

  reg->CACR0 = (uint8_t)(1U << k_ra_cacr0_cfme);

  enum : uint32_t { k_ra_cac_poll_limit = 200000U };
  for (uint32_t i = 0U; i < k_ra_cac_poll_limit; i++) {
    if ((reg->CASTR & (uint8_t)(1U << k_ra_castr_mendf)) != 0U) {
      *out_count = reg->CACNTBR;
      reg->CACR0 = 0U;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}
