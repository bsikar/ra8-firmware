/**
 * @file examples/ek_ra8d2/hw_pending/adc_diag_tsn_demo/main.c
 * @brief ADC_B self-diagnosis (modes 1/2/3) + on-die temperature demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the two safety-relevant analog HAL entry points that
 * ra8_emulator cannot model (the SAR self-test and the temperature-sensor
 * path are not emulated), so this app is bench-only:
 *
 *   1. ``ra8_adc_self_diagnose`` -- the ADC_B built-in reference
 *      self-test. Each of the three modes injects a known internal
 *      reference level into the SAR core and compares the conversion
 *      against the tabulated ideal, proving the converter + reference
 *      are healthy (HUM Ch 53.3.11 "Self-Diagnosis" p 3411-3414).
 *   2. ``ra8_tsn_read_die_temp_milli_c`` -- the close-the-loop die
 *      temperature read that drives the ADC16H temperature-sensor
 *      channel (CNVCS = 0x64) and applies the TSN two-point trim
 *      (HUM Ch 55.3.1 p 3499-3500).
 *   3. ``ra8_adc_read_internal_channel`` on the internal reference
 *      voltage channel (CNVCS = 0x65) as a second internal-channel
 *      probe (HUM Ch 53.2.13.2 "ADEXDRn" p 3391).
 *
 * Bring-up sequence (CGC -> MSTP -> TIME -> peripheral):
 *   1. CGC + SysTick + UART console (SCI8 on PD_02 / PD_03 -> J-Link OB).
 *   2. ``ra8_adc_init`` (14-bit, software trigger) + ``ra8_tsn_init``.
 *   3. Loop once per ``k_adc_diag_period_ms``:
 *        - run self-diagnosis modes 1/2/3, log each raw code + PASS/FAIL;
 *        - read + log the die temperature in milli-degC;
 *        - read + log the internal Vref raw code;
 *        - emit the fixed verdict line ``"diag: selftest PASS"`` only
 *          when every mode reported healthy and both internal reads
 *          succeeded (the HIL scrape keys on this line).
 *
 * Bare EK-RA8D2; no expansion board. Because the tolerance band in
 * ``ra8_adc_self_diagnose`` is a gross-fault detector (see the driver's
 * HUM Ch 69 note), the per-mode PASS/FAIL is logged verbatim so a bench
 * operator can read the raw codes even if the verdict trips.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_tsn.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_adc_diag_baud      = 115200U, /**< SCI8 console baud.              */
  k_adc_diag_period_ms = 500U,    /**< Delay between self-test cycles. */
} adc_diag_const_t;

/** @brief Formatting + iteration constants. */
typedef enum : uint8_t {
  k_adc_diag_num_modes = 3U,  /**< Self-diagnosis modes 1/2/3.           */
  k_adc_diag_radix     = 10U, /**< Decimal serialiser radix.             */
  k_adc_diag_dec_max   = 10U, /**< Max decimal digits for a uint32_t.    */
  k_adc_diag_minus     = '-', /**< ASCII minus for signed serialisation. */
} adc_diag_fmt_t;

/** @brief TSN factory-trim selection (Tj_max part). */
typedef enum : uint16_t {
  k_adc_diag_stab_us = 30U, /**< tTSTBL floor, HUM Figure 55.2 p 3502. */
} adc_diag_tsn_t;

/** @brief Ordered list of self-diagnosis modes to sweep each cycle. */
static const ra8_adc_selfdiag_mode_t k_adc_diag_modes[k_adc_diag_num_modes] = {
  k_ra8_adc_selfdiag_mode_1,
  k_ra8_adc_selfdiag_mode_2,
  k_ra8_adc_selfdiag_mode_3,
};

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_adc_diag_mode_prefix[]  = "diag: mode=";
static const uint8_t k_adc_diag_code_sep[]     = " code=";
static const uint8_t k_adc_diag_pass_sfx[]     = " PASS\r\n";
static const uint8_t k_adc_diag_fail_sfx[]     = " FAIL\r\n";
static const uint8_t k_adc_diag_tsn_prefix[]   = "tsn: die_mC=";
static const uint8_t k_adc_diag_vref_prefix[]  = "vref: raw=";
static const uint8_t k_adc_diag_crlf[]         = "\r\n";
static const uint8_t k_adc_diag_verdict_pass[] = "diag: selftest PASS\r\n";
static const uint8_t k_adc_diag_verdict_fail[] = "diag: selftest FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void adc_diag_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a byte span to the SCI8 console, discarding the status.
 *
 * @param[in] data Non-NULL byte span to transmit.
 * @param[in] len  Byte count (0 is a no-op).
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @since 0.1.0
 */
