/**
 * @file main.c
 * @brief SW1 -> LED1 mirror demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + LED1 + on-board user switch SW1
 * (P009, EK-RA8D2 UM Table 25 p 32). Every 100 ms polls SW1
 * via ``ra_board_sw_read`` and mirrors its pressed/released
 * state to LED1 (LED1 lit while SW1 held). Bare EVM only --
 * no expansion board, no console.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpio_demo_poll_ms = 100U,
} gpio_demo_const_t;

/** @brief Park the CPU after a fatal init failure. */
static void gpio_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LED1 + SW1 up. Halts on any error.
 *
 * @pre Reset_Handler has set up the C runtime.
 * @post LED1 is configured as output (off); SW1 is input + pull-up.
 *
 * @since 0.1.0
 */
static void gpio_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    gpio_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    gpio_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    gpio_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    gpio_demo_panic_halt();
  }
  if (ra_board_sw_init(k_ra_board_sw1) != k_ra_ok) {
    gpio_demo_panic_halt();
  }
}

/**
 * @brief One poll iteration: read SW1, drive LED1.
 *
 * @par MC/DC:
 * Compound decision: ``read != ok``. One atomic condition x 2
 * vectors (read-ok in steady state, read-fail in
 * test_app_gpio_input_demo via mock injection).
 *
 * @param[out] out_state Receives the latest switch state.
 * @return Error code from ra_board_sw_read or ra_board_led_*.
 *
 * @pre out_state non-NULL.
 * @post LED1 reflects *out_state on success.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t gpio_demo_one_iter(ra_board_sw_state_t* out_state)
{
  ra_err_t err = ra_board_sw_read(k_ra_board_sw1, out_state);
  if (err != k_ra_ok) {
    return err;
  }
  if (*out_state == k_ra_board_sw_pressed) {
    return ra_board_led_on(k_ra_board_led1);
  }
  return ra_board_led_off(k_ra_board_led1);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpio_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    ra_board_sw_state_t s = k_ra_board_sw_released;
    if (gpio_demo_one_iter(&s) != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_gpio_demo_poll_ms);
  }
  gpio_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
