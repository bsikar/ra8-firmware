/**
 * @file timer.c
 * @brief Thin SysTick-based timer wrapper
 *
 * @details
 * Re-exports `ra_time_ms()` / `ra_delay_ms()` from `ra_time.h` under
 * a shorter name so driver code can use `ra_now_ms()` and
 * `ra_sleep_ms()` without importing the longer header chain. Pure
 * wrapper, zero additional state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_time.h"

/* Implementation of ra_now_ms (see header for full contract) -- see header for the documented contract. */
uint32_t ra_now_ms(void)
{
  return ra_time_ms();
}

/* Implementation of ra_sleep_ms (see header for full contract) -- see header for the documented contract. */
void ra_sleep_ms(uint32_t ms)
{
  ra_delay_ms(ms);
}
