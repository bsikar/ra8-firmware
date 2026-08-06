/**
 * @file ra8_i2c_config.c
 * @brief I2C Bus Interface (IIC) bring-up, clock and error-status plane
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Configuration-plane half of the RA8D2 RIIC polling driver, split out of
 * ``ra8_i2c.c`` to keep each translation unit under the file-size cap. Owns
 * the init / deinit / bit-rate sequence (HUM Ch 39.3.2 "Initial Settings"
 * p 2395), the runtime clock re-program (``ra8_i2c_set_clock``) and the
 * error-status helpers (``ra8_i2c_get_errors`` / ``ra8_i2c_clear_errors``).
 *
 * The data-transfer plane (start / write / read / stop / scan) lives in
 * ``ra8_i2c.c``. Both translation units share ``s_i2c_state`` and the log
 * tag via ``ra8_i2c_internal.h``.
 *
 * Owns every write to the RIIC register block performed during channel
 * bring-up and clock setup. See HUM Ch 39 "I2C Bus Interface (IIC)",
 * p 2367-2470.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @enum ra8_i2c_brate_t
 * @brief Bit-rate divider field limits (HUM Ch 39.2.15 / 39.2.16).
 */
typedef enum : uint32_t {
  /** ICBRL / ICBRH counter fields are 5 bits ([4:0]). */
  k_ra8_i2c_br_field_max = 0x1FU,
  /** Upper 3 reserved bits of ICBRL / ICBRH read as 1. */
  k_ra8_i2c_br_reserved_hi = 0xE0U,
  /** CKS divider exponent ceiling (CKS[2:0] selects PCLKB / 2^CKS). */
  k_ra8_i2c_cks_max = 7U,
  /** Period split between SCL high and low halves. */
  k_ra8_i2c_period_split = 2U,
} ra8_i2c_brate_t;

/**
 * @brief Map a channel index to its MSTP gate id.
 *
 * @details
 * IIC0 = MSTPB9, IIC1 = MSTPB8, IIC2 = MSTPB7 per HUM Ch 11.2.7
 * "MSTPCRB" p 444, encoded in ``ra8_mstp_regs.h``.
 *
 * @param[in] channel Channel index (already range-checked by caller).
 * @return The matching ``k_ra8_mstp_iicN`` enum value.
 * @retval k_ra8_mstp_iic0 ``channel`` is 0.
 * @retval k_ra8_mstp_iic1 ``channel`` is 1.
 * @retval k_ra8_mstp_iic2 ``channel`` is 2 (or any other value, defensively).
 *
 * @pre ``channel`` is 0, 1 or 2.
 * @pre Caller resolved a non-NULL register pointer for ``channel``.
 * @post Return value is one of the three IIC MSTP ids.
 * @post No global state is mutated.
 * @note Thread safety: pure mapping, no state.
 * @since 0.1.0
 */
static ra8_mstp_t internal_i2c_mstp_id(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra8_mstp_iic0;
  }
  if (channel == 1U) {
    return k_ra8_mstp_iic1;
  }
  return k_ra8_mstp_iic2;
}

/**
 * @brief Pick the smallest CKS divider and divide ``*total`` to match.
 *
 * @details
 * Repeatedly halves the candidate bit-period cycle count until a single
 * SCL half-period fits inside the 5-bit ICBRL/ICBRH field, returning the
 * CKS exponent used. The loop is bounded by ``k_ra8_i2c_cks_max + 1``
 * iterations (NASA P10 Rule 2).
 *
 * @param[in,out] total On entry the full ``PCLKB / bus_hz`` cycle count;
 *                      on return divided by ``2^CKS``.
 *
 * @return The chosen CKS exponent, clamped to ``[0, k_ra8_i2c_cks_max]``.
 * @retval 0 The full period already fits the field.
 *
 * @pre total is non-NULL.
 * @pre ``*total`` was derived from non-zero clocks.
 * @post ``*total`` reflects the divided cycle count.
 * @post Return value is in ``[0, k_ra8_i2c_cks_max]``.
 * @note Thread safety: pure on the supplied pointer; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_pick_cks(uint32_t* total)
{
  uint32_t cks = 0U;
  for (uint32_t i = 0U; i <= (uint32_t)k_ra8_i2c_cks_max; i++) { /* GCOVR_EXCL_BR_LINE */
    const uint32_t half = *total / (uint32_t)k_ra8_i2c_period_split;
    if (half <= ((uint32_t)k_ra8_i2c_br_field_max + 1U)) {
      break;
    }
    cks    = i + 1U;
    *total = *total >> 1U;
  }
  if (cks > (uint32_t)k_ra8_i2c_cks_max) {
    cks = (uint32_t)k_ra8_i2c_cks_max;
  }
  return (uint8_t)cks;
}

