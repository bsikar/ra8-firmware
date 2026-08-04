/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file timer.c
 * @brief Thin SysTick-based timer wrapper
 *
 * @details
 * Re-exports `ra8_time_ms()` / `ra8_delay_ms()` from `ra8_time.h` under
 * a shorter name so driver code can use `ra8_now_ms()` and
 * `ra8_sleep_ms()` without importing the longer header chain. Pure
 * wrapper, zero additional state.
 */

#include <stdint.h>

#include "ra8_time.h"

uint32_t ra8_now_ms(void)
{
  return ra8_time_ms();
}

void ra8_sleep_ms(uint32_t ms)
{
  ra8_delay_ms(ms);
}
