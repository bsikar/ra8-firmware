/**
 * @file ra8_tsn.h
 * @brief On-chip Temperature Sensor (TSN) driver
 * @ingroup grp_hal_analog
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Minimum-viable driver for the RA8D2 on-chip die-temperature
 * sensor (HUM Ch 55, p 3497-3507). The block is tiny: the only
 * peripheral-side register is TSCR (TSEN + TSOE bits) plus two
 * read-only calibration words in MRAM (TSCDR, TSCDR2).
 *
 * ## Surface
 *
 *  - ``ra8_tsn_init()``   -- clear MSTPD22, set TSCR.TSEN, busy-wait
 *                           the 30 us reference-voltage stabilisation,
 *                           then set TSCR.TSOE so the sensor output
 *                           reaches the ADC16H input mux.
 *  - ``ra8_tsn_deinit()`` -- clear TSOE, then TSEN, then re-assert
 *                           the module-stop bit.
 *  - ``ra8_tsn_read_raw()``         -- accepts a raw 12-bit ADC code
 *                                     read by the caller from the
 *                                     ADC and returns it back as an
 *                                     uint16_t (no driver-side ADC
 *                                     access, see "Scope" below).
 *  - ``ra8_tsn_convert_to_milli_c()`` -- two-point trim conversion to
 *                                       degrees Celsius x 1000.
 *  - ``ra8_tsn_get_status()`` / ``ra8_tsn_clear_status()`` --
 *                                       read / clear TSCR enable bits.
 *  - ``ra8_tsn_enter_stop()`` / ``ra8_tsn_exit_stop()`` --
 *                                       MSTPD22 transitions.
 *
 * ## Scope
 *
 * **This driver does NOT touch the ADC.** HUM Ch 55.3.2 (p 3501)
 * describes the full conversion procedure: configure the ADC for
 * the temperature-sensor channel, start a conversion, then read
 * the result. Callers are responsible for that side of the dance
 * via ``libs/ra8_hal/src/adc.c`` (see ``ra8_adc_b_regs.h`` for the
 * ADC channel-select bits). This driver only owns TSCR and the
 * calibration math.
 *
 * ## Not yet implemented
 *
 *  - The TEMPRCR.TSNKEEP path (HUM Ch 6 "Resets") for surviving
 *    a low-power transition with the sensor still enabled. The
 *    HUM Ch 55.3.2 procedure mentions this register but it lives
 *    in the system controller; wiring it in requires the LPM /
 *    PRCR helpers from ``libs/ra8_hal/src/ra8_pwr.c``.
 *  - Abnormal-temperature reset source (Table 55.1 row 4) -- the
 *    output is a hardwired reset-circuit signal so the driver has
 *    no register to expose.
 *  - Single-point conversion (using only TSCDR + the typical slope
 *    from HUM Ch 69 "Electrical Characteristics"). Two-point trim
 *    is more accurate so the driver only ships that path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tsn_regs.h"

/**
 * @enum ra8_tsn_cal_temp_t
 * @brief Factory calibration reference temperatures (degC).
 *
 * @details
 * The RA8D2 family is shipped in three trim variants; the high-side
 * reference is either 105 degC (95 / 105 degC Tj_max parts) or
 * 125 degC (125 degC Tj_max parts), and the low-side reference is
 * always -40 degC. The calibration code observed at each reference
 * is programmed into ``TSCDR`` / ``TSCDR2`` at the factory
 * (HUM Ch 55.3.1 "Calculating the Temperature from the Output
 * Voltage of the Temperature Sensor", p 3499-3500).
 *
 * The numeric value of each enumerator is the temperature itself in
 * signed degC -- callers can cast directly to ``int16_t`` for the
 * conversion math without a lookup table.
 *
 * @invariant Only the three enumerators below are accepted by
 *            ``ra8_tsn_init``. Any other value is rejected with
 *            ``k_ra8_err_invalid_arg``.
 *
 * @see ra8_tsn_config_t::high_ref_degc
 * @see ra8_tsn_config_t::low_ref_degc
 */
