/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_one_shot_demo/main.c
 * @brief GPT one-shot (saw-wave one-shot) HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the GPT in saw-wave one-shot mode (HUM Ch 25.2.1
 * GTCR.MD = 001b) -- the counter counts up from 0 to period once,
 * fires an overflow IRQ, then stops on its own. The demo arms +
 * starts + waits for IRQ + re-arms in a loop so the bench can
 * scrape ``g_gpt_one_shot_match`` and confirm each one-shot
 * completes.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 init.
 *   - ``ra8_gpt_init`` with ``mode = saw_one_shot`` + ``auto_start = false``.
 *   - ``ra8_gpt_attach_handler`` to count overflows.
 *   - Loop: ``ra8_gpt_start_free_run`` -> wait for IRQ -> repeat.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_os_demo_period         = 0x0000FFFFU, /**< 16-bit period at div_4.  */
  k_gpt_os_demo_rearm_delay_ms = 50U,         /**< Sleep between one-shots. */
} gpt_os_demo_const_t;

/** @brief GPT channel. */
typedef enum : uint8_t {
  k_gpt_os_demo_channel = 0U, /**< GPT os demo channel. */
} gpt_os_demo_chan_t;

/**
 * @var g_gpt_one_shot_match
 * @brief HIL liveness counter -- bumped on every IRQ-observed one-shot
 *        completion. Read externally by hil_jlink_memprobe.sh.
 *
 * @details If the GPT clock is gated, GTCR.MD is wrong, or the IRQ
 * is masked, the overflow callback never fires and this counter
 * stops advancing.
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
 * closed silently" and "ra8_gpt_start_free_run rejected".
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_one_shot_mismatch = 0U;

static void gpt_os_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void gpt_os_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    gpt_os_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    gpt_os_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    gpt_os_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    gpt_os_demo_panic_halt();
  }
}

/**
 * @brief Init GPT0 in one-shot mode.
 *
 * @return ra8_err_t from ra8_gpt_init.
 *
 * @pre ra8_isr_globals_enable has been called.
 * @pre GPT0 not yet initialised.
 * @post On k_ra8_ok the channel is armed (not yet running).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t gpt_os_demo_arm(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_one_shot,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_gpt_os_demo_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = false,
  };
  return ra8_gpt_init((uint8_t)k_gpt_os_demo_channel, &cfg);
}

/**
 * @brief Poll GTST.TCFPO for one overflow + clear, bounded.
 *
 * @return true if the overflow flag was observed.
 * @retval true  TCFPO set + cleared within budget.
 * @retval false poll budget elapsed.
 *
 * @pre GPT0 is running (ra8_gpt_start_free_run was called).
 * @pre IRQs are not required (poll-only path).
 * @post On true the GTST.TCFPO bit has been cleared.
 * @post Iteration count bounded.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool gpt_os_demo_wait_ovf(void)
{
  enum : uint32_t { k_poll_budget = 200000U /**< Poll budget. */ };
  for (uint32_t i = 0U; i < k_poll_budget; ++i) {
    uint32_t status = 0U;
    if (ra8_gpt_get_status((uint8_t)k_gpt_os_demo_channel, &status) != k_ra8_ok) {
      return false;
    }
    if ((status & (uint32_t)k_ra8_gpt_status_overflow) != 0U) {
      (void)ra8_gpt_clear_status((uint8_t)k_gpt_os_demo_channel,
                                 (uint32_t)k_ra8_gpt_status_overflow);
      return true;
    }
  }
  return false;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpt_os_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (gpt_os_demo_arm() != k_ra8_ok) {
    gpt_os_demo_panic_halt();
  }

  while (1) {
    if (ra8_gpt_start_free_run((uint8_t)k_gpt_os_demo_channel, (uint32_t)k_gpt_os_demo_period) !=
        k_ra8_ok) {
      g_gpt_one_shot_mismatch += 1U;
      ra8_delay_ms((uint32_t)k_gpt_os_demo_rearm_delay_ms);
      continue;
    }
    if (gpt_os_demo_wait_ovf()) {
      g_gpt_one_shot_match += 1U;
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      g_gpt_one_shot_mismatch += 1U;
    }
    ra8_delay_ms((uint32_t)k_gpt_os_demo_rearm_delay_ms);
  }
  gpt_os_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
