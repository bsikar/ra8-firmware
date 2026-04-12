/**
 * @file ra_hw_err.h
 * @brief Bounded wait-flag primitives for RA8D2 HAL drivers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (header-only)
 *
 * @details
 * Wave 1 substrate. Header-only because every helper here is a
 * single short loop that the compiler should inline at the call
 * site -- making them ``static inline`` keeps the symbol weight
 * to zero in the cross-build.
 *
 * Driver code on the RA8D2 is full of "spin until this status
 * bit changes" sequences:
 *
 *   - SCI: poll TDRE before each TDR write
 *   - IIC: poll BBSY before issuing START
 *   - GPT: poll TCR.SE before reading the count
 *   - CGC: poll OSCSF.* for oscillator stabilisation
 *   - RTC: poll RWAIT for register-update lockout
 *
 * Every one of those loops needs:
 *
 *  1. A statically provable upper bound (NASA Power of 10 Rule 2).
 *  2. An ``ra_err_t`` return so callers can propagate the
 *     timeout via ``RA_RETURN_ON_ERROR``.
 *  3. Optional cycle-friendly behaviour on the target (a wfe
 *     hint, or just a memory barrier so the compiler does not
 *     hoist the load out of the loop).
 *
 * The four helpers below cover every flavour the existing 29
 * driver shells need:
 *
 *  - ``ra_hw_wait_flag_set8``    -- 8-bit register, wait until
 *                                    a mask reads as set.
 *  - ``ra_hw_wait_flag_clear8``  -- 8-bit register, wait until
 *                                    a mask reads as clear.
 *  - ``ra_hw_wait_flag_set32``   -- 32-bit register, wait set.
 *  - ``ra_hw_wait_flag_clear32`` -- 32-bit register, wait clear.
 *
 * Each takes a pointer to a ``volatile`` register, a mask, and a
 * spin budget. The spin budget is a ``uint32_t`` so callers can
 * pick from a typed enum without needing yet another wrapper.
 *
 * On success the helper returns ``k_ra_ok``. On budget exhaustion
 * it returns ``k_ra_err_hw_timeout``. There is no other failure
 * mode.
 *
 * The implementation does NOT issue ``__WFE``, ``__DSB`` or any
 * other Cortex-M intrinsic -- those are bring-up-time decisions
 * that belong in the calling driver. The barrier here is just
 * the ``volatile`` qualifier on the register pointer, which is
 * sufficient to keep the compiler from hoisting the load.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Default budgets
 * =============================================================================
 */

/**
 * @enum ra_hw_err_budget_t
 * @brief Default polling budgets for ``ra_hw_wait_flag_*``.
 *
 * @details
 * Drivers SHOULD use one of these named budgets rather than
 * inventing their own integer literal. Each value is set so that
 * a worst-case Cortex-M85 at 1 GHz spends a comfortable margin
 * over the longest documented stabilisation time for the relevant
 * peripheral, multiplied by ~4 for safety.
 *
 *  - ``k_ra_hw_budget_short``  ~1 us at 1 GHz -- bus handshakes
 *  - ``k_ra_hw_budget_medium`` ~10 us         -- TX FIFO drain
 *  - ``k_ra_hw_budget_long``   ~100 us        -- oscillator wait
 *  - ``k_ra_hw_budget_xlong``  ~1 ms          -- PLL relock
 */
typedef enum : uint32_t {
  k_ra_hw_budget_short  = 0x00000400U,
  k_ra_hw_budget_medium = 0x00004000U,
  k_ra_hw_budget_long   = 0x00040000U,
  k_ra_hw_budget_xlong  = 0x00400000U,
} ra_hw_err_budget_t;

/* =============================================================================
 * 8-bit waiters
 * =============================================================================
 */

/**
 * @brief Spin until ``(*reg & mask) != 0`` or budget runs out.
 *
 * @param[in] reg     Pointer to the volatile 8-bit register to poll.
 *                    Must not be NULL.
 * @param[in] mask    Bit mask within ``*reg`` to test for "set".
 *                    Must be non-zero or the function returns
 *                    immediately with success.
 * @param[in] budget  Maximum loop iterations before declaring
 *                    a timeout. Pass one of the
 *                    ``k_ra_hw_budget_*`` values.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                The flag was observed as set
 *                                 within the budget.
 * @retval k_ra_err_hw_timeout    The budget ran out without the
 *                                 flag being observed.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 *
 * @post No hardware state is modified.
 * @post On success, the most recent read returned ``mask`` set.
 *
 * @note Inline. Thread safety: not thread-safe (the caller must
 *       hold whatever lock the register requires).
 * @since 0.2.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: ``budget`` is a fixed upper bound, supplied by the caller.
 * - Rule 5: 2 preconditions, 2 postconditions.
 */
static inline ra_err_t
ra_hw_wait_flag_set8(volatile const uint8_t* reg, uint8_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
    if ((*reg & mask) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Spin until ``(*reg & mask) == 0`` or budget runs out.
 *
 * @param[in] reg    Pointer to the volatile 8-bit register to poll.
 * @param[in] mask   Bit mask to test for "clear".
 * @param[in] budget Spin budget. See ``ra_hw_err_budget_t``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                The flag was observed as clear.
 * @retval k_ra_err_hw_timeout    Budget exhausted.
 * @retval k_ra_err_null_ptr      ``reg`` was NULL.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 *
 * @post No hardware state is modified.
 * @post On success, the most recent read returned ``mask`` clear.
 *
 * @note Inline. Not thread-safe.
 * @since 0.2.0
 */
static inline ra_err_t
ra_hw_wait_flag_clear8(volatile const uint8_t* reg, uint8_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
    if ((*reg & mask) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* =============================================================================
 * 32-bit waiters
 * =============================================================================
 */

/**
 * @brief Spin until ``(*reg & mask) != 0`` or budget runs out.
 *
 * @param[in] reg    Pointer to the volatile 32-bit register to poll.
 * @param[in] mask   Bit mask to test for "set".
 * @param[in] budget Spin budget.
 *
 * @return ``k_ra_ok`` or ``k_ra_err_hw_timeout`` (or
 *         ``k_ra_err_null_ptr`` if ``reg`` is NULL).
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 * @post No hardware state is modified.
 * @post On success, the latest read returned the mask set.
 *
 * @note Inline. Not thread-safe.
 * @since 0.2.0
 */
static inline ra_err_t
ra_hw_wait_flag_set32(volatile const uint32_t* reg, uint32_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
    if ((*reg & mask) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Spin until ``(*reg & mask) == 0`` or budget runs out.
 *
 * @param[in] reg    Pointer to the volatile 32-bit register to poll.
 * @param[in] mask   Bit mask to test for "clear".
 * @param[in] budget Spin budget.
 *
 * @return ``k_ra_ok`` or ``k_ra_err_hw_timeout``.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 * @post No hardware state is modified.
 * @post On success, the latest read returned the mask clear.
 *
 * @note Inline. Not thread-safe.
 * @since 0.2.0
 */
static inline ra_err_t
ra_hw_wait_flag_clear32(volatile const uint32_t* reg, uint32_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
    if ((*reg & mask) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

#ifdef __cplusplus
}
#endif
