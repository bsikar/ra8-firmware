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

/**
 * @brief Implementation of ra_now_ms (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
uint32_t ra_now_ms(void)
{
  return ra_time_ms();
}

/**
 * @brief Implementation of ra_sleep_ms (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] ms See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_sleep_ms(uint32_t ms)
{
  ra_delay_ms(ms);
}
