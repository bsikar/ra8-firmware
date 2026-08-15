/**
 * @file examples/ek_ra8d2/hw_validated/hil/adc_b_demo/main.c
 * @brief ADC_B VREF-channel sample-and-log demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Configures the ADC_B in 12-bit, software-trigger mode, samples the
 * internal VREF channel (``k_adc_b_demo_channel``) once per loop, and
 * logs the raw count plus the converted millivolt value over SCI8.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8 on PD_02 / PD_03).
 *   2. ``ra8_adc_init_configured`` with software trigger + 12-bit res.
 *   3. Loop: ``ra8_adc_read_channel`` -> format -> ``ra8_board_uart_console_write``.
 *      After each successful read it also emits the fixed verdict line
 *      ``"adc: read PASS\r\n"`` that the HIL scrape keys on.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/* TODO: Move this to its own test for 12-bit res and create another test that is 16-bit res:
 * Analog (Datasheet Page 1):
 * 16-bit A/D Converter (ADC16H) x 2, up to 23 channels
 * 12-bit D/A Converter (DAC12) x 2
 * High-Speed Analog Comparator (ACMPHS) x 4
 * Temperature Sensor (TSN)
 */

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_adc_b_demo_baud       = 115200U, /**< ADC b demo baud.       */
  k_adc_b_demo_period_ms  = 500U,    /**< ADC b demo period ms.  */
  k_adc_b_demo_vref_mv    = 3300U,   /**< EK-RA8D2 VREFH = 3.3V. */
  k_adc_b_demo_full_scale = 4095U,   /**< 12-bit max code.       */
} adc_b_demo_const_t;

/** @brief ADC channel selection. */
typedef enum : uint8_t {
  k_adc_b_demo_channel = 0U, /**< AN000 -- safe default. */
} adc_b_demo_chan_t;

/** @brief Formatting constants. */
typedef enum : uint8_t {
  k_adc_b_demo_dec_digits = 5U,   /**< Max decimal digits for uint16_t (65535). */
  k_adc_b_demo_line_max   = 32U,  /**< "adc: raw=XXXXX mv=XXXXX\r\n" fits here. */
  k_adc_b_demo_radix      = 10U,  /**< ADC b demo radix.                        */
  k_adc_b_demo_cr         = '\r', /**< ADC b demo cr.                           */
  k_adc_b_demo_lf         = '\n', /**< ADC b demo lf.                           */
} adc_b_demo_fmt_t;

static const uint8_t s_adc_b_demo_log_prefix[] = "adc: raw=";
static const uint8_t s_adc_b_demo_mv_sep[]     = " mv=";

/** @brief Verdict line emitted only after a successful channel read. */
static const uint8_t s_adc_b_demo_pass_msg[] = "adc: read PASS\r\n";

