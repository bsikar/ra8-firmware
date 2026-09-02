/**
 * @file ra8_tsn.c
 * @brief On-chip Temperature Sensor (TSN) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Owns TSCR (HUM Ch 55.2.1 p 3498) and the two-point calibration
 * math (HUM Ch 55.3.1 p 3499-3500). Does not touch the ADC --
 * callers must drive the ADC themselves; ``adc.c`` exposes the
 * channel-select path (HUM Ch 53 "16-bit A/D Converter").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tsn.h"

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_tsn_regs.h"

static const char* s_tag = "TSN";

/**
 * @enum ra8_tsn_internal_t
 * @brief Driver-internal magic values.
 *
 * @details
 * - ``k_ra8_tsn_min_stab_us`` matches HUM Figure 55.2 (p 3502)
 *   tTSTBL = 30 us; ``ra8_tsn_init`` rejects shorter waits.
 * - ``k_ra8_tsn_busy_loops_per_us`` is a worst-case lower bound
 *   on the empty-loop count needed to absorb 1 us at the
 *   Cortex-M85 1 GHz ceiling. Real hardware will spin-wait
 *   longer; the driver only needs the floor to be safe.
 */
typedef enum : uint16_t {
  k_ra8_tsn_min_stab_us       = 30U,   /**< tTSTBL spec floor.       */
  k_ra8_tsn_busy_loops_per_us = 1000U, /**< Conservative spin count. */
} ra8_tsn_internal_t;

/**
 * @var s_ra8_tsn_initialized
 * @brief Tracks whether ``ra8_tsn_init`` has run successfully.
 *
 * @note Read by ``ra8_tsn_read_raw`` / ``ra8_tsn_convert_to_milli_c``
 *       to refuse work before init.
 */
static bool s_ra8_tsn_initialized;

/**
 * @var s_ra8_tsn_high_ref_degc
 * @brief Cached high-side calibration temperature.
 *
 * @details Populated from ``ra8_tsn_config_t::high_ref_degc`` so
 *          ``ra8_tsn_convert_to_milli_c`` can run without a config
 *          pointer. Defaults to 125 until init has run.
 */
static int16_t s_ra8_tsn_high_ref_degc = k_ra8_tsn_cal_temp_high_125;

/**
 * @var s_ra8_tsn_low_ref_degc
 * @brief Cached low-side calibration temperature (always -40).
 */
static int16_t s_ra8_tsn_low_ref_degc = k_ra8_tsn_cal_temp_low_n40;

