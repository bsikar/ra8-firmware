/**
 * @file port/threadx/inc/tx_user.h
 * @brief Eclipse ThreadX user-tunable defines for ra8-firmware
 *
 * @details
 * Picked up by both the ThreadX library build and the application TUs
 * because the project compiles ThreadX with `TX_INCLUDE_USER_DEFINE_FILE`
 * and adds `port/threadx/` to the include search path.
 *
 * Tunables intended for the RA8 family:
 *
 * - `TX_TIMER_TICKS_PER_SECOND = 1000` -- 1 ms tick. SysTick reload is
 *   programmed in `tx_initialize_low_level.S` to match.
 * - `TX_MINIMUM_STACK = 512` -- doubled from upstream (200) to give
 *   floating-point + Helium register save plenty of room.
 * - `TX_MAX_PRIORITIES = 32` -- single 32-bit priority bitmap word, the
 *   minimum legal value and the fastest path through the scheduler.
 * - `TX_TIMER_PROCESS_IN_ISR` -- run timer expiration directly from
 *   the SysTick ISR. Saves the timer-thread context switch.
 * - `TX_REACTIVATE_INLINE` -- inline timer reactivation; small speedup
 *   for periodic timers that only matters once we have a real workload.
 * - `TX_DISABLE_PREEMPTION_THRESHOLD` -- the demo does not use it; saves
 *   code size.
 * - `TX_DISABLE_NOTIFY_CALLBACKS` -- ditto.
 *
 * Single-mode-secure: this firmware runs Secure-only for now (Cortex-M85
 * TrustZone is configured but no Non-Secure partition is loaded), so we
 * tell ThreadX it can skip the secure-stack management code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/* Tick + scheduler tuning. */
/** @brief TX TIMER TICKS PER SECOND. */
#define TX_TIMER_TICKS_PER_SECOND (1000)
/** @brief TX MAX PRIORITIES. */
#define TX_MAX_PRIORITIES (32)
/** @brief TX MINIMUM STACK. */
#define TX_MINIMUM_STACK (512)

/* Performance / size knobs (see ThreadX user-guide chapter 4). */
/** @brief TX TIMER PROCESS IN ISR. */
#define TX_TIMER_PROCESS_IN_ISR
/** @brief TX REACTIVATE INLINE. */
#define TX_REACTIVATE_INLINE
/** @brief TX DISABLE PREEMPTION THRESHOLD. */
#define TX_DISABLE_PREEMPTION_THRESHOLD
/** @brief TX DISABLE REDUNDANT CLEARING. */
#define TX_DISABLE_REDUNDANT_CLEARING
/** @brief TX DISABLE NOTIFY CALLBACKS. */
#define TX_DISABLE_NOTIFY_CALLBACKS

/* Single-mode selection.
 *
 * Most apps run ThreadX in the Secure world only (no Non-Secure partition
 * is loaded), so ThreadX compiles out the secure-stack-context machinery
 * (TX_SINGLE_MODE_SECURE). The `threadx_ns` library variant -- used by
 * tz_nsc_cgc_usb to run ThreadX INSIDE the Non-Secure image (#96) --
 * defines RA8_THREADX_NON_SECURE so ThreadX builds for the Non-Secure side
 * instead. Selecting exactly one single-mode keeps the scheduler off the
 * dual-world secure-stack path. */
#if defined(RA8_THREADX_NON_SECURE)
/** @brief TX SINGLE MODE NON SECURE. */
#define TX_SINGLE_MODE_NON_SECURE
#else
/** @brief TX SINGLE MODE SECURE. */
#define TX_SINGLE_MODE_SECURE
#endif
