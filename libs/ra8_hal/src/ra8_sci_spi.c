/**
 * @file ra8_sci_spi.c
 * @brief SCI Simple-SPI controller-mode driver (polling, full-duplex)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements ``ra8_sci_spi.h`` against the RA8D2 SCI_B peripheral in
 * Simple-SPI mode (``CCR3.MOD = 011b``). The flow mirrors FSP
 * ``r_sci_b_spi.c``: clear CCR0 (TE/RE off), programme CCR3
 * (mode/CHR/CPOL/CPHA/LSBF), CCR2 (CKS + BRR for the bit-rate), clear
 * stale CSR latches, then set CCR0 = TE | RE. A frame is a write to
 * TDR followed by a poll of CSR.TDRE then CSR.RDRF, then a read of
 * RDR -- in clock-synchronous / Simple-SPI mode every transmitted
 * frame clocks in a received frame, so the controller is inherently
 * full-duplex.
 *
 * The chip-select is the caller's responsibility (held as a GPIO so
 * it can span a multi-frame SD transaction); this driver never
 * touches SSn (CCR0.SSE stays 0).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sci_spi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_sci_regs.h"

static const char* s_tag = "SCI_SPI";

/**
 * @enum ra8_sci_spi_dim_t
 * @brief Static dimensioning + bit-rate search constants.
 */
typedef enum : uint32_t {
  k_ra8_sci_spi_channel_count = 10U,   /**< SCI0..SCI9.                     */
  k_ra8_sci_spi_cks_max       = 3U,    /**< CKS[1:0] max (clock /64).       */
  k_ra8_sci_spi_brr_max       = 255U,  /**< BRR is 8-bit.                   */
  k_ra8_sci_spi_div_base      = 4U,    /**< B = TCLK / (4 * 4^n * (N + 1)). */
  k_ra8_sci_spi_idle_byte     = 0xFFU, /**< Idle fill when ``tx`` is NULL.  */
} ra8_sci_spi_dim_t;

/**
 * @var s_mstp_table
 * @brief Channel-index -> MSTP id (HUM Ch 11 "MSTPCRB").
 */
static const ra8_mstp_t s_mstp_table[k_ra8_sci_spi_channel_count] = {
  k_ra8_mstp_sci0,
  k_ra8_mstp_sci1,
  k_ra8_mstp_sci2,
  k_ra8_mstp_sci3,
  k_ra8_mstp_sci4,
  k_ra8_mstp_sci5,
  k_ra8_mstp_sci6,
  k_ra8_mstp_sci7,
  k_ra8_mstp_sci8,
  k_ra8_mstp_sci9,
};

/* =============================================================================
 * Bit-rate helper
 * =============================================================================
 */

/**
 * @brief Resolve CCR2.CKS + BRR for a requested Simple-SPI bit-rate.
 *
 * @details
 * Clock-synchronous / Simple-SPI rate (HUM Table 38.7, BGDM = 0):
 * ``B = TCLK / (4 * 4^n * (N + 1))`` with ``n = CKS`` and
 * ``N = BRR``. Picks the smallest ``n`` that keeps ``N <= 255``;
 * clamps to the slowest setting when the rate is unreachably low.
 *
 * @param[in]  baud_hz Desired bit-rate (Hz), already checked non-zero.
 * @param[in]  pclk_hz Baud-clock source (Hz), already checked non-zero.
 * @param[out] out_cks Resolved CKS value (0..3).
 * @param[out] out_brr Resolved BRR value (0..255).
 *
 * @pre ``baud_hz`` and ``pclk_hz`` are non-zero.
 * @pre ``out_cks`` and ``out_brr`` are non-NULL.
 * @post ``*out_cks`` / ``*out_brr`` hold the closest reachable rate.
 * @post No register is written; the caller applies the result.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_resolve_rate(uint32_t baud_hz, uint32_t pclk_hz, uint32_t* out_cks, uint32_t* out_brr)
{
  for (uint32_t cks = 0U; cks <= (uint32_t)k_ra8_sci_spi_cks_max; cks++) {
    const uint32_t divisor = (uint32_t)k_ra8_sci_spi_div_base << (2U * cks);
    const uint32_t denom   = divisor * baud_hz;
    const uint32_t q       = pclk_hz / denom;
    if (q == 0U) {
      /* Requested rate exceeds this CKS ceiling -> fastest (BRR = 0). */
      *out_cks = cks;
      *out_brr = 0U;
      return;
    }
    if ((q - 1U) <= (uint32_t)k_ra8_sci_spi_brr_max) {
      *out_cks = cks;
      *out_brr = q - 1U;
      return;
    }
  }
  /* Unreachably slow: clamp to the slowest divider. */
  *out_cks = (uint32_t)k_ra8_sci_spi_cks_max;
  *out_brr = (uint32_t)k_ra8_sci_spi_brr_max;
}

