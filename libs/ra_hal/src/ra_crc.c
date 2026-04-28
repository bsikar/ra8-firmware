/**
 * @file ra_crc.c
 * @brief Cyclic Redundancy Check driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 CRC block. The CRC unit accepts
 * one of seven hard-wired polynomials selected by CRCCR0.GPS,
 * accumulates the running result in CRCDOR as bytes are written
 * to CRCDIR, and can be reset between operations via the
 * write-only CRCCR0.DORCLR bit. This driver covers init,
 * single-shot compute, runtime polynomial change, status read,
 * reset, and lifecycle. Every register access carries a HUM
 * Ch 48 citation.
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
  k_ra_crccr0_dorclr = 1U << 7, /**< CRCCR0.DORCLR: write-only CRCDOR clear. */
} ra_crccr0_bit_t;

ra_err_t ra_crc_init(ra_crc_poly_t poly)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_crc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "crc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  reg->CRCCR0 = (uint8_t)poly;
  /* HUM Ch 48.2.2 "CRCCR1 : CRC Control Register 1" p 3182 */
  reg->CRCCR1 = 0U;
  ra_log_info_val(s_tag, "crc_init poly", (uint32_t)poly);
  return k_ra_ok;
}

void ra_crc_reset(void)
{
  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- DORCLR
   * is a write-only bit that clears CRCDOR and auto-clears itself. The
   * current GPS/LMS bits are preserved by read-modify-write. */
  reg->CRCCR0 = (uint8_t)(reg->CRCCR0 | k_ra_crccr0_dorclr);
}

ra_err_t ra_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA_CHECK_NULL_PTR(out_crc, s_tag, "out_crc must not be nullptr");

  volatile r_crc_regs_t* reg = ra_crc();
  for (uint32_t i = 0U; i < len; i++) {
    /* HUM Ch 48.2.3 "CRCDIR : CRC Data Input Register" p 3183 */
    reg->CRCDIR = (uint32_t)data[i];
  }
  /* HUM Ch 48.2.4 "CRCDOR : CRC Data Output Register" p 3184 */
  *out_crc = reg->CRCDOR;
  return k_ra_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

ra_err_t ra_crc_deinit(void)
{
  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  reg->CRCCR0 = 0U;
  /* HUM Ch 48.2.2 "CRCCR1 : CRC Control Register 1" p 3182 */
  reg->CRCCR1 = 0U;
  return ra_mstp_disable(k_ra_mstp_crc);
}

ra_err_t ra_crc_set_poly(ra_crc_poly_t poly)
{
  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  reg->CRCCR0 = (uint8_t)poly;
  return k_ra_ok;
}

ra_err_t ra_crc_get_status(uint8_t* out_poly)
{
  RA_CHECK_NULL_PTR(out_poly, s_tag, "out_poly must not be nullptr");
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  *out_poly = ra_crc()->CRCCR0;
  return k_ra_ok;
}

ra_err_t ra_crc_enter_stop(void)
{
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  ra_crc()->CRCCR0 = 0U;
  return ra_mstp_disable(k_ra_mstp_crc);
}

ra_err_t ra_crc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_crc);
}