typedef enum : int16_t {
  k_ra8_tsn_cal_temp_high_125 = 125, /**< 125 degC Tj_max parts.    */
  k_ra8_tsn_cal_temp_high_105 = 105, /**< 95 / 105 degC parts.      */
  k_ra8_tsn_cal_temp_low_n40  = -40, /**< Always -40 degC on RA8D2. */
} ra8_tsn_cal_temp_t;

/**
 * @struct ra8_tsn_config_t
 * @brief Configuration descriptor for ``ra8_tsn_init``.
 *
 * @details
 * - ``high_ref_degc`` selects which calibration temperature was
 *   used at factory shipment. Pass ``k_ra8_tsn_cal_temp_high_125``
 *   for Tj_max = 125 degC parts and ``k_ra8_tsn_cal_temp_high_105``
 *   for Tj_max = 95 / 105 degC parts (HUM Ch 55.3.1 p 3499-3500).
 * - ``low_ref_degc`` is always ``k_ra8_tsn_cal_temp_low_n40`` for
 *   the RA8D2 family and is captured here so the conversion math
 *   stays self-documenting.
 * - ``stab_us`` is the busy-wait observed after asserting TSCR.TSEN
 *   before the sensor output is valid. HUM Figure 55.2 (p 3502)
 *   names this tTSTBL = 30 us; callers may bump the value to
 *   absorb extra slack on slow boards but must not go below 30.
 */
typedef struct {
  ra8_tsn_cal_temp_t high_ref_degc; /**< 105 or 125 degC.          */
  ra8_tsn_cal_temp_t low_ref_degc;  /**< Always -40 degC on RA8D2. */
  uint16_t           stab_us;       /**< tTSTBL in microseconds.   */
} ra8_tsn_config_t;

