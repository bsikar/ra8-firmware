/**
 * @file ra8_hw_err.h
 * @brief Bounded wait-flag primitives for RA8D2 HAL drivers
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (header-only)
 *
 * @details
 * Ring 3 / HAL substrate. Header-only because every helper here is a
 * single short loop that the compiler should inline at the call
 * site -- making them ``static inline`` keeps the symbol weight
 * to zero in the cross-build.
 *
 * Driver code on the RA8D2 is full of "spin until this status
 * bit changes" sequences:
 *
 * - SCI: poll TDRE before each TDR write
 * - IIC: poll BBSY before issuing START
 * - GPT: poll TCR.SE before reading the count
 * - CGC: poll OSCSF.* for oscillator stabilisation
 * - RTC: poll RWAIT for register-update lockout
 *
 * Every one of those loops needs:
 *
 * 1. A statically provable upper bound (NASA Power of 10 Rule 2).
 * 2. An ``ra8_err_t`` return so callers can propagate the
 * timeout via ``RA8_RETURN_ON_ERROR``.
 * 3. Optional cycle-friendly behaviour on the target (a wfe
 * hint, or just a memory barrier so the compiler does not
 * hoist the load out of the loop).
 *
 * The four helpers below cover every flavour the existing 29
 * driver shells need:
 *
 * - ``ra8_hw_wait_flag_set8`` -- 8-bit register, wait until
 * a mask reads as set.
 * - ``ra8_hw_wait_flag_clear8`` -- 8-bit register, wait until
 * a mask reads as clear.
 * - ``ra8_hw_wait_flag_set32`` -- 32-bit register, wait set.
 * - ``ra8_hw_wait_flag_clear32`` -- 32-bit register, wait clear.
 *
 * Each takes a pointer to a ``volatile`` register, a mask, and a
 * spin budget. The spin budget is a ``uint32_t`` so callers can
 * pick from a typed enum without needing yet another wrapper.
 *
 * On success the helper returns ``k_ra8_ok``. On budget exhaustion
 * it returns ``k_ra8_err_hw_timeout``. There is no other failure
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

#include "ra8_err.h"

#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
/* Host unit-test MMIO fault seam (tests/mocks/ra8_fake_mmio.c). Consulted once per
 * poll below so a test can drive a bounded wait to timeout or succeed-after-N,
 * exercising the real poll/timeout legs on host with the driver short-circuits
 * deleted (T1-01). Forward-declared here -- rather than including the test-only
 * header -- so this production header stays test-clean; the definition links
 * only into the host test binary. Absent in firmware and ra8_emulator builds, which
 * do not define UNIT_TEST and take the plain register-read path below. */
[[nodiscard]] bool ra8_fake_mmio_wait_eval(const volatile void* reg, uint32_t iter, bool real_cond);
/* Real-condition-honoring poll consult for the raw-loop status polls in the
 * i2c / i3c / sdhi drivers -- those were NOT written against ra8_hw_wait_* and
 * must keep their "flag_set -> exit, else spin to the bounded timeout" contract.
 * Unlike ra8_fake_mmio_wait_eval (unarmed -> true, the T1-01 short-circuit drop-in),
 * this returns @p flag_set when the register is unarmed, so an unprimed flag
 * still times out exactly as the pre-seam raw loop did and every existing test
 * keeps its behaviour. It also invokes any registered synchronous poll-hook first
 * (see ra8_fake_mmio_set_poll_hook) so a host test can model the peripheral on the
 * driver's OWN poll thread, deterministically and thread-free. */
