/**
 * @file port/threadx/cortex_m85/tx_systick_ready.c
 * @brief Storage for the ThreadX-ready latch that gates SysTick ->
 *        _tx_timer_interrupt.
 *
 * @details
 * The shared SysTick handler in libs/ra_core/src/ra_time.c takes a
 * weak external reference to ``_tx_timer_interrupt`` (issue #8 port-
 * level fix) so any app that links ThreadX gets a ticking kernel
 * timer for free. That works fine when ``tx_kernel_enter`` is called
 * back-to-back with ``ra_time_init``: the SysTick window between
 * them is short enough that no tick fires.
 *
 * But apps that do a long synchronous job (PHY MDIO bring-up,
 * NetX Duo IP-thread create, USB enumeration setup) between
 * ``ra_time_init`` and ``tx_kernel_enter`` give SysTick plenty of
 * time to fire. If the handler calls ``_tx_timer_interrupt`` at
 * that point the kernel timer subsystem is still in
 * TX_INITIALIZE_IN_PROGRESS and ``_tx_timer_expiration_process``
 * dereferences a NULL ``_tx_timer_current_ptr`` or returns through
 * a stack frame the ISR prologue did not author -- the CPU then
 * exception-returns into a value that fails its EXC_RETURN integrity
 * check and HardFaults with ``UFSR.INVPC == 1``.
 *
 * Reproduced on bench 2026-05-23 with ``threadx_netx_tcp_echo``
 * (CFSR=0x400, HFSR=0x40000000, stacked PC inside
 * ``__tx_timer_dont_activate``).
 *
 * The fix: this file owns a single ``volatile uint32_t`` that
 * starts at 0 (zero-initialised .bss) and is set to 1 by the
 * project's ``_tx_initialize_low_level`` (the .S file in this
 * directory) as the very last instruction before it returns to
 * ``_tx_initialize_kernel_enter``. By that point ThreadX has
 * already armed its own SysTick and the timer subsystem is safe
 * to call into.
 *
 * The shared SysTick handler in ra_time.c declares
 * ``g_ra_threadx_systick_ready`` as a WEAK EXTERN and skips
 * ``_tx_timer_interrupt`` while the flag reads 0. Non-ThreadX
 * apps never link this TU, so the weak extern resolves to NULL
 * and the corresponding ``if`` short-circuits at runtime to false.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/**
 * @var g_ra_threadx_systick_ready
 * @brief 0 until ``_tx_initialize_low_level`` has armed ThreadX's
 *        SysTick + timer state; 1 afterwards. Read by the shared
 *        SysTick handler in libs/ra_core/src/ra_time.c.
 * @note Single-writer (assembly, once); multi-reader (every SysTick).
 * @since 0.1.0
 */
volatile uint32_t g_ra_threadx_systick_ready = 0U;