/**
 * @brief Coarse software busy-wait used during init.
 *
 * @details
 * The HAL does not currently expose a microsecond busy-wait
 * helper; ``ra8_delay_ms()`` is the only timed primitive and its
 * 1 ms granularity vastly overshoots the 30 us tTSTBL floor.
 * For a one-shot path that runs only at init it is acceptable to
 * burn an empty loop. The loop body calls ::ra8_hw_nop, which is a real
 * ``nop`` on the target (so the compiler cannot optimise the spin away) and
 * a host no-op through the ``ra8_hw_intrinsics`` seam, which is fine because
 * the host has no timing requirement.
 *
 * @param[in] usec Microseconds to spin (>= ``k_ra8_tsn_min_stab_us``).
 *
 * @pre ``usec`` >= 1.
 * @pre Caller has IRQs masked (otherwise the wait stretches but
 *      never shortens, which is harmless).
 * @post Approximately ``usec`` microseconds have elapsed.
 *
 * @note Re-entrant; touches no globals or MMIO.
 *
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_busy_wait_us(uint16_t usec)
{
  for (uint16_t u = 0U; u < usec; ++u) {
    for (uint16_t i = 0U; i < k_ra8_tsn_busy_loops_per_us; ++i) {
      ra8_hw_nop();
    }
  }
}

/**
 * @brief Validate the high/low reference temperatures in a config.
 *
 * @param[in] cfg Caller-supplied descriptor (must be non-null).
 *
 * @return ``k_ra8_ok`` if the trim selection is one of the
 *         RA8D2-supported pairs, else ``k_ra8_err_invalid_arg``.
 *
 * @pre ``cfg`` is non-null.
 * @pre ``cfg`` lives in readable memory.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_cfg(const ra8_tsn_config_t* cfg)
{
  bool high_ok = false;
  if ((cfg->high_ref_degc == k_ra8_tsn_cal_temp_high_125) ||
      (cfg->high_ref_degc == k_ra8_tsn_cal_temp_high_105)) {
    high_ok = true;
  }
  bool low_ok = false;
  if (cfg->low_ref_degc == k_ra8_tsn_cal_temp_low_n40) {
    low_ok = true;
  }
  if (!high_ok || !low_ok) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->stab_us < k_ra8_tsn_min_stab_us) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_init(const ra8_tsn_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t cfg_err = internal_validate_cfg(cfg);
  RA8_RETURN_ON_ERROR(cfg_err, s_tag, "tsn_init: cfg invalid");

  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_tsn);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "tsn_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  volatile r_tsn_ctrl_regs_t* reg = ra8_tsn();
  reg->TSCR                       = k_ra8_tscr_mask_tsen;

  /* HUM Ch 55.3.2 "Procedures for Measuring Temperature Sensor", p 3502
   * (tTSTBL = 30 us per Figure 55.2 -- wait before enabling output). */
  internal_busy_wait_us(cfg->stab_us);

  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  reg->TSCR = (uint8_t)(k_ra8_tscr_mask_tsen | k_ra8_tscr_mask_tsoe);

  s_ra8_tsn_high_ref_degc = (int16_t)cfg->high_ref_degc;
  s_ra8_tsn_low_ref_degc  = (int16_t)cfg->low_ref_degc;
  s_ra8_tsn_initialized   = true;

  ra8_log_info(s_tag, "tsn_init");
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_deinit(void)
{
  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  volatile r_tsn_ctrl_regs_t* reg = ra8_tsn();
  /* HUM Ch 55.3.2 "Procedures for Measuring Temperature Sensor", p 3502
   * (clear TSOE first, then TSEN). */
  reg->TSCR = k_ra8_tscr_mask_tsen;
  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  reg->TSCR = 0U;

  /* Clear the init flag unconditionally so that callers cannot reach a
   * "looks-initialized but isn't" state if deinit is invoked from a
   * partially-torn-down context (e.g. mstp refcount already 0 because
   * the test harness reset it). The mstp refcount underflow is logged
   * by ra8_mstp itself and is harmless here. */
  s_ra8_tsn_initialized = false;

  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
  (void)ra8_mstp_disable(k_ra8_mstp_tsn);
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_read_raw(uint16_t raw, uint16_t* out_code)
{
  RA8_CHECK_NULL_PTR(out_code, s_tag, "out_code must not be nullptr");
  if (!s_ra8_tsn_initialized) {
    return k_ra8_err_invalid_state;
  }
  /* HUM Ch 55.2.2 "TSCDR : Temperature Sensor Calibration Data Register",
   * p 3498-3499 -- live samples share the 12-bit code width. */
  *out_code = (uint16_t)(raw & k_ra8_tscdr_data_mask);
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_convert_to_milli_c(uint16_t raw_code, int32_t* out_milli_c)
{
  RA8_CHECK_NULL_PTR(out_milli_c, s_tag, "out_milli_c must not be nullptr");
  if (!s_ra8_tsn_initialized) {
    return k_ra8_err_invalid_state;
  }

  /* HUM Ch 55.2.2 "TSCDR : Temperature Sensor Calibration Data Register", p 3498-3499 */
  volatile const r_tsn_cal_regs_t* cal    = ra8_tsn_cal();
  const uint32_t                   cal_hi = cal->TSCDR & (uint32_t)k_ra8_tscdr_data_mask;
  const uint32_t                   cal_lo = cal->TSCDR2 & (uint32_t)k_ra8_tscdr_data_mask;

  if (cal_hi == cal_lo) {
    /* Either an unprogrammed part or both trim words read 0; both
     * mean "no slope is computable" and we must refuse rather than
     * divide by zero (HUM Ch 55.3.1 p 3499). */
    return k_ra8_err_invalid_state;
  }

  /* All math in 64-bit signed integers, microvolt scale. */
  const int64_t avcc_uv      = (int64_t)k_ra8_tsn_adc_avcc_uv;
  const int64_t full_scale   = (int64_t)k_ra8_tsn_adc_full_scale;
  const int64_t v1_uv        = (avcc_uv * (int64_t)cal_hi) / full_scale;
  const int64_t v2_uv        = (avcc_uv * (int64_t)cal_lo) / full_scale;
  const int64_t vs_uv        = (avcc_uv * (int64_t)raw_code) / full_scale;
  const int64_t t1_milli_c   = (int64_t)s_ra8_tsn_high_ref_degc * (int64_t)k_ra8_tsn_uv_per_mv;
  const int64_t t2_milli_c   = (int64_t)s_ra8_tsn_low_ref_degc * (int64_t)k_ra8_tsn_uv_per_mv;
  const int64_t v_delta_uv   = v1_uv - v2_uv; /* != 0 by guard above */
  const int64_t numerator_uv = (vs_uv - v1_uv) * (t1_milli_c - t2_milli_c);
  const int64_t result_milli = (numerator_uv / v_delta_uv) + t1_milli_c;

  *out_milli_c = (int32_t)result_milli;
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_read_die_temp_milli_c(int32_t* out_milli_c)
{
  RA8_CHECK_NULL_PTR(out_milli_c, s_tag, "out_milli_c must not be nullptr");
  if (!s_ra8_tsn_initialized) {
    return k_ra8_err_invalid_state;
  }

  /* Drive the ADC temperature-sensor channel (CNVCS = 0x64) and read the
   * raw code back from its ADEXDR4 result register (HUM Ch 53.2.13.2
   * p 3391). The ADC side lives entirely in adc.c. */
  uint16_t        adc_raw = 0U;
  const ra8_err_t adc_err = ra8_adc_read_internal_channel(k_ra8_adc_chan_temperature, &adc_raw);
  /* GCOVR_EXCL_BR_START -- HW ADC read path */
  RA8_RETURN_ON_ERROR(adc_err, s_tag, "die_temp: adc read");
  /* GCOVR_EXCL_BR_STOP */

  uint16_t        code     = 0U;
  const ra8_err_t mask_err = ra8_tsn_read_raw(adc_raw, &code);
  /* GCOVR_EXCL_BR_START -- HW ADC raw path */
  RA8_RETURN_ON_ERROR(mask_err, s_tag, "die_temp: read_raw");
  /* GCOVR_EXCL_BR_STOP */

  return ra8_tsn_convert_to_milli_c(code, out_milli_c);
}

ra8_err_t ra8_tsn_get_status(uint8_t* out_tscr)
{
  RA8_CHECK_NULL_PTR(out_tscr, s_tag, "out_tscr must not be nullptr");
  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  volatile const r_tsn_ctrl_regs_t* reg = ra8_tsn();
  *out_tscr                             = (uint8_t)(reg->TSCR & k_ra8_tscr_mask_all);
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_clear_status(void)
{
  /* HUM Ch 55.2.1 "TSCR : Temperature Sensor Control Register", p 3498 */
  volatile r_tsn_ctrl_regs_t* reg = ra8_tsn();
  reg->TSCR                       = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_tsn_enter_stop(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
  return ra8_mstp_disable(k_ra8_mstp_tsn);
}

ra8_err_t ra8_tsn_exit_stop(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
  return ra8_mstp_enable(k_ra8_mstp_tsn);
}