/**
 * @brief Clamp one SCL half-period count to the 5-bit BR field.
 *
 * @details
 * Splits ``total`` evenly between the SCL high and low halves, subtracts
 * the +1 the hardware adds, and clamps to ``k_ra8_i2c_br_field_max``.
 *
 * @param[in] total Divided bit-period cycle count.
 *
 * @return The 5-bit half-period field value.
 * @retval 0 The divided period collapsed to a single cycle.
 *
 * @pre total is the output of ``internal_i2c_pick_cks``.
 * @pre None.
 * @post Return value is in ``[0, k_ra8_i2c_br_field_max]``.
 * @post No state is mutated.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_clamp_half(uint32_t total)
{
  uint32_t half = total / (uint32_t)k_ra8_i2c_period_split;
  if (half == 0U) {
    half = 1U;
  }
  uint32_t field = half - 1U;
  if (field > (uint32_t)k_ra8_i2c_br_field_max) {
    field = (uint32_t)k_ra8_i2c_br_field_max;
  }
  return (uint8_t)field;
}

/**
 * @brief Compute the CKS divider and ICBRH/ICBRL half-period counts.
 *
 * @details
 * The RIIC internal reference clock is ``IICphi = PCLKB / 2^CKS`` (HUM
 * Ch 39.2.3 p 2374). One SCL bit period is ``(BRH + 1) + (BRL + 1)``
 * IICphi cycles for the SCLE=0 / NFE=0 transfer-rate expression (HUM
 * Ch 39.2.16 expression (1) p 2392). Delegates the CKS search and field
 * clamp to ``internal_i2c_pick_cks`` / ``internal_i2c_clamp_half``.
 *
 * @param[in]  bus_hz   Target bus clock (non-zero).
 * @param[in]  pclkb_hz PCLKB frequency (non-zero).
 * @param[out] out_cks  CKS exponent [0..7].
 * @param[out] out_brh  ICBRH register value (5-bit count + reserved hi).
 * @param[out] out_brl  ICBRL register value (5-bit count + reserved hi).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Divider computed.
 * @retval k_ra8_err_invalid_arg A clock argument was zero.
 *
 * @pre out_cks and out_brh are non-NULL.
 * @pre bus_hz and pclkb_hz are non-zero.
 * @post On success ``*out_cks <= 7`` and the BR fields are clamped.
 * @post On error no output is written.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_bitrate(uint32_t bus_hz,
                                      uint32_t pclkb_hz,
                                      uint8_t* out_cks,
                                      uint8_t* out_brh,
                                      uint8_t* out_brl)
{
  RA8_CHECK_NULL_PTR(out_cks, s_i2c_tag, "bitrate: out_cks");
  RA8_CHECK_NULL_PTR(out_brh, s_i2c_tag, "bitrate: out_brh");
  if (ra8_i2c_internal_clk_invalid(bus_hz, pclkb_hz)) {
    return k_ra8_err_invalid_arg;
  }

  /* Each transferred bit needs (BRH+1)+(BRL+1) IICphi cycles. Pick the
   * smallest CKS that fits both half-periods in the 5-bit field, then
   * derive the clamped half-period count for ICBRL / ICBRH. */
  uint32_t      total = pclkb_hz / bus_hz;
  const uint8_t cks   = internal_i2c_pick_cks(&total);
  const uint8_t field = internal_i2c_clamp_half(total);

  *out_cks = cks;
  *out_brh = (uint8_t)((uint32_t)k_ra8_i2c_br_reserved_hi | (uint32_t)field);
  *out_brl = (uint8_t)((uint32_t)k_ra8_i2c_br_reserved_hi | (uint32_t)field);
  return k_ra8_ok;
}

