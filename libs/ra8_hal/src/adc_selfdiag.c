/**
 * @file adc_selfdiag.c
 * @brief ADC_B self-diagnosis + internal extended-analog channel reads
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Self-diagnosis + internal / extended-analog slice of the RA8D2 ADC_B
 * HAL driver, split out of ``adc.c`` to keep every translation unit
 * under the file-size budget.
 *
 * Provides the SIL3 / DO-178C Level B periodic proof that the SAR core
 * and reference are healthy (self-diagnosis modes 1/2/3, HUM Ch 53.3.11
 * p 3411-3414), plus the close-the-loop read of the on-chip temperature
 * sensor / internal reference voltage from their ADEXDRn result
 * registers (HUM Ch 53.2.13.2 p 3391).
 *
 * The ADSR.ADACT0 conversion busy-poll budget is shared with ``adc.c``
 * via ``adc_internal.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "adc_internal.h"
#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

static const char* s_tag = "ADC";

/* The public ra8_adc_internal_chan_t values must mirror the register-header
 * CNVCS codes so ra8_adc_b_adexdr_index_for_chan() maps them correctly. */
static_assert((uint8_t)k_ra8_adc_chan_temperature == (uint8_t)k_ra8_adc_b_chan_temperature,
              "temperature CNVCS code drift between ra8_adc.h and ra8_adc_b_regs.h");
static_assert((uint8_t)k_ra8_adc_chan_int_ref_volt == (uint8_t)k_ra8_adc_b_chan_int_ref_volt,
              "internal-Vref CNVCS code drift between ra8_adc.h and ra8_adc_b_regs.h");

/**
 * @enum ra8_adc_diag_slot_t
 * @brief Dedicated virtual-channel / scan-group used for diagnostic reads.
 *
 * @details
 * Internal-channel and self-diagnosis conversions are isolated onto a
 * dedicated ADCHCR slot and scan group so they never collide with the
 * legacy default-group polling path. The slot is the top ADCHCR index
 * (no ADDR result slot is needed -- results come from ADEXDR).
 */
typedef enum : uint8_t {
  k_ra8_adc_diag_vchan = 23U, /**< Dedicated ADCHCR slot for diagnostic reads. */
  k_ra8_adc_diag_group = 8U,  /**< Dedicated scan group for diagnostic reads.  */
} ra8_adc_diag_slot_t;

/**
 * @enum ra8_adc_selfdiag_expect_t
 * @brief Ideal 16-bit signed self-diagnosis result per mode.
 *
 * @details HUM Table 53.19 (p 3412): mode 1 -> 0x0000, mode 2 -> 0x8000
 *          (-32768), mode 3 -> 0x7FFF (+32767).
 */
typedef enum : int32_t {
  k_ra8_adc_selfdiag_expect_mode1 = 0,      /**< 0x0000. */
  k_ra8_adc_selfdiag_expect_mode2 = -32768, /**< 0x8000. */
  k_ra8_adc_selfdiag_expect_mode3 = 32767,  /**< 0x7FFF. */
} ra8_adc_selfdiag_expect_t;

/**
 * @enum ra8_adc_selfdiag_tol_t
 * @brief Allowed absolute deviation from the ideal self-diagnosis result.
 *
 * @details
 * The self-diagnosis pass/fail band. A healthy SAR + reference lands on
 * the ideal value within a few LSB (HUM Ch 53.3.11.2 p 3414); gross
 * faults (stuck SAR, dead reference, wrong polarity) miss it by tens of
 * thousands of LSB, so a loose band reliably separates pass from fail.
 * TODO(confirm self-diagnosis accuracy band from HUM Ch 69 "Electrical
 * Characteristics" -- this gross-fault band is a placeholder until the
 * exact A/D total-error spec is read).
 */
typedef enum : int32_t {
  k_ra8_adc_selfdiag_tol_lsb = 256, /**< +/- LSB tolerance about the ideal. */
} ra8_adc_selfdiag_tol_t;