[[nodiscard]] bool ra8_fake_mmio_poll(const volatile void* reg, uint32_t iter, bool flag_set);
/**
 * @brief Host-seam model of a "set condition: reading this register" bit.
 *
 * @details
 * Register-behaviour model (#238) for read side effects dumb host RAM
 * cannot express. The driver routes the SAME 32-bit read it performs on
 * silicon through this; the seam then latches ``set_mask`` into the
 * RAM-backed register (e.g. IPCSEMn.LOCK, HUM Ch 3.2.3) so composed host
 * flows (take -> release -> take) observe silicon-faithful state. Runs any
 * registered synchronous poll hook first (``ra8_fake_mmio_set_poll_hook``)
 * so a test can model a peer core mutating the register right before the
 * driver's read, deterministically on the driver's own thread.
 *
 * @param[in,out] reg      RAM-backed register to read; latches ``set_mask``
 *                         after the read. NULL returns 0 without touching
 *                         anything (drivers null-check first).
 * @param[in]     set_mask Bits the hardware sets as the read side effect.
 *
 * @return The 32-bit value read *before* the latch -- exactly what the
 *         silicon read data report.
 * @retval 0 ``reg`` was NULL, or the register held 0 before the latch.
 *
 * @pre Host test binary (``RA8_OFF_TARGET`` and ``UNIT_TEST``).
 * @pre ``reg`` points into a ``ra8_fake_mmap`` backed window when non-NULL.
 * @post ``*reg`` has ``set_mask`` latched on top of the pre-read value.
 * @post Any registered poll hook ran once before the read.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_fake_mmio_read_to_set32(volatile uint32_t* reg, uint32_t set_mask);

/**
 * @brief Host-seam model of a write-1-to-clear register write.
 *
 * @details
 * Register-behaviour model (#238) for W1C semantics dumb host RAM cannot
 * express. The driver routes the SAME 32-bit write command it issues on
 * silicon through this; bits covered by ``w1c_mask`` clear where ``value``
 * wrote 1 and hold otherwise, while bits outside the mask store ``value``
 * as plain data. Dumb RAM would have stored the literal 1 and left the
 * register file claiming the opposite of what silicon holds (e.g. a
 * "released" IPCSEMn.LOCK reading back as locked, HUM Ch 3.2.3).
 *
 * @param[in,out] reg      RAM-backed register receiving the command. NULL
 *                         is ignored (drivers null-check first).
 * @param[in]     w1c_mask Bits with write-1-to-clear behaviour.
 * @param[in]     value    The command word the driver writes on silicon.
 *
 * @pre Host test binary (``RA8_OFF_TARGET`` and ``UNIT_TEST``).
 * @pre ``reg`` points into a ``ra8_fake_mmap`` backed window when non-NULL.
 * @post W1C bits written 1 read back 0; W1C bits written 0 are unchanged.
 * @post Non-W1C bits hold the written ``value`` bits.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
void ra8_fake_mmio_write1_clear32(volatile uint32_t* reg, uint32_t w1c_mask, uint32_t value);
#endif

/* =============================================================================
 * Default budgets
 * =============================================================================
 */

/**
 * @enum ra8_hw_err_budget_t
 * @brief Default polling budgets for ``ra8_hw_wait_flag_*``.
 *
 * @details
 * Drivers SHOULD use one of these named budgets rather than
 * inventing their own integer literal. Each value is set so that
 * a worst-case Cortex-M85 at 1 GHz spends a comfortable margin
 * over the longest documented stabilisation time for the relevant
 * peripheral, multiplied by ~4 for safety.
 *
 * - ``k_ra8_hw_budget_short`` ~1 us at 1 GHz -- bus handshakes
 * - ``k_ra8_hw_budget_medium`` ~10 us -- TX FIFO drain
 * - ``k_ra8_hw_budget_long`` ~100 us -- oscillator wait
 * - ``k_ra8_hw_budget_xlong`` ~1 ms -- PLL relock
 */
typedef enum : uint32_t {
  k_ra8_hw_budget_short  = 0x00000400U, /**< RA8 hw budget short.  */
  k_ra8_hw_budget_medium = 0x00004000U, /**< RA8 hw budget medium. */
  k_ra8_hw_budget_long   = 0x00040000U, /**< RA8 hw budget long.   */
  k_ra8_hw_budget_xlong  = 0x00400000U, /**< RA8 hw budget xlong.  */
} ra8_hw_err_budget_t;

/* =============================================================================
 * 8-bit waiters
 * =============================================================================
 */