/**
 * @brief Write CCR2 with the resolved CKS + BRR fields.
 *
 * @details Resolves the divider via ::internal_resolve_rate and stores it in
 * CCR2.CKS / CCR2.BRR, preserving MDDR at its reset value (modulation off).
 *
 * @param[in] reg     Channel register block.
 * @param[in] baud_hz Desired bit-rate (Hz).
 * @param[in] pclk_hz Baud-clock source (Hz).
 *
 * @pre TE = RE = 0 (CCR2 written only while disabled or via set_clock).
 * @pre ``reg`` is a valid SCI channel register block.
 * @post CCR2.CKS / CCR2.BRR encode the closest reachable rate.
 * @post No other CCR2 field is disturbed beyond MDDR's reset value.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_apply_rate(volatile r_sci_regs_t* reg, uint32_t baud_hz, uint32_t pclk_hz)
{
  uint32_t cks = 0U;
  uint32_t brr = 0U;
  internal_resolve_rate(baud_hz, pclk_hz, &cks, &brr);
  /* HUM Ch 38.2.7 "CCR2 : Common Control Register 2" p 2189 */
  uint32_t ccr2 =
    (brr << (uint32_t)k_ra8_sci_ccr2_shift_brr) & (uint32_t)k_ra8_sci_ccr2_mask_brr_field;
  ccr2 |= (cks << (uint32_t)k_ra8_sci_ccr2_shift_cks) & (uint32_t)k_ra8_sci_ccr2_mask_cks_field;
  ccr2 |= ((uint32_t)k_ra8_sci_mddr_default << (uint32_t)k_ra8_sci_ccr2_shift_mddr);
  reg->CCR2 = ccr2;
}

/**
 * @brief Build CCR3 for Simple-SPI controller mode.
 *
 * @details MOD = Simple SPI, CHR = 8-bit, CKE = 00 (internal clock,
 * SCKn drives the bus), BPEN = 0 (input synchroniser active). CPOL /
 * CPHA come from the SPI mode; LSBF from the config.
 *
 * @param[in] cfg Caller config (already null-checked).
 * @return CCR3 register value.
 * @retval non-zero The assembled CCR3 word (MOD field is always set).
 * @pre ``cfg`` is non-NULL.
 * @pre ``cfg->mode`` is one of ::ra8_spi_mode_t (0..3).
 * @post No register is written; the value is returned to the caller.
 * @post ``cfg`` is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_ccr3(const ra8_sci_spi_cfg_t* cfg)
{
  uint32_t ccr3 = ((uint32_t)k_ra8_sci_ccr3_mod_simple_spi << (uint32_t)k_ra8_sci_ccr3_shift_mod);
  ccr3 |= ((uint32_t)k_ra8_sci_ccr3_chr_8bit << (uint32_t)k_ra8_sci_ccr3_shift_chr);
  /* ra8_spi_mode_t is 0..3 where bit 1 = CPOL and bit 0 = CPHA. */
  if (((uint32_t)cfg->mode & (uint32_t)k_ra8_spi_mode_2) != 0U) {
    ccr3 |= (1U << (uint32_t)k_ra8_sci_ccr3_bit_cpol);
  }
  if (((uint32_t)cfg->mode & (uint32_t)k_ra8_spi_mode_1) != 0U) {
    ccr3 |= (1U << (uint32_t)k_ra8_sci_ccr3_bit_cpha);
  }
  if (cfg->lsb_first) {
    ccr3 |= (1U << (uint32_t)k_ra8_sci_ccr3_bit_lsbf);
  }
  return ccr3;
}

