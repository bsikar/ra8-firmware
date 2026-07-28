/**
 * @file examples/ek_ra8d2/hw_validated/hil/cac_accuracy_demo/main.c
 * @brief Clock Frequency Accuracy Measurement (CAC) demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The CAC counts edges of a *measurement target* clock during one period
 * of a divided *reference* clock and raises a frequency-error flag if the
 * count falls outside a programmed window. This demo measures the 24 MHz
 * main oscillator (CACMCLK) against the LOCO low-speed on-chip oscillator
 * (CACLCLK) divided by 32, i.e. a 1024 Hz reference window:
 *
 *   expected count = f_MAIN / (f_LOCO / 32) = 24e6 / 1024 = 23437
 *
 * The allowable window is set to that expected value +/- ~6 %, so a
 * healthy 24 MHz crystal passes (FERRF clear) while a stopped or grossly
 * detuned oscillator trips the frequency-error flag.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs. After ``ra8_cac_init`` programs
 * the limits, the demo writes CACR1 (target = MAIN) and CACR2 (reference =
 * LOCO /32, internal) directly, then once a second performs a measurement
 * and reports ``"cac: meas=ok ferr=0 ovf=0 ok=Y\r\n"`` on the J-Link OB
 * CDC channel. The raw count lands in ``g_cac_count`` for SWD probing.
 *
 * LED1 toggles on a healthy measurement; LED2 toggles on timeout / error.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers. Requires the
 * LOCO to be running (it is the default low-speed clock and feeds IWDT /
 * RTC); if the LOCO is stopped the reference never ticks and the
 * measurement times out.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` models the CAC
 * edge counter (``board_periph_cac.c``): a CACR0.CFME start latches CASTR.MENDF
 * and returns a CACNTBR inside the programmed [CALLVR, CAULVR] window, so
 * ``ra8_cac_measure`` completes with FERRF / OVFF clear and the banner reports
 * ``ok=Y`` (the ``ra8_emulator_smoke.sh`` gate keys on it). Confirmed on a real
 * EK-RA8D2 (2026-06-28): the real cross-clock edge count lands inside the
 * +/-6% window and the HIL gate is green -- the simulator proves the driver
 * start / poll / read-back sequence, silicon proves the real edge count. See
 * ``README.md`` for details.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cac.h"
#include "ra8_cac_regs.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "cac_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_cac_demo_baud      = 115200U, /**< SCI8 baud rate.             */
  k_cac_demo_period_ms = 1000U,   /**< Delay between measurements. */
} cac_demo_config_t;

/**
 * @brief Clock-pair arithmetic for the expected count + window.
 *
 * @details f_MAIN = 24 MHz main XTAL (HUM Ch 9 "CGC", USBCKCR note p 365);
 * f_LOCO = 32768 Hz; the reference is LOCO divided by 32 (CACR2.RCDS = 00).
 * expected = f_MAIN * 32 / f_LOCO. The window is +/- (expected >> 4)
 * (~6.25 %).
 */
typedef enum : uint32_t {
  k_cac_demo_main_hz   = 24000000U, /**< EK-RA8D2 main oscillator (CACMCLK). */
  k_cac_demo_loco_hz   = 32768U,    /**< LOCO oscillator (CACLCLK).          */
  k_cac_demo_ref_div   = 32U,       /**< CACR2.RCDS = 00 -> reference /32.   */
  k_cac_demo_tol_shift = 4U,        /**< Window half-width = expected >> 4.  */
} cac_demo_calc_t;

/**
 * @brief Direct CACR1 / CACR2 values selecting the MAIN-vs-LOCO clock pair.
 *
 * @details
 * ``ra8_cac_init`` leaves CACR1/CACR2 = 0; these must be written while
 * CACR0.CFME = 0 (HUM Ch 10.2.2 p 421 / 10.2.3 p 422 notes).
 *  - CACR1 = 0x00: FMCS = 000 (target = main osc CACMCLK), TCSS = 00
 *    (no target division), EDGES = 00 (rising), CACREFE = 0.
 *  - CACR2 = 0x09: RPS = 1 (internal reference), RSCS = 100 (reference =
 *    LOCO CACLCLK), RCDS = 00 (reference /32), DFS = 00 (no filter).
 */
typedef enum : uint8_t {
  k_cac_demo_cacr1_main = 0x00U, /**< Target = main osc, /1, rising.  */
  k_cac_demo_cacr2_loco = 0x09U, /**< Reference = LOCO /32, internal. */
} cac_demo_cacr_t;

/** @brief Output line tags. */
static const uint8_t k_cac_demo_ok_msg[]  = "cac: meas=ok ferr=0 ovf=0 ok=Y\r\n";
static const uint8_t k_cac_demo_bad_msg[] = "cac: meas=TIMEOUT-or-err ok=N\r\n";

