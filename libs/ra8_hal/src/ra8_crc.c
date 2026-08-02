/**
 * @file ra8_crc.c
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

#include "ra8_crc.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_crc_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "CRC";

/** @brief Byte-3 shift for little-endian word assembly. */
typedef enum : uint8_t {
  k_crc_shift_byte3 = 24U, /**< CRC shift byte3. */
} crc_shift_t;

/**
 * @enum ra8_crccr0_bit_t
 * @brief Bit positions inside CRCCR0 (HUM Ch 48.2.1 p 3181).
 */
typedef enum : uint8_t {
  k_ra8_crccr0_lms_shift    = 6U,       /**< CRCCR0.LMS bit position.         */
  k_ra8_crccr0_dorclr_shift = 7U,       /**< CRCCR0.DORCLR bit position.      */
  k_ra8_crccr0_dorclr       = 1U << 7U, /**< CRCCR0.DORCLR mask (write-only). */
  k_ra8_crccr0_gps_mask     = 0x07U,    /**< CRCCR0.GPS[2:0] mask.            */
} ra8_crccr0_bit_t;

/**
 * @brief Determine if a polynomial uses 32-bit data input.
 * @param[in] poly Polynomial selection.
 * @return true if `poly` is CRC-32 or CRC-32C (32-bit CRCDIR width).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
static inline bool ra8_crc_is_32bit_poly(ra8_crc_poly_t poly)
{
  return (poly == k_ra8_crc_poly_32_ieee802_3) || (poly == k_ra8_crc_poly_32c_rev);
}

/**
 * @enum ra8_crc_seed_t
 * @brief Standard init / xor-out value for IEEE-style 32-bit CRCs.
 *
 * @details
 * HUM Ch 48 documents the calculator core but not the
 * init / xor-out convention -- those are part of the **CRC variant**
 * spec rather than the hardware. IEEE 802.3 / CRC-32 and Castagnoli /
 * CRC-32C both use ``init = 0xFFFFFFFF`` and ``xor_out = 0xFFFFFFFF``
 * (per ITU-T V.42 / RFC 3385 respectively). The on-chip calculator
 * starts CRCDOR at 0 by default, so the driver must pre-load CRCDOR
 * with the init value before clocking data through, and XOR the read-
 * back with the same constant on the way out.
 */
typedef enum : uint32_t {
  k_ra8_crc_32bit_seed = 0xFFFFFFFFUL, /**< IEEE-802.3 / Castagnoli init = xor-out. */
} ra8_crc_seed_t;

/**
 * @brief Feed `len` bytes through the calculator as packed 32-bit words.
 *
 * @details
 * HUM Ch 48.2.3 p 3182 -- for CRC-32 / CRC-32C the engine consumes
 * 32-bit words. FSP's `R_CRC_Calculate` and the project's mirror loop
 * pack four bytes little-endian into each CRCDIR write. Trailing bytes
 * that don't form a full word are ignored (FSP behaviour and HUM
 * Note 1 p 3180 "This function cannot divide data used in CRC
 * calculations").
 *
 * @param[in] data Pointer to ``len`` bytes (non-NULL).
 * @param[in] len  Byte count; the low two bits are ignored.
 *
 * @pre Driver state has been initialized by ``ra8_crc_init``.
 * @pre CRCDOR pre-seeded by the caller with the desired init value.
 * @post CRCDOR holds the engine result over the consumed words.
 * @post No state mutated besides CRCDIR / CRCDOR.
 * @note Not thread-safe; caller must serialize.
 * @since 0.1.0
 */
static inline void internal_crc_feed_words(const uint8_t* data, uint32_t len)
{
  volatile r_crc_regs_t* reg        = ra8_crc();
  const uint32_t         word_count = len >> 2U;
  for (uint32_t i = 0U; i < word_count; i++) {
    const uint32_t base   = i << 2U;
    const uint32_t packed = (uint32_t)data[base + 0U] | ((uint32_t)data[base + 1U] << 8U) |
                            ((uint32_t)data[base + 2U] << 16U) |
                            ((uint32_t)data[base + 3U] << k_crc_shift_byte3);
    /* HUM Ch 48.2.3 "CRCDIR : CRC Data Input Register" p 3182 */
    reg->CRCDIR = packed;
  }
}

/**
 * @brief Feed `len` bytes through the calculator via the 8-bit alias.
 *
 * @details
 * HUM Ch 48.2.3 p 3182 -- CRC-8 / CRC-16 / CRC-CCITT consume one byte
 * per write through ``CRCDIR_BY`` at offset +0x04.
 *
 * @param[in] data Pointer to ``len`` bytes (non-NULL).
 * @param[in] len  Byte count.
 *
 * @pre Driver state has been initialized by ``ra8_crc_init``.
 * @pre CRCDOR pre-seeded with the desired init value (typically 0).
 * @post CRCDOR holds the engine result over all bytes.
 * @post No state mutated besides CRCDIR_BY / CRCDOR.
 * @note Not thread-safe; caller must serialize.
 * @since 0.1.0
 */
static inline void internal_crc_feed_bytes(const uint8_t* data, uint32_t len)
{
  volatile r_crc_regs_t* reg = ra8_crc();
  for (uint32_t i = 0U; i < len; i++) {
    /* HUM Ch 48.2.3 "CRCDIR_BY : CRC Data Input Register" p 3182 */
    reg->CRCDIR_BY = data[i];
  }
}

