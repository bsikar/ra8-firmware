/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_mmio.h
 * @brief Host-test programmable MMIO fault seam for bounded HAL waits
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (host test-only)
 *
 * @details
 * The host unit-test build forces ``RA8_OFF_TARGET`` on every HAL TU, so a
 * driver's "spin until this status bit changes" loop reads its register out of
 * ordinary host RAM (see ``ra8_fake_mmap.c``). RAM alone can model two states --
 * a flag pre-staged as set (success on the first poll) or never set (the loop
 * runs to its budget and returns ``k_ra8_err_hw_timeout``) -- but it CANNOT model
 * the real hardware behaviours the drivers actually poll for:
 *
 * - a flag the peripheral asserts a few cycles into the wait (clear-then-wait),
 *   which a single-threaded test cannot poke mid-loop;
 * - a deterministic timeout regardless of any stray staged bit;
 * - stepping the loop through several iterations before it succeeds, which the
 *   loop's own iteration branch needs for MC/DC;
 * - timing out only one of several sequential wait-loops that a driver stage
 *   runs on the SAME register (e.g. GWCA set_operation_mode called once per
 *   bring-up sub-step), so a test can reach any single stage's failure leg.
 *
 * This seam supplies exactly those. It is consulted from the shared bounded-wait
 * primitives in ``ra8_hw_err.h`` (and any driver that opts in) under
 * ``RA8_OFF_TARGET && UNIT_TEST`` -- i.e. only in the host test binary, never
 * in firmware or ra8_emulator, which link neither this TU nor the guard. When a
 * test has not armed a fault for a register the seam models a peripheral whose
 * flag is already at its wait condition: the wait succeeds on its first poll.
 * This is the drop-in replacement for the per-driver
 * ``#ifdef RA8_OFF_TARGET return k_ra8_ok`` short-circuits (T1-01) -- with
 * those deleted, every consumer that does not care about a given wait makes
 * progress exactly as before, gcov sees the real loop run, and a test that wants
 * the succeed-after-N, timeout, or fail-one-of-N leg arms the seam explicitly.
 *
 * Usage from a test:
 * @code
 * void setUp(void) { ra8_fake_mmap_reset(); ra8_fake_mmio_reset(); }
 *
 * // Success after the peripheral "asserts" the flag on the 3rd poll:
 * ra8_fake_mmio_satisfy_after(&ra8_sci(0)->CSR, 3U);
 * TEST_ASSERT_EQUAL(k_ra8_ok, ra8_sci_flush(0));
 *
 * // Timeout: the flag never asserts:
 * ra8_fake_mmio_fail_wait(&ra8_sci(0)->CSR);
 * TEST_ASSERT_EQUAL(k_ra8_err_hw_timeout, ra8_sci_flush(0));
 * @endcode
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_fake_mmio_limits_t
 * @brief Fixed capacity of the armed-fault table.
 *
 * @details
 * Bounded so the seam allocates nothing at runtime (NASA Power of 10 Rule 3).
 * Eight simultaneously-armed registers is ample: a single wait sequence in one
 * driver call touches at most a handful of distinct status registers.
 */
typedef enum : uint8_t {
  k_ra8_fake_mmio_max_faults = 8U, /**< Max distinct registers armed at once. */
} ra8_fake_mmio_limits_t;

/**
 * @brief Disarm every register: the seam becomes fully transparent.
 *
 * @details
 * Call from a test's ``setUp()`` (alongside ``ra8_fake_mmap_reset()``) so armed
 * faults never leak between cases. After this, ::ra8_fake_mmio_wait_eval returns
 * each register's real RAM value for every register.
 *
 * @pre Called from a host (``RA8_OFF_TARGET`` + ``UNIT_TEST``) test binary.
 * @post No register is armed; the seam is transparent.
 *
 * @note Not thread-safe. Tests are single-threaded.
 * @since 0.1.0
 */
void ra8_fake_mmio_reset(void);

