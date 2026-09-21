/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_pwm_demo/src/main.c
 * @brief GPT PWM duty-cycle LED demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Drives an LED off GPT channel ``k_gpt_pwm_demo_channel`` in saw-wave
 * PWM mode. Each main-loop iteration:
 *
 *   1. Programmes a new GTCCRA duty value via ``ra8_gpt_set_duty``.
 *   2. Walks the duty cycle linearly from 0 to ``period`` and back so
 *      the LED visibly breathes at roughly 1 Hz.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 init.
 *   - ``ra8_gpt_init`` with ``mode = saw_pwm`` + ``auto_start = true``.
 *   - Loop forever stepping ``ra8_gpt_set_duty`` at
 *     ``k_gpt_pwm_demo_step_ms``.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_pwm_demo_period   = 0x0000FFFFU, /**< 16-bit period (~61 Hz at 4 MHz / 4).       */
  k_gpt_pwm_demo_step_ms  = 20U,         /**< Per-step delay -> ~1.3 s sweep.            */
  k_gpt_pwm_demo_step_pct = 1024U,       /**< Duty step (>= 64 to stay in 16-bit range). */
} gpt_pwm_demo_const_t;

/** @brief GPT channel. */
typedef enum : uint8_t {
  k_gpt_pwm_demo_channel = 0U, /**< GPT PWM demo channel. */
} gpt_pwm_demo_chan_t;

/**
 * @var g_gpt_pwm_match
 * @brief HIL liveness counter -- incremented every iteration in which
 *        GTCNT advanced relative to the previous sample AND the new
 *        duty value was accepted by ``ra8_gpt_set_duty``.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the GPT peripheral is actually counting
 * (not just that we configured it without faulting). If the GPT were
 * wedged or the clock gate were off, GTCNT would never change and
 * this counter would stop advancing.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_pwm_match = 0U;

/**
 * @var g_gpt_pwm_mismatch
 * @brief HIL failure counter -- incremented whenever a sub-step fails:
 *        ``ra8_gpt_set_duty`` returned non-ok, ``ra8_gpt_read`` returned
 *        non-ok, or GTCNT did not advance between two samples taken
 *        ``k_gpt_pwm_demo_step_ms`` apart.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches "GPT clock gate closed silently",
 * "GTCNT frozen at 0", and "duty write rejected at runtime" -- none
 * of which alive-mode could detect because the main loop never
 * breaks on these errors.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_pwm_mismatch = 0U;

/**
 * @brief Park the processor after an unrecoverable PWM demo failure.
 *
 * @details Enters a permanent wait-for-interrupt loop so the GPT counter,
 *          duty, and HIL counters remain available to a debugger.
 *
 * @return None.
 *
 * @pre The caller has determined the PWM validation cannot continue.
 * @pre Any mismatch counter update required by the failure is complete.
 * @post The function never returns to its caller.
 * @post No further duty updates or liveness increments occur.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_pwm_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, timing, and the PWM demo status LED.
 *
 * @details Starts CGC and the millisecond time base, then claims LED1 before
 *          GPT configuration. Any failed dependency enters the permanent
 *          panic halt rather than exposing partial setup.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre LED1 is available for exclusive use by this image.
 * @post On success the time base and LED1 are ready for the PWM loop.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_pwm_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_gpt_pwm_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_gpt_pwm_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_gpt_pwm_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_gpt_pwm_demo_panic_halt();
  }
}

/**
 * @brief Configure GPT0 for saw-wave PWM with ``auto_start = true``.
 *
 * @details Selects the divide-by-four source, configured period, zero initial
 *          duty on both compare channels, and immediate counter start.
 *
 * @par MC/DC:
 * Decision: ``ra8_gpt_init != ok``. One atomic condition x 2 vectors --
 * golden (this) + null-cfg reject (covered in test_app_gpt_pwm_demo.c).
 *
 * @return ra8_err_t Status from GPT channel initialization.
 * @retval k_ra8_ok GPT0 was configured and started in saw-PWM mode.
 * @retval (other)  The GPT driver rejected or could not apply the configuration.
 *
 * @pre ::internal_gpt_pwm_demo_setup_or_halt completed successfully.
 * @pre GPT channel 0 is not owned by another driver.
 * @post On success GPT0 counts with both duties initially zero.
 * @post On failure no runtime duty update is attempted by this helper.
 *
 * @note Runtime duty sweeps and counter checks remain the caller's responsibility.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_gpt_pwm_demo_arm(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_gpt_pwm_demo_period,
    .duty_a     = (uint32_t)0U,
    .duty_b     = (uint32_t)0U,
    .auto_start = true,
  };
  return ra8_gpt_init((uint8_t)k_gpt_pwm_demo_channel, &cfg);
}

void main(void)
{
  internal_gpt_pwm_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_gpt_pwm_demo_arm() != k_ra8_ok) {
    internal_gpt_pwm_demo_panic_halt();
  }

  uint32_t duty       = 0U;
  bool     up         = true;
  uint32_t prev_count = 0U;
  bool     have_prev  = false;
  while (1) {
    if (ra8_gpt_set_duty((uint8_t)k_gpt_pwm_demo_channel, k_ra8_gpt_ccr_a, duty) != k_ra8_ok) {
      g_gpt_pwm_mismatch += 1U;
      break;
    }
    uint32_t now_count = 0U;
    if (ra8_gpt_read((uint8_t)k_gpt_pwm_demo_channel, &now_count) != k_ra8_ok) {
      g_gpt_pwm_mismatch += 1U;
    } else if (have_prev && (now_count == prev_count)) {
      /* 20 ms passed but GTCNT did not move -- timer is wedged. */
      g_gpt_pwm_mismatch += 1U;
      prev_count = now_count;
    } else {
      g_gpt_pwm_match += 1U;
      prev_count = now_count;
      have_prev  = true;
    }
    if (up) {
      if (duty + (uint32_t)k_gpt_pwm_demo_step_pct >= (uint32_t)k_gpt_pwm_demo_period) {
        duty = (uint32_t)k_gpt_pwm_demo_period;
        up   = false;
      } else {
        duty += (uint32_t)k_gpt_pwm_demo_step_pct;
      }
    } else {
      if (duty <= (uint32_t)k_gpt_pwm_demo_step_pct) {
        duty = 0U;
        up   = true;
      } else {
        duty -= (uint32_t)k_gpt_pwm_demo_step_pct;
      }
    }
    ra8_delay_ms((uint32_t)k_gpt_pwm_demo_step_ms);
  }
  internal_gpt_pwm_demo_panic_halt();
}
