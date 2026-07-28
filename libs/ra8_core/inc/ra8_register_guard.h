/**
 * @file ra8_register_guard.h
 * @brief IRQ-masked read-modify-write helper for shared registers
 * @ingroup grp_core
 *
 * @details
 * Most RA8D2 peripheral registers are safe to read-modify-write from
 * a single thread, but cross-peripheral routing (ELC events, ICU
 * IELSR entries, MSTP module stop) is updated from several drivers
 * and needs atomicity. This module provides a tiny primitive:
 *
 * @code{.c}
 *   ra8_register_guard_t guard;
 *   ra8_register_guard_enter(&guard);
 *   *reg = (*reg & ~mask) | new_value;
 *   ra8_register_guard_exit(&guard);
 * @endcode
 *
 * `_enter` stores the current PRIMASK and then sets it (masking all
 * maskable interrupts). `_exit` restores the saved PRIMASK. Pairs
 * must always balance; nested calls are supported.
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
 * @struct ra8_register_guard_t
 * @brief Opaque save-restore handle for IRQ masking.
 */
typedef struct {
  uint32_t saved_primask; /**< PRIMASK value captured at enter. */
} ra8_register_guard_t;

/**
 * @brief Enter a critical section: save PRIMASK, mask interrupts.
 *
 * @param[out] guard Pointer to a caller-allocated guard. Must not be
 *                   `nullptr`.
 *
 * @note Nested calls are supported. Each enter must have a matching
 *       exit.
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @since 0.1.0
 */
static inline void ra8_register_guard_enter(ra8_register_guard_t* guard)
{
#ifdef RA8_OFF_TARGET
  guard->saved_primask = 0U;
#else
  uint32_t primask;
  __asm__ volatile("mrs %0, primask" : "=r"(primask));
  __asm__ volatile("cpsid i" ::: "memory");
  guard->saved_primask = primask;
#endif
}

/**
 * @brief Exit a critical section: restore PRIMASK.
 *
 * @param[in] guard Pointer to a guard previously passed to
 *                  `ra8_register_guard_enter()`.
 *
 * @details See implementation for details.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void ra8_register_guard_exit(const ra8_register_guard_t* guard)
{
#ifdef RA8_OFF_TARGET
  (void)guard;
#else
  __asm__ volatile("msr primask, %0" ::"r"(guard->saved_primask) : "memory");
#endif
}

#ifdef __cplusplus
}
#endif