/**
 * @brief Decode latched ICSR2 error bits into a ``k_ra8_i2c_err_*`` mask.
 *
 * @details
 * Tests the AL, NACKF and TMOF flags independently and ORs the matching
 * ``k_ra8_i2c_err_*`` bit into the result so a caller sees every latched
 * fault in a single mask.
 *
 * @param[in] icsr2 Snapshot of ICSR2.
 * @return OR of ``k_ra8_i2c_err_*`` bits.
 * @retval k_ra8_i2c_err_none No fault bit set.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the input snapshot.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_decode_errors(uint8_t icsr2)
{
  uint8_t mask = (uint8_t)k_ra8_i2c_err_none;
  if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_al) != 0U) {
    mask |= (uint8_t)k_ra8_i2c_err_arb_lost;
  }
  if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) != 0U) {
    mask |= (uint8_t)k_ra8_i2c_err_nack;
  }
  /* TMOF lives in ICSR2 bit 0; reuse the timeout mask via the position. */
  if ((icsr2 & (uint8_t)(1U << (uint8_t)k_ra8_i2c_icsr2_tmof_pos)) != 0U) {
    mask |= (uint8_t)k_ra8_i2c_err_timeout;
  }
  return mask;
}

/* =============================================================================
 * Init / deinit -- mirrors HUM Ch 39.3.2 "Initial Settings" p 2395.
 * =============================================================================
 */

/**
 * @brief Apply the bring-up register sequence for an IIC channel.
 *
 * @details
 * Follows HUM Ch 39.3.2 p 2395: hold IIC reset (ICCR1.IICRST with
 * ICE = 0), enable internal reset (ICE = 1), program CKS / ICBRL /
 * ICBRH and the ICFER function bits, then release the reset. FMPE is
 * set for the Fast-mode Plus (>= 1 MHz) bus rate.
 *
 * @param[in] reg Channel register block.
 * @param[in] cks CKS divider exponent.
 * @param[in] brh ICBRH register value.
 * @param[in] brl ICBRL register value.
 * @param[in] fast_plus True when the bus runs at Fast-mode Plus.
 *
 * @pre reg is non-NULL.
 * @pre Channel MSTP gate already ungated.
 * @post ICCR1.ICE is set and the channel is out of reset.
 * @post ICMR1.CKS, ICBRL, ICBRH and ICFER hold the programmed values.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_apply_init_regs(volatile r_i2c_regs_t* reg,
                                         uint8_t                cks,
                                         uint8_t                brh,
                                         uint8_t                brl,
                                         bool                   fast_plus)
{
  /* Initial-settings step 1 (Figure 39.5): ICE = 0, pins inactive.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = 0U;
  /* Initial-settings step 2: IICRST = 1 (IIC reset, ICE still 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra8_i2c_msk_iccr1_iicrst;
  /* Initial-settings step 3: ICE = 1 (internal reset, pins active).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)((uint8_t)k_ra8_i2c_msk_iccr1_iicrst | (uint8_t)k_ra8_i2c_msk_iccr1_ice);

  /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1 -- CKS[6:4]" p 2374 */
  reg->ICMR1 = (uint8_t)((uint32_t)cks << (uint32_t)k_ra8_i2c_icmr1_cks_pos);
  /* HUM Ch 39.2.15 "ICBRL : I2C Bus Bit Rate Low-Level Register" p 2391 */
  reg->ICBRL = brl;
  /* HUM Ch 39.2.16 "ICBRH : I2C Bus Bit Rate High-Level Register" p 2392 */
  reg->ICBRH = brh;

  /* Enable arbitration-lost detection, NACK transfer suspension, the SCL
   * synchronous circuit, and (for Fm+) the slope-control circuit.
   * HUM Ch 39.2.6 "ICFER : I2C Bus Function Enable Register" p 2378 */
  uint8_t icfer = (uint8_t)((uint8_t)k_ra8_i2c_msk_icfer_male | (uint8_t)k_ra8_i2c_msk_icfer_nacke |
                            (uint8_t)k_ra8_i2c_msk_icfer_scle);
  if (fast_plus) {
    icfer |= (uint8_t)k_ra8_i2c_msk_icfer_fmpe;
  }
  reg->ICFER = icfer;

  /* Initial-settings step 5: release the internal reset (IICRST = 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra8_i2c_msk_iccr1_ice;
}

ra8_err_t ra8_i2c_init(uint8_t channel, const ra8_i2c_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_i2c_tag, "i2c_init: cfg");
  if (ra8_i2c_internal_clk_invalid(cfg->bus_hz, cfg->pclkb_hz)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t mst_err = ra8_mstp_enable(internal_i2c_mstp_id(channel));
  RA8_RETURN_ON_ERROR(mst_err, s_i2c_tag, "i2c_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  uint8_t         cks    = 0U;
  uint8_t         brh    = 0U;
  uint8_t         brl    = 0U;
  const ra8_err_t br_err = internal_i2c_bitrate(cfg->bus_hz, cfg->pclkb_hz, &cks, &brh, &brl);
  RA8_RETURN_ON_ERROR(br_err, s_i2c_tag, "i2c_init: bitrate"); /* GCOVR_EXCL_BR_LINE */

  internal_i2c_apply_init_regs(reg,
                               cks,
                               brh,
                               brl,
                               cfg->bus_hz >= (uint32_t)k_ra8_i2c_speed_fast_plus);

  s_i2c_state[channel].initialized = true;
  s_i2c_state[channel].bus_held    = false;

  ra8_log_info_val(s_i2c_tag, "i2c_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_deinit(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* ICE = 0: place the SCL/SDA pins back in the inactive state.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1                       = 0U;
  s_i2c_state[channel].initialized = false;
  s_i2c_state[channel].bus_held    = false;
  return ra8_mstp_disable(internal_i2c_mstp_id(channel));
}

