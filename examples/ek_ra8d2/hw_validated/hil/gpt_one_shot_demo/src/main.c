/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_one_shot_demo/src/main.c
 * @brief GPT one-shot (saw-wave one-shot) HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises a one-shot timer through the board's timer port: board
 * timer 0 (GPT0 on this board, saw-wave one-shot, HUM Ch 25.2.1
 * GTCR.MD = 001b) counts up from 0 to the period once, reports the
 * wrap, then stops on its own. The demo starts, polls for the wrap
 * and restarts in a loop so the bench can scrape
 * ``g_gpt_one_shot_match`` and confirm each one-shot completes.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 init.
 *   - ``fw_timer_open`` on ``ra8_board_timer()`` in one-shot mode.
 *   - Loop: ``fw_timer_start`` -> poll ``fw_timer_take_wrap`` -> repeat.
 *
 * No GPT register or driver call remains here; which chip channel and
 * clock divider back board timer 0 is the board profile's business.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_gpt_profile.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_os_demo_period         = 0x0000FFFFU, /**< Wrap point, in counts.   */
  k_gpt_os_demo_rearm_delay_ms = 50U,         /**< Sleep between one-shots. */
} gpt_os_demo_const_t;

/** @brief Board timer, a dense board index rather than a chip channel. */
static const fw_timer_ch_t k_gpt_os_demo_timer = {.index = 0U};

/**
 * @var g_gpt_one_shot_match
 * @brief HIL liveness counter -- bumped on every observed one-shot
 *        completion. Read externally by hil_jlink_memprobe.sh.
 *
 * @details If the timer's clock is gated or its mode is wrong, the
 * wrap is never reported and this counter stops advancing.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_one_shot_match = 0U;

/**
 * @var g_gpt_one_shot_mismatch
 * @brief HIL failure counter -- bumped when arm-or-start returns
 *        non-ok inside the loop.
 *
 * @details The memprobe asserts this stays at 0. Catches "clock gate
 * closed silently" and "the timer port refused the start".
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_one_shot_mismatch = 0U;

/**
 * @brief Park the core after an unrecoverable demo failure.
 *
 * @details Repeatedly executes WFI so a debugger can inspect the failed GPT
 *          or clock state without further peripheral accesses.
 *
 * @pre Called only from boot failure or an unreachable terminal path.
 * @pre The caller does not require recovery without an external reset.
 * @post The core remains in the WFI loop until reset or debug intervention.
 * @post No demo counters are modified after entry.
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
 * @brief Initialize clocks, the delay service, and LED1.
 *
 * @details Brings dependencies up in order and parks immediately if any HAL
 *          operation fails, leaving GPT configuration to ``internal_arm``.
 *
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, the delay service and LED1 are ready for the one-shot loop.
 * @post Board timer 0 stays closed until ``internal_arm`` runs.
 * @note Not thread-safe; it mutates global board and clock state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  const fw_clock_module_t clk_core = {.kind = k_fw_clock_module_core, .index = 0U};
  if (fw_clock_rate_for(ra8_board_clock(), clk_core, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Open board timer 0 as a one-shot.
 *
 * @details Asks the board's timer port for a one-shot channel with the fixed
 *          demo period, left stopped. The board profile picks the chip
 *          channel and the clock divider; the port checks the period against
 *          the counter width before the chip sees it.
 *
 * @return ra8_err_t from ``fw_timer_open``.
 * @retval k_ra8_ok Board timer 0 is open in one-shot mode, not running.
 * @retval (other)  The port or the board refused the open.
 *
 * @pre ``internal_setup_or_halt`` completed.
 * @pre Board timer 0 is not open.
 * @post On k_ra8_ok the timer is open and stopped.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_arm(void)
{
  return fw_timer_open(ra8_board_timer(),
                       k_gpt_os_demo_timer,
                       k_fw_timer_mode_one_shot,
                       (uint32_t)k_gpt_os_demo_period);
}

/**
 * @brief Poll for the one-shot's wrap, bounded.
 *
 * @details Asks the timer port up to the fixed poll budget whether the count
 *          reached its period; the port clears the report as it answers.
 *          A port failure counts the same as exhaustion so the caller records
 *          a mismatch.
 *
 * @return true if the wrap was reported within budget.
 * @retval true  Wrap reported and cleared.
 * @retval false Poll budget elapsed, or the port refused.
 *
 * @pre Board timer 0 was started.
 * @pre IRQs are not required (poll-only path).
 * @post On true the wrap report has been taken.
 * @post Iteration count bounded.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_wait_ovf(void)
{
  enum : uint32_t { k_poll_budget = 200000U /**< Poll budget. */ };
  for (uint32_t i = 0U; i < k_poll_budget; ++i) {
    bool wrapped = false;
    if (fw_timer_take_wrap(ra8_board_timer(), k_gpt_os_demo_timer, &wrapped) != k_ra8_ok) {
      return false;
    }
    if (wrapped) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Repeatedly start and verify one-shot intervals on board timer 0.
 *
 * @details Opens board timer 0 once, then starts, polls, records the
 *          exported pass/failure counters, toggles LED1 on completion, and
 *          delays before the next one-shot.
 *
 * @pre Reset startup and SystemInit completed successfully.
 * @pre Board timer 0 and LED1 are not owned by another execution context.
 * @post Each completed one-shot increments ``g_gpt_one_shot_match``.
 * @post Each start or bounded-poll failure increments the mismatch counter.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_arm() != k_ra8_ok) {
    internal_panic_halt();
  }

  while (1) {
    if (fw_timer_start(ra8_board_timer(), k_gpt_os_demo_timer) != k_ra8_ok) {
      g_gpt_one_shot_mismatch += 1U;
      ra8_delay_ms((uint32_t)k_gpt_os_demo_rearm_delay_ms);
      continue;
    }
    if (internal_wait_ovf()) {
      g_gpt_one_shot_match += 1U;
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      g_gpt_one_shot_mismatch += 1U;
    }
    ra8_delay_ms((uint32_t)k_gpt_os_demo_rearm_delay_ms);
  }
  internal_panic_halt();
}
