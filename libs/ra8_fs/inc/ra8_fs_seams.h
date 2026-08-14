/**
 * @file ra8_fs_seams.h
 * @brief The two dependency-injection seams `ra8_fs` offers its callers.
 * @ingroup grp_storage
 *
 * @details
 * `ra8_fs` is written for a world with no scheduler and no calendar, and that
 * stays its default. What this header adds is somewhere for a caller that HAS
 * one to put it:
 *
 *   - ::ra8_fs_set_lock installs the mutual exclusion taken around every
 *     public call, so an RTOS-world consumer does not have to wrap each call
 *     site by hand or ship a second filesystem to get a lock;
 *   - ::ra8_fs_set_clock installs the calendar the on-disk timestamps are
 *     stamped from, so a data logger's files carry a real date instead of the
 *     1980 epoch this library falls back to.
 *
 * Both are the same shape as ::ra8_fs_backend_t: a small struct of function
 * pointers plus a caller-owned cookie, copied on install, with no primitive,
 * no RTOS header and no clock driver named anywhere in this library. The
 * adapters that bind `tx_mutex_get` or `ra8_rtc_get()` live with the caller
 * that owns them.
 *
 * They live in their own header because they are the parts of the API a
 * caller wires ONCE at init and never touches again, and because
 * `ra8_fs.h` -- which describes the filesystem itself -- had grown past the
 * repository's 1000-line source cap carrying them. `ra8_fs.h` includes this
 * file, so nothing a consumer includes changes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs_types.h"

/* =============================================================================
 * Optional lock seam
 * =============================================================================
 */

/**
 * @struct ra8_fs_lock_t
 * @brief Dependency-injection seam for mutual exclusion around the public API.
 *
 * @details
 * Two function pointers and a cookie. `ra8_fs` never creates, destroys or names
 * a lock primitive: the caller owns one and hands over the two operations that
 * drive it, exactly as ::ra8_fs_backend_t hands over the two operations that
 * drive a block device. A ThreadX consumer binds `tx_mutex_get` /
 * `tx_mutex_put` over a `TX_MUTEX*`; a bare-metal-with-interrupts consumer
 * binds a PRIMASK save / restore pair. Neither adapter belongs in this library
 * and neither is present in it.
 *
 * Both callbacks are invoked with @ref ctx as their only argument, and neither
 * may fail: a lock whose acquire can return an error is not a lock this seam
 * can express, and swallowing that error would be worse than not offering the
 * seam. Wrap such a primitive in an adapter that blocks until it succeeds.
 *
 * The pair must be *re-entrant-free*: `acquire` is never called twice without
 * an intervening `release`, so a plain non-recursive mutex is the right shape.
 *
 * @invariant `acquire` and `release` are both non-NULL, or the whole struct is
 *            never installed (::ra8_fs_set_lock rejects a half-filled one).
 * @invariant `ctx` may be NULL; it is passed through untouched.
 *
 * @par Example:
 * @code
 * static void tx_acquire(void* ctx) { (void)tx_mutex_get((TX_MUTEX*)ctx, TX_WAIT_FOREVER); }
 * static void tx_release(void* ctx) { (void)tx_mutex_put((TX_MUTEX*)ctx); }
 *
 * static TX_MUTEX s_fs_mutex;
 * const ra8_fs_lock_t lock = {.acquire = tx_acquire, .release = tx_release, .ctx = &s_fs_mutex};
 * (void)ra8_fs_set_lock(&lock);
 * @endcode
 *
 * @see ra8_fs_set_lock()  Installs (or removes) the binding.
 * @since 0.1.0
 */
typedef struct {
  /**
   * @brief Take the lock; must block until it is held.
   * @param[in] ctx The @ref ctx cookie, passed through unmodified.
   */
  void (*acquire)(void* ctx);

  /**
   * @brief Release the lock taken by the matching `acquire` call.
   * @param[in] ctx The @ref ctx cookie, passed through unmodified.
   */
  void (*release)(void* ctx);

  /** @brief Caller-owned cookie handed back to both callbacks. */
  void* ctx;
} ra8_fs_lock_t;