ra8_err_t ra8_i2c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t         cks    = 0U;
  uint8_t         brh    = 0U;
  uint8_t         brl    = 0U;
  const ra8_err_t br_err = internal_i2c_bitrate(bus_hz, pclkb_hz, &cks, &brh, &brl);
  if (br_err != k_ra8_ok) {
    return br_err;
  }
  /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1 -- CKS[6:4]" p 2374 */
  reg->ICMR1 = (uint8_t)(((uint32_t)reg->ICMR1 & ~((uint32_t)(uint8_t)k_ra8_i2c_cks_max
                                                   << (uint32_t)k_ra8_i2c_icmr1_cks_pos)) |
                         ((uint32_t)cks << (uint32_t)k_ra8_i2c_icmr1_cks_pos));
  /* HUM Ch 39.2.15 "ICBRL : I2C Bus Bit Rate Low-Level Register" p 2391 */
  reg->ICBRL = brl;
  /* HUM Ch 39.2.16 "ICBRH : I2C Bus Bit Rate High-Level Register" p 2392 */
  reg->ICBRH = brh;
  return k_ra8_ok;
}

/* =============================================================================
 * Status helpers.
 * =============================================================================
 */

ra8_err_t ra8_i2c_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_i2c_tag, "i2c_get_errors: out_mask");
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  *out_mask = internal_i2c_decode_errors(reg->ICSR2);
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_clear_errors(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  enum : uint8_t {
    k_ra8_i2c_err_clear_mask =
      (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_al | (uint8_t)k_ra8_i2c_msk_icsr2_nackf |
                (uint8_t)(1U << (uint8_t)
                            k_ra8_i2c_icsr2_tmof_pos)), /**< RA8 I2C error clear mask. */
  };
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- W0C" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t) ~(uint8_t)k_ra8_i2c_err_clear_mask);
  return k_ra8_ok;
}