/**
 * @brief Translate a public self-diagnosis mode into the DIAGVAL code.
 *
 * @param[in]  mode         Public self-diagnosis mode selector.
 * @param[out] out_diagval  Receives the ADSGDCRn.DIAGVAL[2:0] code.
 * @return True if @p mode is valid, false otherwise.
 * @retval true  @p mode is one of modes 1/2/3; @p out_diagval set to the DIAGVAL code.
 * @retval false @p mode is out of range; @p out_diagval left untouched.
 *
 * @details Maps mode 1/2/3 onto DIAGVAL 0x4/0x5/0x6 (HUM Ch 53.2.4.1
 *          p 3340). Leaves @p out_diagval untouched on the invalid path.
 * @pre @p out_diagval is non-null.
 * @pre @p mode is sourced from ra8_adc_selfdiag_mode_t.
 * @post @p out_diagval is set only when the function returns true.
 * @post No registers are accessed.
 * @note Re-entrant; touches no globals or MMIO.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_diagval_for_mode(ra8_adc_selfdiag_mode_t mode,
                                                   uint32_t*               out_diagval)
{
  switch (mode) {
    case k_ra8_adc_selfdiag_mode_1:
      *out_diagval = (uint32_t)k_ra8_adsgdcr_diag_mode1;
      return true;
    case k_ra8_adc_selfdiag_mode_2:
      *out_diagval = (uint32_t)k_ra8_adsgdcr_diag_mode2;
      return true;
    case k_ra8_adc_selfdiag_mode_3:
      *out_diagval = (uint32_t)k_ra8_adsgdcr_diag_mode3;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Ideal signed self-diagnosis result for a mode.
 *
 * @param[in] mode Self-diagnosis mode (already validated).
 * @return Ideal 16-bit signed value (HUM Table 53.19 p 3412).
 * @retval -32768 @p mode is mode 2 (negative full-scale ideal).
 * @retval 32767  @p mode is mode 3 (positive full-scale ideal).
 * @retval 0      @p mode is mode 1 or any unlisted value (mid-scale ideal).
 *
 * @details Mode 1 -> 0, mode 2 -> -32768, mode 3 -> +32767.
 * @pre @p mode is one of modes 1/2/3.
 * @pre Caller validated @p mode via internal_diagval_for_mode.
 * @post Return value is in [-32768, +32767].
 * @post No registers are accessed.
 * @note Re-entrant; touches no globals or MMIO.
 * @since 0.1.0
 */
RA8_INTERNAL static int32_t internal_selfdiag_expected(ra8_adc_selfdiag_mode_t mode)
{
  switch (mode) {
    case k_ra8_adc_selfdiag_mode_2:
      return (int32_t)k_ra8_adc_selfdiag_expect_mode2;
    case k_ra8_adc_selfdiag_mode_3:
      return (int32_t)k_ra8_adc_selfdiag_expect_mode3;
    case k_ra8_adc_selfdiag_mode_1:
    default:
      return (int32_t)k_ra8_adc_selfdiag_expect_mode1;
  }
}