ra8_err_t ra8_crc_init(ra8_crc_poly_t poly)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_crc);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "crc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_crc_regs_t* reg    = ra8_crc();
  const uint8_t          crccr0 = (uint8_t)((uint8_t)poly | k_ra8_crccr0_dorclr);
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181.
   * Mirror FSP `R_CRC_Open`: write GPS plus DORCLR=1 in a single
   * store so any stale CRCDOR value from a prior session is cleared
   * atomically with the polynomial select. LMS defaults to 0
   * (LSB-first); use ra8_crc_set_bit_order() to change. */
  reg->CRCCR0 = crccr0;
  /* HUM Ch 48.2.2 "CRCCR1 : CRC Control Register 1" p 3182 */ /* snoop off. */
  reg->CRCCR1 = 0U;
  ra8_log_info_val(s_tag, "crc_init poly", (uint32_t)poly);
  return k_ra8_ok;
}

void ra8_crc_reset(void)
{
  volatile r_crc_regs_t* reg = ra8_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- DORCLR
   * is a write-only bit that clears CRCDOR and auto-clears itself on
   * the real chip. We read-modify-write so the GPS/LMS bits are
   * preserved. */
  reg->CRCCR0 = (uint8_t)(reg->CRCCR0 | k_ra8_crccr0_dorclr);
}

ra8_err_t ra8_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA8_CHECK_NULL_PTR(out_crc, s_tag, "out_crc must not be nullptr");

  volatile r_crc_regs_t* reg = ra8_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- GPS lives
   * in the low 3 bits of CRCCR0. */
  const ra8_crc_poly_t poly      = (ra8_crc_poly_t)(reg->CRCCR0 & k_ra8_crccr0_gps_mask);
  const bool           is_32_bit = ra8_crc_is_32bit_poly(poly);

  /* HUM Ch 48.2.4 "CRCDOR : CRC Data Output Register" p 3182 documents
   * CRCDOR as a 32-bit read/write register; "Because its initial value
   * is 0x00000000, rewrite the CRCDOR ... register to perform the
   * calculations using a value other than the initial value." Standard
   * CRC-32 / CRC-32C use init = 0xFFFFFFFF and xor-out = 0xFFFFFFFF; the
   * hardware applies neither, so the driver pre-seeds CRCDOR and XORs
   * the readback on the way out for the 32-bit polynomials. CRC-8 / 16
   * / CCITT keep the chip's natural init = 0 (HUM example p 3185 shows
   * the same flow). */
  if (is_32_bit) {
    /* HUM Ch 48.2.4 "CRCDOR : CRC Data Output Register" p 3182 -- pre-seed
     * CRCDOR with the CRC-32 init value before clocking data through (see
     * the decision comment above for the init / xor-out rationale). */
    reg->CRCDOR = (uint32_t)k_ra8_crc_32bit_seed;
    internal_crc_feed_words(data, len);
    /* HUM Ch 48.2.4 "CRCDOR : CRC Data Output Register" p 3182 -- read the
     * running result back and apply the CRC-32 xor-out. */
    *out_crc = reg->CRCDOR ^ (uint32_t)k_ra8_crc_32bit_seed;
    return k_ra8_ok;
  }

  internal_crc_feed_bytes(data, len);
  /* HUM Ch 48.2.4 p 3182 -- the full 32-bit register holds the running
   * result; lower bits mirror `CRCDOR_HA` / `CRCDOR_BY` aliases.
   * Reading the wide register matches FSP `crc_calculated_value_get`
   * and is a superset of the narrower aliases. */
  *out_crc = reg->CRCDOR;
  return k_ra8_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

ra8_err_t ra8_crc_deinit(void)
{
  volatile r_crc_regs_t* reg = ra8_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  reg->CRCCR0 = 0U;
  /* HUM Ch 48.2.2 "CRCCR1 : CRC Control Register 1" p 3182 */
  reg->CRCCR1 = 0U;
  return ra8_mstp_disable(k_ra8_mstp_crc);
}

ra8_err_t ra8_crc_set_poly(ra8_crc_poly_t poly)
{
  volatile r_crc_regs_t* reg = ra8_crc();
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 -- pulse
   * DORCLR alongside the new GPS so the running result doesn't bleed
   * into the next polynomial's calculation (mirrors FSP Open). */
  reg->CRCCR0 = (uint8_t)((uint8_t)poly | k_ra8_crccr0_dorclr);
  return k_ra8_ok;
}

ra8_err_t ra8_crc_get_status(uint8_t* out_poly)
{
  RA8_CHECK_NULL_PTR(out_poly, s_tag, "out_poly must not be nullptr");
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  *out_poly = ra8_crc()->CRCCR0;
  return k_ra8_ok;
}

ra8_err_t ra8_crc_enter_stop(void)
{
  /* HUM Ch 48.2.1 "CRCCR0 : CRC Control Register 0" p 3181 */
  ra8_crc()->CRCCR0 = 0U;
  return ra8_mstp_disable(k_ra8_mstp_crc);
}

ra8_err_t ra8_crc_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_crc);
}