static void adc_diag_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Serialise an unsigned 32-bit value into decimal ASCII.
 *
 * @param[out] buf Destination, at least ``k_adc_diag_dec_max`` bytes.
 * @param[in]  val Value to serialise.
 *
 * @return Number of digits written (1..10).
 *
 * @pre ``buf`` is non-NULL and sized for the widest uint32_t.
 * @pre No trailing NUL is required by the caller.
 * @post ``buf`` holds the most-significant digit first.
 * @post The return value is in ``[1, k_adc_diag_dec_max]``.
 * @since 0.1.0
 */
static uint32_t adc_diag_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_adc_diag_dec_max];
  uint32_t n = 0U;
  uint32_t v = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_adc_diag_radix));
    v      = v / (uint32_t)k_adc_diag_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Log one unsigned 32-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @since 0.1.0
 */
static void adc_diag_write_u32(uint32_t val)
{
  uint8_t        buf[k_adc_diag_dec_max];
  const uint32_t n = adc_diag_u32_to_dec(buf, val);
  adc_diag_write(buf, n);
}

/**
 * @brief Log one signed 32-bit value (leading minus for negatives).
 *
 * @param[in] val Value to print (milli-degC range on this app).
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in an int32_t (always true).
 * @post For negatives a '-' precedes the magnitude digits.
 * @post The decimal representation of ``val`` has been queued.
 * @since 0.1.0
 */
static void adc_diag_write_i32(int32_t val)
{
  if (val < 0) {
    const uint8_t minus = (uint8_t)k_adc_diag_minus;
    adc_diag_write(&minus, 1U);
    adc_diag_write_u32((uint32_t)(-(int64_t)val));
  } else {
    adc_diag_write_u32((uint32_t)val);
  }
}

/**
 * @brief Emit one ``diag: mode=N code=CCCCC PASS|FAIL`` line.
 *
 * @param[in] mode_num Human mode number (1/2/3).
 * @param[in] code     Raw 16-bit self-diagnosis result code.
 * @param[in] pass     Driver verdict for this mode.
 *
 * @pre The console has been initialised.
 * @pre ``mode_num`` is one of 1/2/3.
 * @post A single newline-terminated line has been queued.
 * @post The PASS/FAIL suffix reflects ``pass``.
 * @since 0.1.0
 */
static void adc_diag_emit_mode(uint8_t mode_num, uint16_t code, bool pass)
{
  adc_diag_write(k_adc_diag_mode_prefix, (uint32_t)(sizeof(k_adc_diag_mode_prefix) - 1U));
  adc_diag_write_u32((uint32_t)mode_num);
  adc_diag_write(k_adc_diag_code_sep, (uint32_t)(sizeof(k_adc_diag_code_sep) - 1U));
  adc_diag_write_u32((uint32_t)code);
  if (pass) {
    adc_diag_write(k_adc_diag_pass_sfx, (uint32_t)(sizeof(k_adc_diag_pass_sfx) - 1U));
  } else {
    adc_diag_write(k_adc_diag_fail_sfx, (uint32_t)(sizeof(k_adc_diag_fail_sfx) - 1U));
  }
}

/**
 * @brief Run one self-test cycle: 3 self-diagnosis modes + temp + Vref.
 *
 * @return True iff every mode reported healthy and both internal reads
 *         returned ``k_ra8_ok``.
 *
 * @pre ``adc_diag_arm`` has initialised the ADC and TSN.
 * @pre The console has been initialised.
 * @post One log block (modes + temp + Vref) has been queued.
 * @post No ADC state other than the dedicated diagnostic group changed.
 * @since 0.1.0
 */