/**
 * @brief Test whether a signed deviation is inside the tolerance band.
 *
 * @param[in] diff Signed (actual - expected) deviation in LSB.
 * @return True iff @p diff is within +/- ``k_ra8_adc_selfdiag_tol_lsb``.
 * @retval true  @p diff lies within +/- ``k_ra8_adc_selfdiag_tol_lsb`` LSB.
 * @retval false @p diff exceeds the tolerance band on either edge.
 *
 * @details Compound decision: true when @p diff lies within both the
 *          upper and lower edges of the symmetric tolerance band.
 * @pre @p diff was computed in int32_t to avoid 16-bit overflow.
 * @pre The tolerance band is symmetric about zero.
 * @post No side effects.
 * @post Return value depends only on @p diff and the constant band.
 * @note Re-entrant; touches no globals or MMIO.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_selfdiag_in_band(int32_t diff)
{
  return (diff <= (int32_t)k_ra8_adc_selfdiag_tol_lsb) &&
         (diff >= -(int32_t)k_ra8_adc_selfdiag_tol_lsb);
}

/**
 * @brief Test whether a channel is a supported internal/extended source.
 *
 * @param[in] chan Internal-channel selector.
 * @return True iff @p chan is the temperature sensor or internal Vref.
 * @retval true  @p chan is the temperature sensor or the internal Vref channel.
 * @retval false @p chan is any other (unsupported) channel.
 *
 * @details Compound decision: true for either the temperature sensor or
 *          the internal reference-voltage channel.
 * @pre @p chan is sourced from ra8_adc_internal_chan_t.
 * @pre The set of supported channels matches the ADEXDR routing.
 * @post No side effects.
 * @post No registers are accessed.
 * @note Re-entrant; touches no globals or MMIO.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_supported_ext_chan(ra8_adc_internal_chan_t chan)
{
  return (chan == k_ra8_adc_chan_temperature) || (chan == k_ra8_adc_chan_int_ref_volt);
}

/**
 * @brief Program a dedicated ADCHCR slot for an extended-analog channel.
 *
 * @param[in] vch          Virtual-channel (ADCHCR) slot index.
 * @param[in] physical_ch  CNVCS[6:0] code of the extended-analog source.
 * @param[in] group        Scan-group membership.
 * @param[in] differential True -> AINMD differential, false -> single-ended.
 *
 * @details Writes SGSEL, CNVCS, and AINMD (HUM Ch 53.2.3.1 p 3335-3336).
 * @pre @p vch is a valid ADCHCR slot.
 * @pre @p group is a valid scan group.
 * @post On a valid slot the ADCHCR fields match the arguments.
 * @post No write occurs when @p vch is out of range.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_program_ext_channel(uint8_t vch, uint8_t physical_ch, uint8_t group, bool differential)
{
  volatile uint32_t* reg = ra8_adc_b_adchcr(vch);
  if (reg == nullptr) {
    return;
  }
  const uint32_t cnvcs =
    ((uint32_t)physical_ch << (uint32_t)k_ra8_adchcr_bit_cnvcs) & k_ra8_adchcr_mask_cnvcs;
  const uint32_t sgsel =
    ((uint32_t)group << (uint32_t)k_ra8_adchcr_bit_sgsel) & k_ra8_adchcr_mask_sgsel;
  const uint32_t ainmd =
    differential ? (((uint32_t)1UL << (uint32_t)k_ra8_adchcr_bit_ainmd) & k_ra8_adchcr_mask_ainmd)
                 : 0UL;
  /* HUM Ch 53.2.3.1 "ADCHCRn : A/D Conversion Channel Configuration Register n" p 3335 */
  *reg = (cnvcs | sgsel | ainmd);
}

/**
 * @brief Force the ADPRC data-format and SIGNSEL sign-format on a slot.
 *
 * @param[in] vch     Virtual-channel (ADDOPCRC) slot index.
 * @param[in] adprc   ADPRC[1:0] data-format code.
 * @param[in] signsel SIGNSEL sign-format code.
 *
 * @details Read-modify-writes ADDOPCRCn (HUM Ch 53.2.3.4 p 3339).
 * @pre @p vch is a valid ADDOPCRC slot.
 * @pre @p adprc / @p signsel are sourced from their register enums.
 * @post On a valid slot the ADPRC + SIGNSEL fields match the arguments.
 * @post No write occurs when @p vch is out of range.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_data_format(uint8_t vch, uint8_t adprc, uint8_t signsel)
{
  volatile uint32_t* reg = ra8_adc_b_addopcrc(vch);
  if (reg == nullptr) {
    return;
  }
  const uint32_t adprc_field =
    ((uint32_t)adprc << (uint32_t)k_ra8_addopcrc_bit_adprc) & k_ra8_addopcrc_mask_adprc;
  const uint32_t sign_field =
    ((uint32_t)signsel << (uint32_t)k_ra8_addopcrc_bit_signsel) & k_ra8_addopcrc_mask_signsel;
  /* HUM Ch 53.2.3.4 "ADDOPCRCn : A/D Conversion Data Operation Control C Register n" p 3339 */
  const uint32_t cleared = *reg & ~(k_ra8_addopcrc_mask_adprc | k_ra8_addopcrc_mask_signsel);
  *reg                   = (cleared | adprc_field | sign_field);
}

