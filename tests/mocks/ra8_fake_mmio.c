/**
 * @file ra8_fake_mmio.c
 * @brief Host-side programmable MMIO fault seam
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (host test-only)
 *
 * @details Provides bounded programmable register-wait outcomes so negative
 * HAL paths can be exercised deterministically without dereferencing hardware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fake_mmio.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_hw_err.h" /* prototype for the guarded ra8_fake_mmio_wait_eval hook. */

/**
 * @enum ra8_fake_mmio_mode_t
 * @brief How an armed register overrides a bounded wait.
 */
typedef enum : uint8_t {
  k_ra8_fake_mmio_mode_none          = 0U, /**< Free slot / transparent.    */
  k_ra8_fake_mmio_mode_fail          = 1U, /**< Never satisfied -> timeout. */
  k_ra8_fake_mmio_mode_satisfy_after = 2U, /**< Satisfied once iter >= arg. */
  k_ra8_fake_mmio_mode_fail_nth      = 3U, /**< The arg-th (0-based) wait-loop on
                                          *   this register fails; all other
                                          *   wait-loops on it succeed. Isolates
                                          *   the timeout leg of a driver that
                                          *   polls one register in several
                                          *   sequential stages (e.g. GWCA
                                          *   set_operation_mode called N times
                                          *   during bring-up).                */
} ra8_fake_mmio_mode_t;

/**
 * @struct ra8_fake_mmio_fault_t
 * @brief One armed register entry. A zero @c addr marks a free slot -- an MMIO
 *        register address is never 0 (arming rejects a NULL reg), so 0 is a
 *        safe free-slot sentinel and keeps the lookup a single-condition test.
 */
typedef struct {
  uintptr_t            addr;      /**< Polled register address; 0 == free slot.   */
  uint32_t             arg;       /**< satisfy-after poll index / fail-nth index. */
  uint32_t             wait_seen; /**< fail-nth: count of wait-loops started.     */
  ra8_fake_mmio_mode_t mode;      /**< Override mode.                             */
} ra8_fake_mmio_fault_t;

static ra8_fake_mmio_fault_t s_faults[k_ra8_fake_mmio_max_faults];

/**
 * @brief Optional synchronous per-poll hook, invoked from ::ra8_fake_mmio_poll.
 *
 * @details
 * When set, this runs once at the top of every ::ra8_fake_mmio_poll on the
 * DRIVER's own polling thread, letting a test model the peripheral (stuff
 * response registers, latch a NACK flag, re-assert RSPEND) exactly when the
 * driver polls -- deterministically, with no concurrent servicer thread and no
 * wall-clock timer to race the poll. Cleared by ::ra8_fake_mmio_reset.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 */
static void (*s_poll_hook)(void);

/**
 * @brief Find the armed fault row for one MMIO register address.
 * @details Performs a bounded linear scan while reserving address zero as the
 * table's free-slot and null-register sentinel.
 * @param[in] addr Integer form of the polled register address.
 * @return Matching mutable row, or null when the register is not armed.
 * @retval non-NULL The unique row whose address equals @p addr.
 * @retval NULL @p addr is zero or no row matches it.
 * @pre The fault table was initialized statically or by reset.
 * @pre No concurrent caller mutates the table during the scan.
 * @post The table contents are unchanged.
 * @post Any returned pointer refers inside ::s_faults.
 * @note The fixed table bound satisfies NASA Power-of-10 Rule 2.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fake_mmio_fault_t* internal_find(uintptr_t addr)
{
  if (addr == 0U) {
    return nullptr; /* 0 is the free-slot sentinel and the NULL-reg address. */
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_mmio_max_faults; ++i) {
    if (s_faults[i].addr == addr) {
      return &s_faults[i];
    }
  }
  return nullptr;
}