/**
 * @brief Arm ``reg`` so bounded waits polling it never see it satisfied.
 *
 * @details
 * Forces every ::ra8_fake_mmio_wait_eval for ``reg`` to report "not satisfied",
 * so the caller's poll loop runs to its full budget and returns
 * ``k_ra8_err_hw_timeout`` -- deterministically, independent of any value staged
 * in the register's RAM. Re-arming an already-armed register updates it.
 *
 * @param[in] reg Address of the polled register (e.g. ``&ra8_sci(0)->CSR``).
 *                Must not be NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Register armed to fail its waits.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 * @retval k_ra8_err_no_mem The fault table is full.
 *
 * @pre ``reg`` is non-NULL.
 * @pre Fewer than ::k_ra8_fake_mmio_max_faults distinct registers are armed
 *      (unless ``reg`` is already armed, which updates in place).
 * @post A subsequent wait on ``reg`` returns ``k_ra8_err_hw_timeout``.
 *
 * @note Test-only. Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_mmio_fail_wait(const volatile void* reg);

/**
 * @brief Arm ``reg`` so bounded waits polling it succeed on poll ``n``.
 *
 * @details
 * Models a peripheral that asserts its ready flag a few polls into the wait:
 * ::ra8_fake_mmio_wait_eval for ``reg`` reports "not satisfied" for iterations
 * ``0 .. n-1`` and "satisfied" from iteration ``n`` on. With ``n == 0`` the wait
 * succeeds immediately; with ``n`` below the caller's budget it succeeds after
 * ``n`` loop iterations (exercising the loop's continuation branch for MC/DC);
 * with ``n`` at or above the budget it behaves like ::ra8_fake_mmio_fail_wait.
 * Re-arming an already-armed register updates it.
 *
 * @param[in] reg Address of the polled register. Must not be NULL.
 * @param[in] n   Zero-based poll index at which the flag becomes satisfied.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Register armed.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 * @retval k_ra8_err_no_mem The fault table is full.
 *
 * @pre ``reg`` is non-NULL.
 * @pre Fewer than ::k_ra8_fake_mmio_max_faults distinct registers are armed
 *      (unless ``reg`` is already armed, which updates in place).
 * @post A subsequent wait on ``reg`` succeeds on its ``n``-th poll.
 *
 * @note Test-only. Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_mmio_satisfy_after(const volatile void* reg, uint32_t n);

/**
 * @brief Arm ``reg`` so only the ``n``-th bounded wait-loop polling it times out.
 *
 * @details
 * Isolates one timeout leg of a driver stage that re-polls the SAME register
 * across several sequential wait-loops (for example the RA8D2 GWCA
 * ``set_operation_mode``, which polls GWMS once per bring-up sub-step). A plain
 * ::ra8_fake_mmio_fail_wait fails the FIRST such loop and hides the later legs;
 * this mode fails only the ``n``-th (zero-based) wait-loop on ``reg`` and lets
 * every other loop succeed on its first poll, so a test can drive the failure
 * path of any single stage. ::ra8_fake_mmio_wait_eval counts a new wait-loop on
 * each ``iter == 0`` poll; ``n == 0`` is therefore equivalent to
 * ::ra8_fake_mmio_fail_wait. Re-arming an already-armed register updates it and
 * resets the loop counter.
 *
 * @param[in] reg Address of the polled register. Must not be NULL.
 * @param[in] n   Zero-based index of the wait-loop on ``reg`` to fail.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Register armed.
 * @retval k_ra8_err_null_ptr ``reg`` was NULL.
 * @retval k_ra8_err_no_mem The fault table is full.
 *
 * @pre ``reg`` is non-NULL.
 * @pre Fewer than ::k_ra8_fake_mmio_max_faults distinct registers are armed
 *      (unless ``reg`` is already armed, which updates in place).
 * @post The ``n``-th wait-loop on ``reg`` returns ::k_ra8_err_hw_timeout; all
 *       others succeed.
 *
 * @note Test-only. Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_mmio_fail_nth_wait(const volatile void* reg, uint32_t n);

/**
 * @brief Register a synchronous per-poll hook invoked from ::ra8_fake_mmio_poll.
 *
 * @details
 * The i2c / i3c / sdhi status polls route each bounded-wait iteration through
 * ::ra8_fake_mmio_poll. When a hook is installed it runs once at the top of every
 * such poll, on the DRIVER's own thread, so a test can model the peripheral --
 * stuff response registers, latch a NACK flag, re-assert RSPEND -- exactly when
 * the driver polls. Because the mutation happens on the driver's poll thread and
 * is driven by the driver's own progress, the outcome is deterministic on any
 * host: there is no concurrent servicer thread to starve and no wall-clock timer
 * to race. Pass ``nullptr`` to remove the hook; ::ra8_fake_mmio_reset also clears
 * it, so a hook never leaks between test cases.
 *
 * @param[in] hook Function invoked once per ::ra8_fake_mmio_poll call, or
 *                 ``nullptr`` to disable. Reads/writes only the RAM-backed fake
 *                 register windows.
 *
 * @pre Called from a host (``RA8_OFF_TARGET`` + ``UNIT_TEST``) test binary.
 * @post Subsequent ::ra8_fake_mmio_poll calls invoke @p hook (or none if nullptr).
 *
 * @note Test-only. Not thread-safe. The hook itself runs single-threaded, inline
 *       with the driver's poll.
 * @since 0.1.0
 */
void ra8_fake_mmio_set_poll_hook(void (*hook)(void));

/*
 * The per-poll hook ``ra8_fake_mmio_wait_eval(reg, iter, real_cond)`` is defined in
 * ra8_fake_mmio.c but DECLARED in ``ra8_hw_err.h`` (guarded by
 * ``RA8_OFF_TARGET && UNIT_TEST``). It is placed there, not here, because the
 * HAL waiters that call it include ``ra8_hw_err.h`` but have no include path to
 * this test-only header; declaring it in both would be a redundant declaration.
 * A test that calls it directly includes ``ra8_hw_err.h`` for that decl.
 */

#ifdef __cplusplus
}
#endif
