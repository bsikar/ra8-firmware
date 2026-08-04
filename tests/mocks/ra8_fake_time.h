/**
 * @file ra8_fake_time.h
 * @brief Host-test deterministic time source
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Unit tests that depend on ``ra8_time_ms`` advancing need a way
 * to move time forward without spinning. ``ra8_fake_time`` wraps
 * ``ra8_time_on_tick`` in two test-friendly helpers:
 *
 *  - ``ra8_fake_time_reset()`` -- call ``ra8_time_init`` with a known
 *    clock and reset the tick counter to zero.
 *
 *  - ``ra8_fake_time_advance_ms(n)`` -- call ``ra8_time_on_tick`` n
 *    times so ``ra8_time_ms`` returns the previous value + n.
 *
 * This keeps tests deterministic regardless of host CPU speed
 * or scheduler behaviour. On the target, SysTick drives the
 * counter; on the host, the tests drive it directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Zero the tick counter and re-initialise the time source.
 *
 * @details
 * Calls ``ra8_time_init(1000000U)`` (an arbitrary non-zero CPU
 * frequency that passes the reload check) and resets the tick
 * counter to zero.
 *
 * @pre Test is running under ``RA8_OFF_TARGET``.
 * @post ``ra8_time_ms()`` returns 0.
 * @post ``ra8_time_on_tick`` works as normal afterwards.
 *
 * @note Thread safety: test code only, single-threaded.
 * @since 0.1.0
 */
void ra8_fake_time_reset(void);

/**
 * @brief Advance the tick counter by ``n`` milliseconds.
 *
 * @details
 * Calls ``ra8_time_on_tick`` ``n`` times. After this call
 * ``ra8_time_ms()`` returns the previous value plus ``n``.
 *
 * @param[in] n Number of milliseconds to advance.
 *
 * @pre Test is running under ``RA8_OFF_TARGET``.
 * @post ``ra8_time_ms`` is increased by ``n`` relative to before.
 *
 * @note Thread safety: test code only, single-threaded.
 * @since 0.1.0
 */
void ra8_fake_time_advance_ms(uint32_t n);

#ifdef __cplusplus
}
#endif