/**
 * @brief Start a scan group and bounded-poll ADSR.ADACT0 to completion.
 *
 * @param[in] group Scan-group index to kick.
 * @return ``k_ra8_ok`` when ADC0 goes idle, ``k_ra8_err_out_of_range`` for a
 *         bad group, or ``k_ra8_err_hw_timeout`` if the poll budget expires.
 * @retval k_ra8_ok               ADSR.ADACT0 cleared within the poll budget.
 * @retval k_ra8_err_out_of_range @p group has no ADSTR register (bad index).
 * @retval k_ra8_err_hw_timeout   ADACT0 stayed set until the poll budget expired.
 *
 * @details Kicks ADSTR[group], then polls ADSR.ADACT0 (HUM Ch 53 p 3308).
 * @pre @p group has been enabled in ADSGER.
 * @pre A virtual channel maps to @p group.
 * @post ADSTR[group].ADST has been asserted.
 * @post Returns only after ADACT0 clears or the budget expires.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_start_and_wait(uint8_t group)
{
  volatile uint32_t* adstr = ra8_adc_b_adstr(group);
  if (adstr == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *adstr = k_ra8_adstr_mask_adst;
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint32_t i = 0U; i < k_ra8_adc_busy_wait_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*ra8_adc_b_adsr() & k_ra8_adsr_mask_adact0) == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Arm the self-diagnosis channel, run one scan, and disarm DIAGVAL.
 *
 * @param[in] diagval ADSGDCRn.DIAGVAL[2:0] code for the requested mode.
 * @return ``k_ra8_ok`` when ADC0 goes idle, otherwise a forwarded error.
 * @retval k_ra8_ok               The diagnostic scan completed and ADC0 went idle.
 * @retval k_ra8_err_out_of_range Forwarded from internal_start_and_wait: bad group.
 * @retval k_ra8_err_hw_timeout   Forwarded from internal_start_and_wait: poll budget expired.
 *
 * @details
 * Maps the self-diagnosis channel (CNVCS = 0x60) onto the dedicated slot
 * in differential, 16-bit signed format (HUM Ch 53.3.11.1 p 3412 + Notes
 * p 3414), enables the dedicated scan group, writes DIAGVAL, kicks the
 * scan, then clears DIAGVAL back to off so later normal scans on the
 * group are not stuck in diagnosis mode (HUM Ch 53.2.4.1 p 3340).
 *
 * @pre @p diagval is one of the valid DIAGVAL mode codes.
 * @pre ``ra8_adc_init`` has powered and clocked the converter.
 * @post ADSGDCRn.DIAGVAL for the diagnostic group is back to 0.
 * @post The dedicated slot maps the self-diagnosis channel.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_selfdiag_run(uint32_t diagval)
{
  internal_program_ext_channel((uint8_t)k_ra8_adc_diag_vchan,
                               (uint8_t)k_ra8_adc_b_chan_selfdiag_adc0,
                               (uint8_t)k_ra8_adc_diag_group,
                               true);
  internal_set_data_format((uint8_t)k_ra8_adc_diag_vchan,
                           (uint8_t)k_ra8_addopcrc_adprc_16bit,
                           (uint8_t)k_ra8_addopcrc_signsel_signed);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_adsger() |= (uint32_t)(1UL << (uint32_t)k_ra8_adc_diag_group);

  /* HUM Ch 53.2.4.1 "ADSGDCRn : Scan Group Diagnosis Function Control Register n" p 3340 */
  volatile uint32_t* sgdcr = ra8_adc_b_adsgdcr((uint8_t)k_ra8_adc_diag_group);
  *sgdcr = (*sgdcr & ~k_ra8_adsgdcr_mask_diagval) |
           ((diagval << (uint32_t)k_ra8_adsgdcr_bit_diagval) & k_ra8_adsgdcr_mask_diagval);

  const ra8_err_t run_err = internal_start_and_wait((uint8_t)k_ra8_adc_diag_group);

  /* Always return DIAGVAL to off so later normal scans on this group are
   * not stuck in diagnosis mode (HUM Ch 53.2.4.1 p 3340). */
  /* HUM Ch 53.2.4.1 "ADSGDCRn : Scan Group Diagnosis Function Control Register n" p 3340 */
  *sgdcr = *sgdcr & ~k_ra8_adsgdcr_mask_diagval;

  return run_err;
}

