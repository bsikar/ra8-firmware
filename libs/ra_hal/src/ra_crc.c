/**
 * @file ra_crc.c
 * @brief Cyclic Redundancy Check driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 CRC block. The CRC unit accepts one of five
 * hard-wired polynomials (CRC-8, CRC-16, CRC-CCITT, CRC-32 IEEE
 * 802.3, CRC-32C Castagnoli) selected by `CRCCR0.GPS[2:0]`,
 * accumulates the running result in `CRCDOR` as data is written to
 * `CRCDIR`/`CRCDIR_BY`, and can be reset between operations via the
 * write-only `CRCCR0.DORCLR` bit.
 *
 * Cross-verified against FSP `r_crc.c` (`R_CRC_Open`,
 * `R_CRC_Calculate`, `R_CRC_Close`) at `_reference/fsp/ra/fsp/src/
 * r_crc/r_crc.c`. Like FSP, the driver pulses DORCLR during init to
 * clear stale CRCDOR contents (`crccr0 |= 1 << R_CRC_CRCCR0_DORCLR_Pos`
 * in `R_CRC_Open`) and uses byte-access `CRCDIR_BY` for 8/16/CCITT
 * polynomials and 32-bit `CRCDIR` for 32-bit polynomials. Every
 * register access carries a HUM Ch 48 citation (p 3180-3189).
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

/**
 * @enum ra_crccr0_bit_t
 * @brief Bit positions inside CRCCR0 (HUM Ch 48.2.1 p 3181).
 */
typedef enum : uint8_t {
  k_ra_crccr0_lms_shift    = 6U,       /**< CRCCR0.LMS bit position.            */
  k_ra_crccr0_dorclr_shift = 7U,       /**< CRCCR0.DORCLR bit position.         */
  k_ra_crccr0_dorclr       = 1U << 7U, /**< CRCCR0.DORCLR mask (write-only).    */
  k_ra_crccr0_gps_mask     = 0x07U,    /**< CRCCR0.GPS[2:0] mask.               */
} ra_crccr0_bit_t;

/**
 * @brief Determine if a polynomial uses 32-bit data input.
 * @param[in] poly Polynomial selection.
 * @return true if `poly` is CRC-32 or CRC-32C (32-bit CRCDIR width).
 */
static inline bool ra_crc_is_32bit_poly(ra_crc_poly_t poly)
{
  return (poly == k_ra_crc_poly_32_ieee802_3) || (poly == k_ra_crc_poly_32c_rev);
}

ra_err_t ra_crc_init(ra_crc_poly_t poly)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_crc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "crc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181.
   * Mirror FSP `R_CRC_Open`: write GPS plus DORCLR=1 in a single
   * store so any stale CRCDOR value from a prior session is cleared
   * atomically with the polynomial select. LMS defaults to 0
   * (LSB-first); use ra_crc_set_bit_order() to change. */
  const uint8_t crccr0 = (uint8_t)((uint8_t)poly | k_ra_crccr0_dorclr);
  reg->CRCCR0          = crccr0;
  /* HUM Ch 48.2.2 "CRCCR1 : CRC Control Register 1" p 3182 */ /* snoop off. */
  reg->CRCCR1 = 0U;
  ra_log_info_val(s_tag, "crc_init poly", (uint32_t)poly);
  return k_ra_ok;
}

void ra_crc_reset(void)
{
  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- DORCLR
   * is a write-only bit that clears CRCDOR and auto-clears itself on
   * the real chip. We read-modify-write so the GPS/LMS bits are
   * preserved. */
  reg->CRCCR0 = (uint8_t)(reg->CRCCR0 | k_ra_crccr0_dorclr);
}

ra_err_t ra_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA_CHECK_NULL_PTR(out_crc, s_tag, "out_crc must not be nullptr");

  volatile r_crc_regs_t* reg = ra_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  /* GPS lives in the low 3 bits of CRCCR0. */
  const ra_crc_poly_t poly = (ra_crc_poly_t)(reg->CRCCR0 & k_ra_crccr0_gps_mask);

  if (ra_crc_is_32bit_poly(poly)) {
    /* HUM Ch 48.2.3 "CRCDIR : CRC Data Input Register" p 3183 -- for
     * CRC-32/32C the calculator consumes 32-bit words. FSP requires
     * 4-byte-aligned length (R_CRC_Calculate cfg-check). We mirror
     * that by ignoring trailing bytes that don't form a full word. */
    const uint32_t word_count = len >> 2U;
    for (uint32_t i = 0U; i < word_count; i++) {
      const uint32_t base   = i << 2U;
      const uint32_t packed = (uint32_t)data[base + 0U] | ((uint32_t)data[base + 1U] << 8U) |
                              ((uint32_t)data[base + 2U] << 16U) |
                              ((uint32_t)data[base + 3U] << 24U);
      reg->CRCDIR           = packed;
    }
  } else {
    /* HUM Ch 48.2.3 p 3183, FSP `crc_calculate_polynomial` -- CRC-8,
     * CRC-16 and CRC-CCITT consume one byte per write through the
     * `CRCDIR_BY` 8-bit alias at +0x04. */
    for (uint32_t i = 0U; i < len; i++) {
      reg->CRCDIR_BY = data[i];
    }
  }

  /* HUM Ch 48.2.4 "CRCDOR : CRC Data Output Register" p 3184 -- the
   * full 32-bit register holds the running result; lower bits mirror
   * `CRCDOR_HA`/`CRCDOR_BY` aliases. Reading the wide register
   * matches FSP `crc_calculated_value_get` for the 32-bit polynomial
   * case and is a superset of the narrower aliases. */
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
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- pulse
   * DORCLR alongside the new GPS so the running result doesn't bleed
   * into the next polynomial's calculation (mirrors FSP Open). */
  reg->CRCCR0 = (uint8_t)((uint8_t)poly | k_ra_crccr0_dorclr);
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
