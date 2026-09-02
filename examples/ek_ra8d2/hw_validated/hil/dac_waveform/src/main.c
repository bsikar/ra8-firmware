/**
 * @file examples/ek_ra8d2/hw_validated/hil/dac_waveform/src/main.c
 * @brief 12-bit DAC_B triangle-wave generator for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Drives DAC_B channel 0 (DAC0OUT pin) with a triangle wave spanning
 * the full 12-bit code range (0..4095). The ramp uses 64 steps with
 * 1 ms between samples, so a single period (up + down) is ~128 ms
 * (~8 Hz visible). Probe with an oscilloscope on the DAC0 output pad
 * of the EK-RA8D2.
 *
 * The slow rate is dictated by the millisecond-granularity HAL delay
 * (``ra8_delay_ms``) -- there is no microsecond busy-wait helper in
 * ``libs/ra8_core/ra8_time`` yet. Adding one is tracked in
 * ``docs/ROADMAP.md``; once available this demo's step period drops
 * to 16 us for an audible 1 kHz triangle.
 *
 * Sequence:
 *   1. CGC + SysTick bring-up.
 *   2. ``ra8_dac_b_init_configured`` with NORMAL VREFH, right-justified
 *      12-bit data, channel 0 enabled, internal output disabled.
 *   3. Loop: walk ``value`` from 0 to 4095 in N_STEPS increments,
 *      then back down. ``ra8_dac_b_write`` updates the channel
 *      every iteration; ``ra8_delay_us`` paces the step.
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
#include "ra8_dac_b.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Channel + sample-rate tunables. */
typedef enum : uint16_t {
  k_dac_demo_channel  = 0U,    /**< DAC demo channel.                            */
  k_dac_demo_steps    = 64U,   /**< DAC demo steps.                              */
  k_dac_demo_step_ms  = 1U,    /**< 1 ms x 64 x 2 = 128 ms full triangle period. */
  k_dac_demo_max_code = 4095U, /**< DAC demo maximum code.                       */
} dac_demo_const_t;

/**
 * @var g_dac_waveform_tick
 * @brief HIL liveness counter -- incremented once per triangle period.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD; the script
 * halts the chip, samples this value, lets the chip run for N seconds,
 * halts again, and asserts the delta >= HIL_PROBE_MIN_ADVANCE. Catches
 * the "PC is in MRAM but main loop never iterated" failure mode that
 * the plain HIL_MODE=alive check misses.
 *
 * `volatile` keeps the increment alive under optimization; the global
 * (non-static) keeps the symbol linker-visible without --gc-sections
 * culling.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_dac_waveform_tick = 0U;

/**
 * @brief Park the processor after an unrecoverable DAC waveform failure.
 *
 * @details Preserves the analog-generator state in a permanent
 *          wait-for-interrupt loop for debugger inspection.
 *
 * @return None.
 *
 * @pre The caller has determined that waveform generation cannot continue.
 * @pre Any desired diagnostic state has already been recorded.
 * @post The function never returns to its caller.
 * @post No more DAC samples or liveness increments are produced.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dac_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + DAC_B0 up. Halts on any error.
 *
 * @details Initializes the system clock and millisecond time base, then
 *          configures DAC_B channel 0 for right-aligned samples on the normal
 *          reference. A failed dependency enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre DAC_B channel 0 is not owned by another context.
 * @post On success the time base and DAC channel 0 are initialized.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dac_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_dac_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_dac_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_dac_demo_panic_halt();
  }
  const ra8_dac_b_cfg_t cfg = {
    .vref                    = k_ra8_dac_b_vref_normal,
    .data_format             = k_ra8_dac_b_format_right,
    .internal_output_enabled = false,
    .enable_channel0         = true,
    .enable_channel1         = false,
  };
  if (ra8_dac_b_init_configured(&cfg) != k_ra8_ok) {
    internal_dac_demo_panic_halt();
  }
}

/**
 * @brief Emit one full ramp-up + ramp-down cycle.
 *
 * @details Writes evenly spaced codes from zero toward full scale and back to
 *          zero, delaying between samples to produce one triangle period.
 *
 * @par MC/DC:
 * Compound decision: ``write != ok`` checked twice (up + down).
 * Two atomic conditions x N+1 = 3 vectors -- both ok (golden case),
 * up-write fails, down-write fails (covered in test_app_dac_waveform.c).
 *
 * @return ra8_err_t Result of generating the complete waveform period.
 * @retval k_ra8_ok Every sample in both ramps was accepted.
 * @retval k_ra8_err_hw_error A DAC write in either ramp failed.
 *
 * @pre ::internal_dac_demo_setup_or_halt initialized DAC_B channel 0.
 * @pre This context exclusively owns waveform writes for the duration.
 * @post On success the output completes an up-ramp and down-ramp.
 * @post On failure no later sample in the period is attempted.
 *
 * @note Blocking helper; each sample includes the configured millisecond delay.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_dac_demo_one_triangle_period(void)
{
  const uint16_t step = (uint16_t)((uint32_t)k_dac_demo_max_code / (uint32_t)k_dac_demo_steps);
  for (uint16_t i = 0U; i < (uint16_t)k_dac_demo_steps; ++i) {
    const uint16_t code = (uint16_t)(i * step);
    if (ra8_dac_b_write((uint8_t)k_dac_demo_channel, code) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    ra8_delay_ms((uint32_t)k_dac_demo_step_ms);
  }
  for (uint16_t i = (uint16_t)k_dac_demo_steps; i > 0U; --i) {
    const uint16_t code = (uint16_t)((i - 1U) * step);
    if (ra8_dac_b_write((uint8_t)k_dac_demo_channel, code) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    ra8_delay_ms((uint32_t)k_dac_demo_step_ms);
  }
  return k_ra8_ok;
}

void main(void)
{
  internal_dac_demo_setup_or_halt();
  ra8_isr_globals_enable();
  while (1) {
    if (internal_dac_demo_one_triangle_period() != k_ra8_ok) {
      break;
    }
    g_dac_waveform_tick += 1U;
  }
  internal_dac_demo_panic_halt();
}