/**
 * @brief Create or replace the scripted wait behavior for one register.
 * @details Reuses an existing address row when present, otherwise claims the
 * first free bounded-table slot and resets its per-wait observation counter.
 * @param[in] reg Address of the register whose polls will be overridden.
 * @param[in] mode Override behavior to install.
 * @param[in] arg Mode-specific poll or wait-loop index.
 * @return Arming status.
 * @retval k_ra8_ok The row was created or replaced.
 * @retval k_ra8_err_null_ptr @p reg is null.
 * @retval k_ra8_err_no_mem No free row remains.
 * @pre @p mode is a non-`none` ::ra8_fake_mmio_mode_t value.
 * @pre No other thread accesses the fake table concurrently.
 * @post Success leaves one unique row for @p reg with `wait_seen` zero.
 * @post Failure leaves every row unchanged.
 * @note Re-arming a register replaces its prior mode and argument.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_arm(const volatile void* reg, ra8_fake_mmio_mode_t mode, uint32_t arg)
{
  if (reg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const uintptr_t        addr = (uintptr_t)reg;
  ra8_fake_mmio_fault_t* slot = internal_find(addr);
  if (slot == nullptr) {
    for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_mmio_max_faults; ++i) {
      if (s_faults[i].addr == 0U) {
        slot = &s_faults[i];
        break;
      }
    }
  }
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
  }
  slot->addr      = addr;
  slot->mode      = mode;
  slot->arg       = arg;
  slot->wait_seen = 0U;
  return k_ra8_ok;
}

void ra8_fake_mmio_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_fake_mmio_max_faults; ++i) {
    s_faults[i].addr      = 0U;
    s_faults[i].arg       = 0U;
    s_faults[i].wait_seen = 0U;
    s_faults[i].mode      = k_ra8_fake_mmio_mode_none;
  }
  s_poll_hook = nullptr;
}

ra8_err_t ra8_fake_mmio_fail_wait(const volatile void* reg)
{
  return internal_arm(reg, k_ra8_fake_mmio_mode_fail, 0U);
}

ra8_err_t ra8_fake_mmio_satisfy_after(const volatile void* reg, uint32_t n)
{
  return internal_arm(reg, k_ra8_fake_mmio_mode_satisfy_after, n);
}

ra8_err_t ra8_fake_mmio_fail_nth_wait(const volatile void* reg, uint32_t n)
{
  return internal_arm(reg, k_ra8_fake_mmio_mode_fail_nth, n);
}

/**
 * @brief Evaluate one armed fault row for the current poll iteration.
 * @details Implements success-after-N, fail-one-wait, and unconditional-timeout
 * behavior while advancing the wait counter only on iteration zero.
 * @param[in,out] slot Armed row selected for the register.
 * @param[in] iter Zero-based iteration within the current bounded wait.
 * @return Whether the fake reports the wait condition satisfied.
 * @retval true The selected mode allows this wait to complete now.
 * @retval false The caller must continue polling until its own bound.
 * @pre @p slot is non-null and names an armed ::s_faults row.
 * @pre Each wait loop supplies @p iter beginning at zero.
 * @post Fail-nth mode counts a new wait exactly once at iteration zero.
 * @post Other mode state remains unchanged.
 * @note The caller, not this helper, owns the timeout budget.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_armed_eval(ra8_fake_mmio_fault_t* slot, uint32_t iter)
{
  if (slot->mode == k_ra8_fake_mmio_mode_satisfy_after) {
    return (iter >= slot->arg);
  }
  if (slot->mode == k_ra8_fake_mmio_mode_fail_nth) {
    /* A new wait-loop starts on its first poll (iter == 0); the arg-th such loop
     * (0-based) fails to converge, every other loop succeeds on its first poll.
     * This isolates the timeout leg of a driver stage that re-polls the same
     * register across several sequential calls. */
    if (iter == 0U) {
      slot->wait_seen++;
    }
    return ((slot->wait_seen - 1U) != slot->arg);
  }
  return false; /* k_ra8_fake_mmio_mode_fail: never satisfied -> caller times out. */
}

