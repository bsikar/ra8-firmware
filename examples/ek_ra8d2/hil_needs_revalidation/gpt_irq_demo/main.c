/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/gpt_irq_demo/main.c
 * @brief GPT0 overflow interrupt -> NVIC -> ISR demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Proves the full real interrupt path -- a peripheral event linked through the
 * ICU IELSR table, pended on the NVIC, and taken as a Cortex-M exception that
 * runs an application ISR. Unlike the poll-based timer demos (agt_periodic,
 * gpt_one_shot_demo), this app does NOT poll a status flag: it registers a
 * handler for the GPT0 counter-overflow event with ``ra8_isr_register`` (which
 * writes IELSR and enables the NVIC line), starts GPT0 in saw-wave PWM mode,
 * and then sleeps. Every overflow fires ``internal_gpt_irq_demo_isr`` in handler mode,
 * which increments ``g_gpt_irq_count`` and toggles board LED1. The main loop
 * itself never touches the LED, so any LED transition is proof the real ISR ran.
 *
 * Sequence:
 *   1. CGC + SysTick + LED1 bring-up (panic-halt on any error).
 *   2. ``ra8_isr_init`` then ``ra8_isr_register(GPT0 overflow, isr)``.
 *   3. ``ra8_gpt_init`` (saw PWM) + ``ra8_gpt_start_free_run``.
 *   4. Loop: ``ra8_delay_ms`` -- all work happens in the ISR.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_elc.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_irq_demo_period   = 0x0000FFFFU, /**< 16-bit period at div_1.         */
  k_gpt_irq_demo_loop_ms  = 100U,        /**< Idle sleep between iterations.  */
  k_gpt_irq_demo_priority = 3U,          /**< NVIC priority for the overflow. */
} gpt_irq_demo_const_t;

/** @brief GPT channel and its counter-overflow ELC event (FSP ra8d2 0x0C1). */
typedef enum : uint16_t {
  k_gpt_irq_demo_channel   = 0U,     /**< GPT IRQ demo channel.              */
  k_gpt_irq_demo_ovf_event = 0x0C1U, /**< GPT0 GTCIV counter-overflow event. */
} gpt_irq_demo_event_t;

/**
 * @var g_gpt_irq_count
 * @brief Count of GPT0 overflow interrupts actually serviced by the ISR.
 *
 * @details
 * Incremented only inside ``internal_gpt_irq_demo_isr``. If the ICU event link, the
 * NVIC enable, or the vector dispatch were broken this would never advance --
 * so a non-zero value is direct evidence the real NVIC interrupt path ran.
 * Read externally by J-Link (HIL) and by tools/ra8_emulator (emulator).
 *
 * @note Read externally only; the firmware main loop never reads it back.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_irq_count = 0U;

/**
 * @brief Park the processor after an unrecoverable demo failure.
 * @details Enters a permanent wait-for-interrupt loop so the failing state and
 *          liveness counter remain available to a debugger or HIL probe.
 * @pre Reset startup initialized the stack and exception environment.
 * @pre The caller has determined that normal demo execution cannot continue.
 * @post Control never returns to the caller.
 * @post No further application-level state transition is attempted.
 * @note An interrupt can wake one iteration, but the terminal loop resumes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_irq_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief GPT0 counter-overflow interrupt service routine.
 *
 * @details
 * Runs in NVIC handler mode, reached via the real path: GPT0 overflow ->
 * IELSR event link -> NVIC pend -> vector 16 + IRQ -> ra8_isr_dispatch -> here.
 * Bumps the liveness counter and toggles LED1 so the effect is observable both
 * on hardware (LED + memprobe) and in the emulator (LED transition + IRQ count).
 *
 * @param[in] ctx Unused registration context.
 * @return Nothing.
 *
 * @pre Registered for the GPT0 overflow event via ra8_isr_register.
 * @pre @p ctx is the unused registration context supplied by this application.
 * @post g_gpt_irq_count has advanced by one and LED1 has toggled.
 * @post No GPT configuration or interrupt routing is modified.
 *
 * @note Not re-entrant; a single GPT channel drives it.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_irq_demo_isr(void* ctx)
{
  (void)ctx;
  g_gpt_irq_count += 1U;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
}

/**
 * @brief Initialize the clocks, timebase, and observable LED fixture.
 * @details Brings dependencies up in order and treats any failure as terminal,
 *          leaving interrupt routing disabled until the separate arm step.
 * @pre Reset startup completed data and BSS initialization.
 * @pre Board clock and GPIO registers are in their reset-compatible state.
 * @post On return the CPU clock rate is known, the timebase runs, and LED1 is initialized.
 * @post Any failed prerequisite has entered the terminal panic loop.
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_irq_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_gpt_irq_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_gpt_irq_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_gpt_irq_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_gpt_irq_demo_panic_halt();
  }
}

/**
 * @brief Link the GPT0 overflow event to the NVIC and arm GPT0.
 *
 * @return k_ra8_ok on success, else the first failing step's error.
 *
 * @pre internal_gpt_irq_demo_setup_or_halt has run; interrupts not yet enabled.
 * @post On k_ra8_ok GPT0 is counting and its overflow vectors to the ISR.
 *
 * @note Not thread-safe (single-threaded boot context).
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_gpt_irq_demo_arm(void)
{
  ra8_err_t err = ra8_isr_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_isr_register((ra8_elc_event_t)k_gpt_irq_demo_ovf_event,
                         internal_gpt_irq_demo_isr,
                         nullptr,
                         (uint8_t)k_gpt_irq_demo_priority,
                         nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = (uint32_t)k_gpt_irq_demo_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = false,
  };
  err = ra8_gpt_init((uint8_t)k_gpt_irq_demo_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpt_start_free_run((uint8_t)k_gpt_irq_demo_channel, (uint32_t)k_gpt_irq_demo_period);
}

void main(void)
{
  internal_gpt_irq_demo_setup_or_halt();

  if (internal_gpt_irq_demo_arm() != k_ra8_ok) {
    internal_gpt_irq_demo_panic_halt();
  }

  ra8_isr_globals_enable();

  while (1) {
    /* All observable work happens in internal_gpt_irq_demo_isr; the loop only idles. */
    ra8_delay_ms((uint32_t)k_gpt_irq_demo_loop_ms);
  }

  internal_gpt_irq_demo_panic_halt();
}
