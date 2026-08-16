/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpio_input_demo/main.c
 * @brief SW1 -> LED1 mirror demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + LED1 + on-board user switch SW1
 * (P009, EK-RA8D2 UM Table 25 p 32). Every 100 ms polls SW1
 * via ``ra8_board_sw_read`` and mirrors its pressed/released
 * state to LED1 (LED1 lit while SW1 held). Bare EVM only --
 * no expansion board, no console.
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
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpio_demo_poll_ms = 100U, /**< GPIO demo poll ms. */
} gpio_demo_const_t;

/**
 * @var g_gpio_input_tick
 * @brief HIL liveness counter -- incremented each poll iteration.
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
volatile uint32_t g_gpio_input_tick = 0U;

/**
 * @brief Park the CPU after a fatal GPIO demo failure.
 *
 * @details Enters a permanent wait-for-interrupt loop, preserving the most
 *          recent switch and LED state for an attached debugger.
 *
 * @return None.
 *
 * @pre The caller has determined the polling loop cannot safely continue.
 * @pre Any desired diagnostic output has already been requested.
 * @post The function never returns to its caller.
 * @post No further switch reads, LED writes, or liveness increments occur.
 *
 * @note Interrupt wakeups return immediately to the permanent loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpio_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LED1 + SW1 up. Halts on any error.
 *
 * @details Initializes the clock tree and millisecond time base, then claims
 *          LED1 as the output indicator and SW1 as the pulled-up input. Any
 *          failing step transfers control to the permanent fatal halt.
 *
 * @return None.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @pre LED1 and SW1 are available for exclusive use by this image.
 * @post LED1 is configured as output (off); SW1 is input + pull-up.
 * @post On any setup error the function does not return.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpio_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_gpio_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_gpio_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_gpio_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_gpio_demo_panic_halt();
  }
  if (ra8_board_sw_init(k_ra8_board_sw1) != k_ra8_ok) {
    internal_gpio_demo_panic_halt();
  }
}

/**
 * @brief One poll iteration: read SW1, drive LED1.
 *
 * @details Samples the board switch into caller-owned storage, turns LED1 on
 *          for the pressed state and off for the released state, and preserves
 *          the first board-support error.
 *
 * @par MC/DC:
 * Compound decision: ``read != ok``. One atomic condition x 2
 * vectors (read-ok in steady state, read-fail in
 * test_app_gpio_input_demo via mock injection).
 *
 * @param[out] out_state Receives the latest switch state.
 *
 * @return Error code from ra8_board_sw_read or ra8_board_led_*.
 * @retval k_ra8_ok The switch was sampled and LED1 was updated.
 * @retval (other)  The switch read or requested LED operation failed.
 *
 * @pre @p out_state is non-NULL writable storage.
 * @pre ::internal_gpio_demo_setup_or_halt completed successfully.
 * @post LED1 reflects *out_state on success.
 * @post On a switch-read error LED1 is not modified.
 *
 * @note Polling helper; the caller controls its invocation cadence.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t
internal_gpio_demo_one_iter(ra8_board_sw_state_t* out_state)
{
  ra8_err_t err = ra8_board_sw_read(k_ra8_board_sw1, out_state);
  if (err != k_ra8_ok) {
    return err;
  }
  if (*out_state == k_ra8_board_sw_pressed) {
    return ra8_board_led_on(k_ra8_board_led1);
  }
  return ra8_board_led_off(k_ra8_board_led1);
}

void main(void)
{
  internal_gpio_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    ra8_board_sw_state_t s = k_ra8_board_sw_released;
    if (internal_gpio_demo_one_iter(&s) != k_ra8_ok) {
      break;
    }
    g_gpio_input_tick += 1U;
    ra8_delay_ms((uint32_t)k_gpio_demo_poll_ms);
  }
  internal_gpio_demo_panic_halt();
}