/**
 * @brief Write the decimal digits of a 16-bit value into a byte buffer.
 *
 * @details Handles zero explicitly; otherwise collects least-significant
 *          digits in a bounded local array and reverses them into the caller's
 *          destination without appending a NUL terminator.
 *
 * @param[out] buf Destination with room for five decimal digits.
 * @param[in] val Value to convert.
 * @return Number of bytes written to ``buf``.
 * @retval 1 Zero or a single-digit value was written.
 * @retval 2..5 The exact digit count of a larger value.
 * @pre ``buf`` points to at least ``k_adc_b_demo_dec_digits`` writable bytes.
 * @pre The destination does not overlap this function's local storage.
 * @post The returned prefix of ``buf`` is the base-10 representation of ``val``.
 * @post No byte beyond the returned count is modified.
 * @note Reentrant when callers provide distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_u16_to_dec(uint8_t* buf, uint16_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_adc_b_demo_dec_digits];
  uint32_t n = 0U;
  uint16_t v = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint16_t)k_adc_b_demo_radix));
    v      = (uint16_t)(v / (uint16_t)k_adc_b_demo_radix);
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Park the core after an unrecoverable ADC demo failure.
 * @details Repeatedly executes WFI, preserving failure state for a debugger.
 * @pre Called only from boot failure or the terminal foreground path.
 * @pre The caller does not require recovery without reset.
 * @post The core stays in the WFI loop until external intervention.
 * @post No more ADC conversions or console writes are requested.
 * @note Not thread-safe; this is a terminal single-threaded path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, SysTick, SCI8, and LED1.
 * @details Brings dependencies up in order and parks on the first HAL error.
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, delays, console output, and LED1 are ready.
 * @post ADC_B remains unconfigured until ``internal_arm`` runs.
 * @note Not thread-safe; it mutates global board and clock state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_adc_b_demo_baud) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Configure ADC_B in 12-bit software-trigger mode.
 * @details Supplies the fixed resolution, trigger, alignment, and single-scan
 *          policy to the configured ADC initialization API.
 *
 * @par MC/DC:
 * Decision: ``ra8_adc_init_configured != ok``. One atomic condition x
 * 2 vectors -- golden (this) + null cfg reject
 * (test_app_adc_b_demo.c).
 * @return ADC configuration status from the HAL.
 * @retval k_ra8_ok ADC_B accepted the fixed demo configuration.
 * @retval (other) ADC_B initialization failed or rejected the configuration.
 * @pre ``internal_setup_or_halt`` completed clock initialization.
 * @pre No other context owns ADC_B configuration.
 * @post On success, software-triggered channel reads are available.
 * @post On failure, the error is returned unchanged to the caller.
 * @note Not thread-safe with concurrent ADC configuration.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_arm(void)
{
  const ra8_adc_cfg_t cfg = {
    .resolution    = k_ra8_adc_res_12bit,
    .trigger       = k_ra8_adc_trig_software,
    .right_aligned = true,
    .scan_mode     = false,
  };
  return ra8_adc_init_configured(&cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Sample ADC_B and report raw and converted values continuously.
 * @details Initializes and arms ADC_B, formats each successful channel sample
 *          without variadic I/O, emits the verdict, toggles LED1, and delays.
 * @return Unreachable success value retained for the freestanding ABI.
 * @pre Reset startup and SystemInit completed successfully.
 * @pre The selected ADC channel matches the board analog connection.
 * @post Every PASS verdict follows a successful channel read.
 * @post Any read or LED failure leads to the terminal panic helper.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_arm() != k_ra8_ok) {
    internal_panic_halt();
  }

  while (1) {
    uint16_t raw = 0U;
    if (ra8_adc_read_channel((uint8_t)k_adc_b_demo_channel, &raw) != k_ra8_ok) {
      break;
    }
    const uint16_t mv = (uint16_t)(((uint32_t)raw * (uint32_t)k_adc_b_demo_vref_mv) /
                                   (uint32_t)k_adc_b_demo_full_scale);

    uint8_t  line[k_adc_b_demo_line_max];
    uint32_t pos = 0U;
    for (uint32_t i = 0U; i < (sizeof(s_adc_b_demo_log_prefix) - 1U); i++) {
      line[pos++] = s_adc_b_demo_log_prefix[i];
    }
    pos += internal_u16_to_dec(&line[pos], raw);
    for (uint32_t i = 0U; i < (sizeof(s_adc_b_demo_mv_sep) - 1U); i++) {
      line[pos++] = s_adc_b_demo_mv_sep[i];
    }
    pos += internal_u16_to_dec(&line[pos], mv);
    line[pos++] = (uint8_t)k_adc_b_demo_cr;
    line[pos++] = (uint8_t)k_adc_b_demo_lf;

    (void)ra8_board_uart_console_write(line, (size_t)pos);
    (void)ra8_board_uart_console_write(s_adc_b_demo_pass_msg,
                                       (size_t)(sizeof(s_adc_b_demo_pass_msg) - 1U));
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_adc_b_demo_period_ms);
  }
  internal_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