/**
 * @brief Power on the TSN block and enable its ADC output path.
 *
 * @details
 * Algorithm (HUM Figure 55.2 p 3502 condensed for the case where
 * TEMPRCR.TSNKEEP is left at its reset value):
 *
 *  1. Validate ``cfg`` and the configured stabilisation delay.
 *  2. Clear MSTPD22 via ``ra8_mstp_enable(k_ra8_mstp_tsn)``
 *     (HUM Ch 11.2.9 p 449).
 *  3. Set TSCR.TSEN = 1 to start the sensor.
 *  4. Busy-wait ``cfg->stab_us`` microseconds (>= 30 us per the
 *     HUM tTSTBL spec).
 *  5. Set TSCR.TSOE = 1 to route the sensor output to the ADC16H
 *     input mux. The ADC itself is **not** touched here -- driving
 *     the ADC channel select stays the caller's job (see
 *     ``libs/ra8_hal/src/adc.c`` and the ADC chapter, HUM Ch 53).
 *
 * @param[in] cfg Non-null configuration descriptor.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok            Sensor enabled and routed to the ADC.
 * @retval k_ra8_err_null_ptr  ``cfg`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``cfg->stab_us`` below the 30 us floor
 *                              or calibration temperatures invalid.
 * @retval Other               Forwarded from ``ra8_mstp_enable``.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @pre AVCC0 / VREFH0 are stable (3.3 V).
 * @post TSCR.TSEN = 1 and TSCR.TSOE = 1.
 * @post The 30 us reference-voltage stabilisation has elapsed.
 *
 * @note Thread safety: not thread-safe; call once during init.
 * @note The driver does not start an ADC conversion. Callers must
 *       drive the ADC themselves.
 *
 * @par Example:
 * @code
 * const ra8_tsn_config_t cfg = {
 *     .high_ref_degc = k_ra8_tsn_cal_temp_high_125,
 *     .low_ref_degc  = k_ra8_tsn_cal_temp_low_n40,
 *     .stab_us       = 30,
 * };
 * (void)ra8_tsn_init(&cfg);
 * @endcode
 *
 * @see ra8_tsn_deinit
 * @see ra8_tsn_read_raw
 * @see ra8_tsn_convert_to_milli_c
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_init(const ra8_tsn_config_t* cfg);

/**
 * @brief Tear down the TSN block.
 *
 * @details
 * Clears TSCR.TSOE then TSCR.TSEN (HUM Figure 55.2 p 3502 reverse
 * sequence) and then re-asserts MSTPD22 via ``ra8_mstp_disable``.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok    Sensor stopped and module-stop reasserted.
 * @retval Other      Forwarded from ``ra8_mstp_disable``.
 *
 * @pre Caller has previously called ``ra8_tsn_init``.
 * @pre No outstanding ADC conversion targeting the TSN channel.
 * @post TSCR.TSEN = 0 and TSCR.TSOE = 0.
 * @post MSTPD22 is set (block clock-gated).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_tsn_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_deinit(void);

/**
 * @brief Pass through a raw ADC sample of the TSN channel.
 *
 * @details
 * The TSN block has no result register of its own -- the
 * conversion code lives in the ADC's data register. The caller
 * arms the ADC (see ``ra8_adc.h`` and ADC chapter HUM Ch 53),
 * reads the raw 12-bit code, and hands it here. This function
 * masks the value down to the documented 12-bit width and
 * returns it via ``out_code``. It exists so that future callers
 * can be retargeted at a different conversion path (for example
 * the high-resolution accumulator) without changing their call
 * sites.
 *
 * @param[in]  raw       Raw ADC code as read from the ADC's data
 *                       register. Only the lower 12 bits are
 *                       observed (HUM Ch 55.2.2 p 3498-3499:
 *                       calibration codes are 12-bit, so live
 *                       samples are too).
 * @param[out] out_code  Receives the masked 12-bit code.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok            Code stored in ``*out_code``.
 * @retval k_ra8_err_null_ptr  ``out_code`` was nullptr.
 * @retval k_ra8_err_invalid_state TSCR.TSEN is 0 (sensor not started).
 *
 * @pre ``ra8_tsn_init`` has been called.
 * @pre Caller already obtained the raw ADC code.
 * @post ``*out_code`` <= 0x0FFF.
 * @post No registers were written.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_tsn_convert_to_milli_c
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_read_raw(uint16_t raw, uint16_t* out_code);

/**
 * @brief Convert a raw 12-bit TSN code to milli-degrees-Celsius.
 *
 * @details
 * Two-point trim conversion (HUM Ch 55.3.1 p 3499-3500):
 *
 *   v1_uv  = AVCC_uV * CAL_HI / 4096
 *   v2_uv  = AVCC_uV * CAL_LO / 4096
 *   vs_uv  = AVCC_uV * raw    / 4096
 *   T_mC   = ((vs_uv - v1_uv) * (T1 - T2) * 1000)
 *            / (v1_uv - v2_uv)
 *          + T1 * 1000
 *
 * Everything stays in 64-bit signed integer arithmetic so the
 * driver can run on the Cortex-M85 without pulling in libm.
 *
 * @param[in]  raw_code  12-bit ADC code from ``ra8_tsn_read_raw``.
 * @param[out] out_milli_c  Temperature in milli-degC (+25000 = 25 degC).
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok                 Conversion succeeded.
 * @retval k_ra8_err_null_ptr       ``out_milli_c`` was nullptr.
 * @retval k_ra8_err_invalid_state  ``ra8_tsn_init`` has not run, or the
 *                                 calibration words are 0 (would
 *                                 divide by zero).
 *
 * @pre ``ra8_tsn_init`` has been called.
 * @pre ``raw_code`` <= 0x0FFF.
 * @post ``*out_milli_c`` populated when the call returns k_ra8_ok.
 * @post No peripheral registers were written.
 *
 * @note Thread safety: re-entrant -- reads MMIO only.
 * @see ra8_tsn_read_raw
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_convert_to_milli_c(uint16_t raw_code, int32_t* out_milli_c);

/**
 * @brief Read the die temperature end-to-end through the HAL.
 *
 * @details
 * Closes the loop that ``ra8_tsn_read_raw`` deliberately leaves to the
 * caller: it drives the ADC16H temperature-sensor channel (CNVCS = 0x64,
 * HUM Ch 53.2.3.1 Table p 3335) via ``ra8_adc_read_internal_channel``,
 * masks the raw 12-bit code through ``ra8_tsn_read_raw``, then runs the
 * two-point trim conversion (``ra8_tsn_convert_to_milli_c``). The result
 * register for the temperature channel is ADEXDR4 (HUM Ch 53.2.13.2
 * p 3391); that routing lives in ``adc.c``.
 *
 * @param[out] out_milli_c Temperature in milli-degC (+25000 = 25 degC).
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok                Temperature read and converted.
 * @retval k_ra8_err_null_ptr      ``out_milli_c`` was nullptr.
 * @retval k_ra8_err_invalid_state ``ra8_tsn_init`` has not run, or the
 *                                calibration words are unprogrammed.
 * @retval k_ra8_err_hw_timeout    The ADC conversion never completed.
 * @retval k_ra8_err_out_of_range  The temperature ADEXDR slot was unmapped.
 *
 * @pre ``ra8_tsn_init`` has routed the sensor to the ADC mux (TSCR.TSOE).
 * @pre ``ra8_adc_init`` has powered and clocked the ADC16H.
 * @post ``*out_milli_c`` populated when the call returns k_ra8_ok.
 * @post No TSN registers were written (only the ADC was driven).
 *
 * @note Thread safety: not thread-safe; serialise with other ADC use.
 * @see ra8_tsn_convert_to_milli_c
 * @see ra8_tsn_read_raw
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_read_die_temp_milli_c(int32_t* out_milli_c);

/**
 * @brief Read TSCR (TSEN + TSOE bits).
 *
 * @param[out] out_tscr Receives the current TSCR value (only
 *                      ``k_ra8_tscr_mask_all`` bits are valid).
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok            ``*out_tscr`` populated.
 * @retval k_ra8_err_null_ptr  ``out_tscr`` was nullptr.
 *
 * @pre TSN module-stop is cleared (otherwise the read returns 0).
 * @pre ``out_tscr`` is non-null.
 * @post No registers were written.
 * @post ``*out_tscr`` populated when the call returns k_ra8_ok.
 *
 * @note Thread safety: re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_get_status(uint8_t* out_tscr);

/**
 * @brief Clear TSCR (drive both TSEN and TSOE low).
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok Always.
 *
 * @pre TSN module-stop is cleared so the write actually lands.
 * @pre Caller has accepted that this stops the sensor immediately.
 * @post TSCR == 0.
 * @post Sensor output is no longer driven onto the ADC mux.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_clear_status(void);

/**
 * @brief Re-assert MSTPD22 to clock-gate the TSN block.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok    Module-stop bit asserted.
 * @retval Other      Forwarded from ``ra8_mstp_disable``.
 *
 * @pre Caller has stopped any in-flight ADC conversion that
 *      targets the TSN channel.
 * @pre ``ra8_mstp_init`` has been called.
 * @post MSTPD22 = 1.
 * @post Subsequent TSCR reads return 0 until ``ra8_tsn_exit_stop``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_tsn_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_enter_stop(void);

/**
 * @brief Clear MSTPD22 to re-power the TSN block.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok    Module-stop bit cleared.
 * @retval Other      Forwarded from ``ra8_mstp_enable``.
 *
 * @pre ``ra8_mstp_init`` has been called.
 * @pre Caller will re-run the TSEN / tTSTBL / TSOE sequence
 *      before reading any new conversion.
 * @post MSTPD22 = 0.
 * @post TSCR is back to its reset value (0).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_tsn_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tsn_exit_stop(void);

#ifdef __cplusplus
}
#endif