/**
 * @var g_cac_count
 * @brief Raw CACNTBR count captured on the last successful measurement.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_cac_count = 0U;

/**
 * @var g_cac_ok
 * @brief 1 when the last measurement completed inside the window.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_cac_ok = 0U;

/**
 * @var g_cac_status
 * @brief Last CASTR snapshot (FERRF | MENDF | OVFF bits).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_cac_status = 0U;

/**
 * @var g_cac_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_cac_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void cac_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Expected edge count for the MAIN-vs-LOCO/32 window. */
static uint32_t cac_demo_expected(void)
{
  return ((uint32_t)k_cac_demo_main_hz * (uint32_t)k_cac_demo_ref_div) /
         (uint32_t)k_cac_demo_loco_hz;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void cac_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_cac_demo_baud) != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    cac_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    cac_demo_panic_halt();
  }
}

/**
 * @brief Programme CAC limits + the MAIN-vs-LOCO clock pair.
 *
 * @details
 * ``ra8_cac_init`` enables the CAC clock, clears CACR0.CFME, and loads the
 * +/-6% window; the demo then selects the clock pair by writing CACR1 +
 * CACR2 directly (the HAL keeps them at 0). Both writes are valid only
 * while CFME = 0, which ``ra8_cac_init`` guarantees on return.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra8_ok`` -- 2 vectors. No compound condition.
 *
 * @return ``ra8_err_t`` from ``ra8_cac_init``.
 * @pre CGC is up; IRQs masked or single-threaded init.
 * @post On success CACR1/CACR2 select MAIN target + LOCO/32 reference.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cac_demo_configure(void)
{
  const uint32_t expected = cac_demo_expected();
  const uint32_t tol      = expected >> (uint32_t)k_cac_demo_tol_shift;
  const uint16_t upper    = (uint16_t)(expected + tol);
  const uint16_t lower    = (uint16_t)(expected - tol);

  const ra8_err_t err = ra8_cac_init(upper, lower);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 10.2.2 "CACR1 : CAC Control Register 1" p 421 -- target =
   * main osc (FMCS=000), no division, rising edge. Valid while CFME=0. */
  ra8_cac()->CACR1 = (uint8_t)k_cac_demo_cacr1_main;
  /* HUM Ch 10.2.3 "CACR2 : CAC Control Register 2" p 422 -- internal
   * reference = LOCO (RSCS=100) divided by 32 (RCDS=00), no filter. */
  ra8_cac()->CACR2 = (uint8_t)k_cac_demo_cacr2_loco;
  return k_ra8_ok;
}

/**
 * @brief Run one measurement and fold the result into the verdict.
 *
 * @param[out] out_ok 1 when the measurement completed inside the window.
 *
 * @details
 * Healthy iff ``ra8_cac_measure`` returned ``k_ra8_ok`` (MENDF inside the
 * poll budget) AND neither the frequency-error (FERRF) nor the overflow
 * (OVFF) status bit is set.
 *
 * @par MC/DC:
 * Decision ``ok = meas_ok && !ferrf && !ovff`` (3 conditions). The host
 * test supplies N+1 = 4 vectors, varying each condition independently.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once the (bounded) attempt finishes,
 *         or the status-read error.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ::cac_demo_configure succeeded.
 * @post ``g_cac_count`` / ``g_cac_status`` updated; ``*out_ok`` is 0 or 1.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cac_demo_sample(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  uint16_t        count   = 0U;
  const ra8_err_t meas    = ra8_cac_measure(&count);
  const uint8_t   meas_ok = (meas == k_ra8_ok) ? 1U : 0U;

  uint8_t         status = 0U;
  const ra8_err_t serr   = ra8_cac_get_status(&status);
  if (serr != k_ra8_ok) {
    return serr;
  }
  g_cac_status = (uint32_t)status;
  if (meas_ok != 0U) {
    g_cac_count = (uint32_t)count;
  }
  const uint8_t ferrf = ((status & (uint8_t)k_ra8_cac_status_ferrf) != 0U) ? 1U : 0U;
  const uint8_t ovff  = ((status & (uint8_t)k_ra8_cac_status_ovff) != 0U) ? 1U : 0U;
  *out_ok             = (meas_ok != 0U && ferrf == 0U && ovff == 0U) ? 1U : 0U;
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  cac_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (ra8_emulator does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (cac_demo_configure() != k_ra8_ok) {
    cac_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = cac_demo_sample(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_cac_ok             = (uint32_t)good;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_cac_demo_ok_msg,
                                         (size_t)(sizeof(k_cac_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_cac_demo_bad_msg,
                                         (size_t)(sizeof(k_cac_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_cac_heartbeat;
    ra8_delay_ms(k_cac_demo_period_ms);
  }
  cac_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