ra8_err_t ra8_adc_self_diagnose(ra8_adc_selfdiag_mode_t mode, uint16_t* out_code, bool* out_pass)
{
  RA8_CHECK_NULL_PTR(out_code, s_tag, "out_code must not be nullptr");
  RA8_CHECK_NULL_PTR(out_pass, s_tag, "out_pass must not be nullptr");
  *out_code = 0U;
  *out_pass = false;

  uint32_t diagval = 0U;
  if (!internal_diagval_for_mode(mode, &diagval)) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t run_err = internal_selfdiag_run(diagval);
  RA8_RETURN_ON_ERROR(run_err, s_tag, "self_diagnose: conversion"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 53.2.13.2 "ADEXDRn : A/D Extended Analog Data Register n" p 3391 */
  const uint32_t exd =
    *ra8_adc_b_adexdr((uint8_t)k_ra8_adc_b_chan_selfdiag_adc0 - (uint8_t)k_ra8_adc_b_ext_chan_base);
  const uint16_t data     = (uint16_t)(exd & k_ra8_adexdr_mask_data);
  const bool     err_flag = (exd & k_ra8_adexdr_mask_err) != 0U;

  const int32_t actual   = (int32_t)(int16_t)data; /* signed format (SIGNSEL=0). */
  const int32_t expected = internal_selfdiag_expected(mode);
  const bool    in_band  = internal_selfdiag_in_band(actual - expected);

  *out_code = data;
  *out_pass = (!err_flag) && in_band;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_read_internal_channel(ra8_adc_internal_chan_t chan, uint16_t* out_raw)
{
  RA8_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");
  *out_raw = 0U;
  if (!internal_is_supported_ext_chan(chan)) {
    return k_ra8_err_invalid_arg;
  }

  /* Single-ended, 12-bit unsigned so the code lines up with the 12-bit
   * scale used by the TSN two-point calibration (HUM Ch 55.2.2 p 3498). */
  internal_program_ext_channel((uint8_t)k_ra8_adc_diag_vchan,
                               (uint8_t)chan,
                               (uint8_t)k_ra8_adc_diag_group,
                               false);
  internal_set_data_format((uint8_t)k_ra8_adc_diag_vchan,
                           (uint8_t)k_ra8_addopcrc_adprc_12bit,
                           (uint8_t)k_ra8_addopcrc_signsel_unsigned);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_adsger() |= (uint32_t)(1UL << (uint32_t)k_ra8_adc_diag_group);

  const ra8_err_t run_err = internal_start_and_wait((uint8_t)k_ra8_adc_diag_group);
  RA8_RETURN_ON_ERROR(run_err, s_tag, "read_internal: conversion"); /* GCOVR_EXCL_BR_LINE */

  const uint8_t      idx = ra8_adc_b_adexdr_index_for_chan((uint8_t)chan);
  volatile uint32_t* exd = ra8_adc_b_adexdr(idx);
  if (exd == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53.2.13.2 "ADEXDRn : A/D Extended Analog Data Register n" p 3391 */
  *out_raw = (uint16_t)(*exd & k_ra8_adexdr_mask_data);
  return k_ra8_ok;
}
