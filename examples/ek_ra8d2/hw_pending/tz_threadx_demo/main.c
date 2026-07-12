/**
 * @file main.c
 * @brief CPU0 (M85) Secure world fallback for the trustzone_threadx_demo.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Under normal execution, SystemInit runs ra8_trustzone_init which transitions
 * the CPU directly to the Non-Secure world to run ThreadX.
 *
 * If the transition fails, main() serves as a fallback.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/**
 * @var g_tz_threadx_demo_fallback_count
 * @brief Diagnostic counter incremented if Secure fallback main is reached.
 * @since 0.1.0
 */
volatile uint32_t g_tz_threadx_demo_fallback_count = 0U;

/**
 * @brief Secure fallback main entry point.
 * @return Never returns.
 * @since 0.1.0
 */
int main(void)
{
  g_tz_threadx_demo_fallback_count = 1U;

  for (;;) {
    __asm__ volatile("nop");
  }
}
