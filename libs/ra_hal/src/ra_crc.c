/**
 * @file ra_crc.c
 * @brief CRC calculator driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_crc.h"

#include <stdint.h>

#include "ra8d2_crc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "CRC";

typedef enum : uint8_t {
  k_ra_crccr1_clr = 1U << 7, /**< CRCCR1.CRCSWR: software reset bit. */
} ra_crccr1_bit_t;

ra_err_t ra_crc_init(ra_crc_poly_t poly)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_crc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "crc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_crc_regs_t* reg = ra_crc();
  reg->CRCCR0                = (uint8_t)poly;
  reg->CRCCR1                = 0U;
  ra_log_info_val(s_tag, "crc_init poly", (uint32_t)poly);
  return k_ra_ok;
}

void ra_crc_reset(void)
{
  volatile r_crc_regs_t* reg = ra_crc();
  reg->CRCCR1                = (uint8_t)k_ra_crccr1_clr;
  reg->CRCCR1                = 0U;
}

ra_err_t ra_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA_CHECK_NULL_PTR(out_crc, s_tag, "out_crc must not be nullptr");

  volatile r_crc_regs_t* reg = ra_crc();
  for (uint32_t i = 0U; i < len; i++) {
    reg->CRCDIR = (uint32_t)data[i];
  }
  *out_crc = reg->CRCDOR;
  return k_ra_ok;
}