ra8_err_t ra8_sci_spi_init(uint8_t channel, const ra8_sci_spi_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  if (channel >= (uint8_t)k_ra8_sci_spi_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->baud_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  const ra8_err_t mst_err = ra8_mstp_enable(s_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "spi_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 */
  reg->CCR0 = 0U;
  reg->CCR1 = 0U;
  reg->CCR3 = internal_ccr3(cfg);
  internal_apply_rate(reg, cfg->baud_hz, cfg->pclk_hz);
  reg->CCR4 = 0U;
  /* HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register" p 2238 */
  reg->CFCLR = (uint32_t)k_ra8_sci_cfclr_default;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 */
  reg->CCR0 = (1U << (uint32_t)k_ra8_sci_ccr0_bit_te) | (1U << (uint32_t)k_ra8_sci_ccr0_bit_re);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_spi_deinit(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra8_sci_spi_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0" p 2182 */
  reg->CCR0 = 0U;
  return ra8_mstp_disable(s_mstp_table[channel]);
}

ra8_err_t ra8_sci_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclk_hz)
{
  if (channel >= (uint8_t)k_ra8_sci_spi_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (baud_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* CCR2 (CKS/BRR) is writable while enabled; TE/RE stay set. */
  const uint32_t saved_ccr0 = reg->CCR0;
  reg->CCR0                 = 0U;
  internal_apply_rate(reg, baud_hz, pclk_hz);
  reg->CCR0 = saved_ccr0;
  return k_ra8_ok;
}

ra8_err_t ra8_sci_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "spi_xfer8: channel out of range");

  /* HUM Ch 38.2.17 "CSR : Common Status Register" p 2225 */
  const uint32_t tdre = (1U << (uint32_t)k_ra8_sci_csr_bit_tdre);
  ra8_err_t      err  = ra8_hw_wait_flag_set32(&reg->CSR, tdre, k_ra8_hw_budget_long);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 38.2.3 "TDR : Transmit Data Register" p 2181 */
  reg->TDR = (uint32_t)tx;

  /* HUM Ch 38.2.17 "CSR : Common Status Register" p 2225 */
  const uint32_t rdrf = (1U << (uint32_t)k_ra8_sci_csr_bit_rdrf);
  err                 = ra8_hw_wait_flag_set32(&reg->CSR, rdrf, k_ra8_hw_budget_long);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 38.2.2 "RDR : Receive Data Register" p 2180 */
  const uint8_t received = (uint8_t)(reg->RDR & (uint32_t)k_ra8_sci_rdr_mask_data8);
  /* HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register" p 2238 */
  reg->CFCLR = (1U << (uint32_t)k_ra8_sci_cfclr_bit_rdrfc);

  if (rx != nullptr) {
    *rx = received;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sci_spi_xfer(uint8_t channel, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  if (channel >= (uint8_t)k_ra8_sci_spi_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < len; i++) {
    const uint8_t   out = (tx != nullptr) ? tx[i] : (uint8_t)k_ra8_sci_spi_idle_byte;
    uint8_t         in  = 0U;
    const ra8_err_t err = ra8_sci_spi_xfer8(channel, out, &in);
    if (err != k_ra8_ok) {
      return err;
    }
    if (rx != nullptr) {
      rx[i] = in;
    }
  }
  return k_ra8_ok;
}