void ra8_fake_mmio_set_poll_hook(void (*hook)(void))
{
  s_poll_hook = hook;
}

bool ra8_fake_mmio_wait_eval(const volatile void* reg, uint32_t iter, bool real_cond)
{
  ra8_fake_mmio_fault_t* slot = internal_find((uintptr_t)reg);
  if (slot == nullptr) {
    /* Unarmed: model a peripheral whose flag is already at its wait condition,
     * so the poll succeeds on its first read. This makes deleting a driver's
     * ``#ifdef RA8_OFF_TARGET return k_ra8_ok`` short-circuit a DROP-IN: every
     * consumer that does not care about this particular wait -- app smoke tests,
     * integration/_cov tests, any code path that just needs the driver to make
     * progress -- passes exactly as it did under the short-circuit, without
     * having to pre-stage the register. A test that wants the TIMEOUT leg arms
     * ::ra8_fake_mmio_fail_wait; one that wants the succeed-after-N / continuation
     * leg arms ::ra8_fake_mmio_satisfy_after. The driver's real read still happened
     * (side-effect free); its value is intentionally ignored for the loop-exit
     * decision here so an unstaged flag never spins a consumer to timeout. */
    (void)real_cond;
    return true;
  }
  return internal_armed_eval(slot, iter);
}

bool ra8_fake_mmio_poll(const volatile void* reg, uint32_t iter, bool flag_set)
{
  /* Run the synchronous per-poll hook first, on the DRIVER's own polling thread,
   * so a test can model the peripheral (stuff responses, latch NACK, re-assert
   * RSPEND) exactly when the driver polls -- deterministically, with no
   * concurrent servicer thread and no wall-clock timer to race. */
  if (s_poll_hook != nullptr) {
    s_poll_hook();
  }
  ra8_fake_mmio_fault_t* slot = internal_find((uintptr_t)reg);
  if (slot == nullptr) {
    /* Unarmed: honor the driver's real flag read (raw-loop parity) -- an unprimed
     * flag still spins the caller to its timeout, exactly as the pre-seam loop
     * did. Contrast ::ra8_fake_mmio_wait_eval, whose unarmed leg returns true as a
     * drop-in for the deleted RA8_OFF_TARGET short-circuits. */
    return flag_set;
  }
  return internal_armed_eval(slot, iter);
}

/* =============================================================================
 * Register-behaviour models (#238): read-to-set and write-1-to-clear.
 * These replace per-driver RA8_OFF_TARGET peripheral models: the driver
 * performs its real register touch through them, and the seam applies the
 * silicon side effect that dumb host RAM cannot (a read that latches a bit,
 * a W1C command that clears instead of storing the 1). Declared for the HAL
 * in ra8_hw_err.h, same as ra8_fake_mmio_wait_eval / ra8_fake_mmio_poll.
 * =============================================================================
 */

uint32_t ra8_fake_mmio_read_to_set32(volatile uint32_t* reg, uint32_t set_mask)
{
  if (reg == nullptr) {
    return 0U; /* Defensive: drivers null-check before touching a register. */
  }
  /* Same contract as ra8_fake_mmio_poll: run the synchronous hook first, on the
   * DRIVER's own thread, so a test can model the peer -- e.g. another core
   * releasing a hardware semaphore -- exactly before the driver's read. */
  if (s_poll_hook != nullptr) {
    s_poll_hook();
  }
  const uint32_t prev = *reg;
  *reg                = prev | set_mask;
  return prev;
}

void ra8_fake_mmio_write1_clear32(volatile uint32_t* reg, uint32_t w1c_mask, uint32_t value)
{
  if (reg == nullptr) {
    return; /* Defensive: drivers null-check before touching a register. */
  }
  const uint32_t prev = *reg;
  /* Bits covered by w1c_mask clear where the command wrote 1 and are
   * otherwise unchanged; bits outside the mask behave as plain storage. */
  *reg = (prev & ~(value & w1c_mask)) | (value & ~w1c_mask);
}
