/**
 * @file ra8_time_interface_systick.c
 * @brief Production `ra8_time_interface_t` instance backed by SysTick
 *
 * @details
 * `ra8_time.h` publishes `g_ra8_time_interface_systick` as the production
 * time vtable a driver injects when it wants wall-clock time without
 * hard-coding a call to ::ra8_time_ms. This translation unit owns that
 * object. It holds no state of its own: `now_ms` forwards to ::ra8_time_ms
 * and `delay_ms` forwards to ::ra8_delay_ms, so the vtable observes exactly
 * the same 1 kHz SysTick tick counter a direct caller would, and `ctx` is
 * `nullptr` because neither forwarder needs one.
 *
 * Kept out of `ra8_time.c` deliberately: that unit owns the tick counter,
 * the SysTick IRQ body and the delay policy, and is linked into images that
 * never touch dependency injection. Splitting the vtable out keeps the
 * function pointers (and the linker's inability to garbage-collect an
 * address-taken function) off those images.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_time.h"
#include "ra8_time_interface.h"

/**
 * @brief `now_ms` forwarder for ::g_ra8_time_interface_systick.
 *
 * @details Discards the context and returns ::ra8_time_ms verbatim, so a
 *          consumer reading time through the vtable sees the same tick
 *          counter as a direct caller.
 *
 * @param[in] ctx Unused; the SysTick source keeps no per-instance state.
 *
 * @return Milliseconds since `ra8_time_init()`, wrapping at 2^32.
 * @retval 0..UINT32_MAX Whatever ::ra8_time_ms reports.
 *
 * @pre None. Before `ra8_time_init()` the underlying counter simply reads 0.
 * @post No state modified.
 *
 * @note Thread-safe (single-word read on Cortex-M is atomic).
 *
 * @since 0.1.0
 */
static uint32_t internal_time_if_systick_now_ms(void* ctx)
{
  (void)ctx;
  return ra8_time_ms();
}

/**
 * @brief `delay_ms` forwarder for ::g_ra8_time_interface_systick.
 *
 * @details Discards the context and forwards to ::ra8_delay_ms, which
 *          busy-waits on the tick counter (or DWT_CYCCNT when PRIMASK is
 *          set). Zero returns immediately.
 *
 * @param[in] ctx Unused; the SysTick source keeps no per-instance state.
 * @param[in] ms  Milliseconds to wait.
 *
 * @pre Same as ::ra8_delay_ms: `ra8_time_init()` has been called.
 * @post At least `ms` milliseconds have elapsed.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static void internal_time_if_systick_delay_ms(void* ctx, uint32_t ms)
{
  (void)ctx;
  ra8_delay_ms(ms);
}

const ra8_time_interface_t g_ra8_time_interface_systick = {
  .now_ms   = internal_time_if_systick_now_ms,
  .delay_ms = internal_time_if_systick_delay_ms,
  .ctx      = nullptr,
};