/**
 * @brief Spin until ``(*reg & mask) != 0`` or budget runs out.
 *
 * @param[in] reg Pointer to the volatile 8-bit register to poll.
 * Must not be NULL.
 * @param[in] mask Bit mask within ``*reg`` to test for "set".
 * Must be non-zero or the function returns
 * immediately with success.
 * @param[in] budget Maximum loop iterations before declaring
 * a timeout. Pass one of the
 * ``k_ra8_hw_budget_*`` values.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The flag was observed as set
 * within the budget.
 * @retval k_ra8_err_hw_timeout The budget ran out without the
 * flag being observed.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 *
 * @post No hardware state is modified.
 * @post On success, the most recent read returned ``mask`` set.
 *
 * @note Inline. Thread safety: not thread-safe (the caller must
 * hold whatever lock the register requires).
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: ``budget`` is a fixed upper bound, supplied by the caller.
 * - Rule 5: 2 preconditions, 2 postconditions.
 *
 * @details See implementation.
 */
static inline ra8_err_t
ra8_hw_wait_flag_set8(volatile const uint8_t* reg, uint8_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(reg, i, ((*reg & mask) != 0U))) {
      return k_ra8_ok;
    }
#else
    if ((*reg & mask) != 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Spin until ``(*reg & mask) == 0`` or budget runs out.
 *
 * @param[in] reg Pointer to the volatile 8-bit register to poll.
 * @param[in] mask Bit mask to test for "clear".
 * @param[in] budget Spin budget. See ``ra8_hw_err_budget_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The flag was observed as clear.
 * @retval k_ra8_err_hw_timeout Budget exhausted.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 *
 * @post No hardware state is modified.
 * @post On success, the most recent read returned ``mask`` clear.
 *
 * @note Inline. Not thread-safe.
 * @since 0.1.0
 *
 * @details See implementation.
 */
static inline ra8_err_t
ra8_hw_wait_flag_clear8(volatile const uint8_t* reg, uint8_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(reg, i, ((*reg & mask) == 0U))) {
      return k_ra8_ok;
    }
#else
    if ((*reg & mask) == 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/* =============================================================================
 * 32-bit waiters
 * =============================================================================
 */

/**
 * @brief Spin until ``(*reg & mask) != 0`` or budget runs out.
 *
 * @param[in] reg Pointer to the volatile 32-bit register to poll.
 * @param[in] mask Bit mask to test for "set".
 * @param[in] budget Spin budget.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The flag was observed as set within the budget.
 * @retval k_ra8_err_hw_timeout The budget ran out without the flag observed.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 * @post No hardware state is modified.
 * @post On success, the latest read returned the mask set.
 *
 * @note Inline. Not thread-safe.
 * @since 0.1.0
 *
 * @details See implementation.
 */
static inline ra8_err_t
ra8_hw_wait_flag_set32(volatile const uint32_t* reg, uint32_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(reg, i, ((*reg & mask) != 0U))) {
      return k_ra8_ok;
    }
#else
    if ((*reg & mask) != 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Spin until ``(*reg & mask) == 0`` or budget runs out.
 *
 * @param[in] reg Pointer to the volatile 32-bit register to poll.
 * @param[in] mask Bit mask to test for "clear".
 * @param[in] budget Spin budget.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The flag was observed as clear within the budget.
 * @retval k_ra8_err_hw_timeout The budget ran out without the flag observed.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 *
 * @pre ``reg`` is non-NULL.
 * @pre ``budget`` > 0.
 * @post No hardware state is modified.
 * @post On success, the latest read returned the mask clear.
 *
 * @note Inline. Not thread-safe.
 * @since 0.1.0
 *
 * @details See implementation.
 */
static inline ra8_err_t
ra8_hw_wait_flag_clear32(volatile const uint32_t* reg, uint32_t mask, uint32_t budget)
{
  if (reg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < budget; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(reg, i, ((*reg & mask) == 0U))) {
      return k_ra8_ok;
    }
#else
    if ((*reg & mask) == 0U) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

#ifdef __cplusplus
}
#endif
