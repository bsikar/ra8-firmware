/**
 * @file main.c
 * @brief CPU0 (M85) Secure world bootloader/fallback for the e-reader.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * On normal execution, this function is not reached. SystemInit calls
 * ra8_trustzone_init, which configures the SAU regions and transitions the CPU
 * to the Non-Secure world to run the actual e-reader firmware.
 *
 * If the transition fails or is bypassed (e.g. on off-target builds),
 * main() runs as a fallback, increments a diagnostic counter, and parks
 * the CPU safely.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"

/**
 * @var g_ra8_secure_fallback_count
 * @brief Diagnostic counter incremented if Secure fallback main is reached.
 * @since 0.1.0
 */
volatile uint32_t g_ra8_secure_fallback_count = 0U;

/**
 * @brief Secure fallback main entry point.
 * @since 0.1.0
 */
void main(void)
{
  g_ra8_secure_fallback_count = 1U;

  for (;;) {
    __asm__ volatile("nop");
  }
}