/**
 * @brief Install (or remove) the lock taken around every public `ra8_fs` call.
 *
 * @details
 * There is one lock for the whole library, not one per mount: the state that
 * needs serialising -- the file-handle table, the mount table and the static
 * scratch sector -- is shared by every mount. Installing a lock therefore
 * serialises every public entry point against every other one.
 *
 * Passing NULL removes the binding and restores the default: no lock, no call,
 * and the caller is once again responsible for serialising access. That is the
 * state the library powers up in, so a bare-metal consumer never calls this
 * function at all and pays nothing for its existence.
 *
 * The binding is copied, so the caller's ::ra8_fs_lock_t need not outlive the
 * call; the object @ref ra8_fs_lock_t::ctx points at must, however, outlive
 * every subsequent `ra8_fs` call.
 *
 * Call this before the first `ra8_fs` call and before any other thread can
 * reach the library: swapping the binding while a call is in flight would
 * release a lock the caller never took.
 *
 * @param[in] lock Lock binding to install, or NULL to remove the current one.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Binding installed (or removed for a NULL @p lock).
 * @retval k_ra8_err_invalid_arg @p lock is non-NULL but `acquire` or `release`
 *                               is NULL -- a half-filled binding would leave
 *                               the library holding a lock it cannot drop.
 *
 * @pre No `ra8_fs` call is in flight on any thread.
 * @pre When @p lock is non-NULL, both of its callbacks are non-NULL.
 * @post On ::k_ra8_ok with a non-NULL @p lock, every subsequent public call
 *       brackets itself with `acquire` / `release`.
 * @post On ::k_ra8_ok with a NULL @p lock, no callback is invoked again.
 * @post On ::k_ra8_err_invalid_arg the previous binding is unchanged.
 *
 * @note Not thread-safe with respect to itself; this is an init-time call.
 * @warning Installing a lock does not make one open ::ra8_fs_file_t shareable
 *          between threads. It serialises the library, not a handle: two
 *          threads reading the same handle still race on its offset.
 *
 * @par MC/DC:
 * Decision: `if (lock->acquire == nullptr || lock->release == nullptr)`
 * -- vectors live in `tests/test_ra8_fs_lock.c`.
 *
 * @see ra8_fs_lock_t  The binding this installs.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_set_lock(const ra8_fs_lock_t* lock);

/* =============================================================================
 * Wall-clock seam (dependency injection)
 * =============================================================================
 */

/**
 * @struct ra8_fs_clock_t
 * @brief Caller-supplied calendar source used to stamp files.
 *
 * @details
 * One function pointer plus a cookie, in the shape every other seam in this
 * library uses (::ra8_fs_backend_t for blocks, ::ra8_fs_lock_t for mutual
 * exclusion). Production wiring is a three-line adapter over `ra8_rtc_get()`
 * that lives with the caller; the host tests inject a fake that returns a
 * fixed instant, which is what makes the on-disk bytes assertable.
 *
 * @invariant `now` is non-NULL in an installed binding (::ra8_fs_set_clock
 *            rejects one that is not).
 *
 * @par Example:
 * @code
 * static ra8_err_t rtc_now(void* ctx, ra8_fs_datetime_t* out) {
 *   (void)ctx;
 *   ra8_rtc_time_t r = {};
 *   ra8_err_t e = ra8_rtc_get(&r);
 *   if (e != k_ra8_ok) { return e; }
 *   out->year = r.year; out->month = r.month; out->day = r.day;
 *   out->hour = r.hour; out->minute = r.minute; out->second = r.second;
 *   return k_ra8_ok;
 * }
 * const ra8_fs_clock_t clk = { .now = rtc_now, .ctx = nullptr };
 * (void)ra8_fs_set_clock(&clk);
 * @endcode
 *
 * @see ra8_fs_set_clock()
 * @since 0.1.0
 */
typedef struct {
  /**
   * @brief Report the current civil time.
   * @param[in]  ctx Caller cookie from ::ra8_fs_clock_t::ctx.
   * @param[out] out Receives the reading; only written on ::k_ra8_ok.
   * @return ::k_ra8_ok on success; any other code makes the filesystem fall
   *         back to the FAT epoch for that one stamp.
   */
  ra8_err_t (*now)(void* ctx, ra8_fs_datetime_t* out);

  /** @brief Caller-owned context passed back into `now`. */
  void* ctx;
} ra8_fs_clock_t;

/**
 * @brief Install (or remove) the calendar source used to stamp files.
 *
 * @details
 * With no binding installed -- the default -- every timestamp `ra8_fs` writes
 * is 1980-01-01 00:00:00, the first instant both FAT and exFAT can express.
 * That is deliberate and is NOT the same as leaving the fields zero: a FAT
 * date of 0 encodes month 0 and day 0, and both fields are 1-based, so it is
 * not a calendar date at all. macOS (msdosfs) reads a zero date as
 * uninitialised and shows 31 Dec 1969, Linux clamps the fields to 1 and shows
 * 1980-01-01, and Windows shows a blank. No host agrees, and none of them is
 * showing a time. Writing the epoch makes every host agree.
 *
 * Installing a binding replaces the default for every subsequent stamp. A
 * `now` that returns anything but ::k_ra8_ok falls back to the epoch for that
 * stamp only -- a clock that has not been set yet must not stop a data logger
 * from writing.
 *
 * The binding is COPIED, so one built as a compound literal or on the
 * installing function's stack stays valid; the `ctx` cookie remains the
 * caller's to keep alive.
 *
 * @param[in] clock Binding to install, or NULL to go back to the epoch default.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Binding installed, or removed when @p clock was NULL.
 * @retval k_ra8_err_invalid_arg @p clock is non-NULL but its `now` is NULL.
 *
 * @pre No filesystem operation is in flight (this is an init-time call).
 * @pre @p clock, when non-NULL, has a `now` that does not itself call into `ra8_fs`.
 * @post On ::k_ra8_ok every later create/write/rename stamps from @p clock.
 * @post On ::k_ra8_err_invalid_arg the previously installed binding is unchanged.
 *
 * @note Not thread-safe; install before any concurrent filesystem use.
 * @warning Timestamps already on disk are not revisited. Installing a clock
 *          affects later writes only.
 *
 * @see ra8_fs_clock_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fs_set_clock(const ra8_fs_clock_t* clock);

#ifdef __cplusplus
}
#endif