static bool adc_diag_run_cycle(void)
{
  bool healthy = true;
  for (uint32_t i = 0U; i < (uint32_t)k_adc_diag_num_modes; i++) {
    uint16_t        code = 0U;
    bool            pass = false;
    const ra8_err_t err  = ra8_adc_self_diagnose(k_adc_diag_modes[i], &code, &pass);
    if (err != k_ra8_ok) {
      healthy = false;
      pass    = false;
    }
    if (!pass) {
      healthy = false;
    }
    adc_diag_emit_mode((uint8_t)(i + 1U), code, pass);
  }

  int32_t         milli    = 0;
  const ra8_err_t temp_err = ra8_tsn_read_die_temp_milli_c(&milli);
  if (temp_err != k_ra8_ok) {
    healthy = false;
  }
  adc_diag_write(k_adc_diag_tsn_prefix, (uint32_t)(sizeof(k_adc_diag_tsn_prefix) - 1U));
  adc_diag_write_i32(milli);
  adc_diag_write(k_adc_diag_crlf, (uint32_t)(sizeof(k_adc_diag_crlf) - 1U));

  uint16_t        vref     = 0U;
  const ra8_err_t vref_err = ra8_adc_read_internal_channel(k_ra8_adc_chan_int_ref_volt, &vref);
  if (vref_err != k_ra8_ok) {
    healthy = false;
  }
  adc_diag_write(k_adc_diag_vref_prefix, (uint32_t)(sizeof(k_adc_diag_vref_prefix) - 1U));
  adc_diag_write_u32((uint32_t)vref);
  adc_diag_write(k_adc_diag_crlf, (uint32_t)(sizeof(k_adc_diag_crlf) - 1U));

  return healthy;
}

/**
 * @brief Core bring-up: CGC -> MSTP -> TIME -> console + LED.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock + console + LED is live.
 * @post Any failure parks the CPU via ``adc_diag_panic_halt``.
 * @since 0.1.0
 */
static void adc_diag_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    adc_diag_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    adc_diag_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    adc_diag_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    adc_diag_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_adc_diag_baud) != k_ra8_ok) {
    adc_diag_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    adc_diag_panic_halt();
  }
}

/**
 * @brief Initialise the ADC16H and the temperature sensor.
 *
 * @details
 * ``ra8_adc_init`` powers the converter (14-bit, software trigger); the
 * self-diagnosis + internal-channel paths force their own per-slot data
 * format, so the legacy init is sufficient. ``ra8_tsn_init`` routes the
 * sensor to the ADC mux and caches the 125 degC Tj_max trim -- the raw
 * calibration words are factory-programmed regardless, and the bench
 * step confirms the trim matches the shipped part (HUM Ch 55.3.1).
 *
 * @return ``ra8_err_t`` error code from the first failing init.
 * @retval k_ra8_ok ADC and TSN are ready.
 * @retval Other   Forwarded from ``ra8_adc_init`` or ``ra8_tsn_init``.
 *
 * @pre ``adc_diag_setup_or_halt`` has ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` the ADC is powered and TSCR.TSOE is set.
 * @post On failure no partial ADC state is relied upon by the caller.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t adc_diag_arm(void)
{
  const ra8_err_t adc_err = ra8_adc_init();
  if (adc_err != k_ra8_ok) {
    return adc_err;
  }
  const ra8_tsn_config_t cfg = {
    .high_ref_degc = k_ra8_tsn_cal_temp_high_125,
    .low_ref_degc  = k_ra8_tsn_cal_temp_low_n40,
    .stab_us       = (uint16_t)k_adc_diag_stab_us,
  };
  return ra8_tsn_init(&cfg);
}

void main(void)
{
  adc_diag_setup_or_halt();
  ra8_isr_globals_enable();

  if (adc_diag_arm() != k_ra8_ok) {
    adc_diag_panic_halt();
  }

  while (1) {
    const bool healthy = adc_diag_run_cycle();
    if (healthy) {
      adc_diag_write(k_adc_diag_verdict_pass, (uint32_t)(sizeof(k_adc_diag_verdict_pass) - 1U));
    } else {
      adc_diag_write(k_adc_diag_verdict_fail, (uint32_t)(sizeof(k_adc_diag_verdict_fail) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_adc_diag_period_ms);
  }
  adc_diag_panic_halt();
}
