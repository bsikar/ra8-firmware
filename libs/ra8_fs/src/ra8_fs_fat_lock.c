/*
 */
/**
 * @file ra8_fs_fat_lock.c
 * @brief The optional mutual-exclusion seam taken around every public call.
 *
 * @details
 * `ra8_fs` was written for a world with no scheduler, and its shared state --
 * the file-handle table, the mount table, the one static scratch sector -- is
 * serialised by there being nobody else to race. That is the correct default
 * and it stays the default: with no binding installed the two helpers here
 * compile down to a load and a branch, and nothing else in the library changes.
 *
 * What this file adds is somewhere for an RTOS-world caller to put its mutex,
 * so the alternative is not "wrap every call site by hand" or "ship a second
 * filesystem to get a lock". The binding is a pair of function pointers plus a
 * cookie (::ra8_fs_lock_t); no lock primitive, no RTOS header and no scheduler
 * concept appears in this library, and the adapter that binds `tx_mutex_get` /
 * `tx_mutex_put` lives with the caller that owns the mutex.
 *
 * The lock is taken by the public entry points only. Every one of them is a
 * wrapper of the shape
 *
 *     priv_lock_acquire(); e = priv_<op>_locked(...); priv_lock_release();
 *
 * so a return path that forgets to release cannot be written -- there is one
 * return per wrapper -- and every guarded implementation carries
 * `RA8_EXPECTS_LOCK("ra8_fs_lock")` so the annotation checker, not a comment,
 * is what says no internal helper may take it a second time.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/**
 * @var s_lock
 * @brief The installed lock binding; meaningful only when `s_lock_installed`.
 *
 * @details A copy of the caller's ::ra8_fs_lock_t rather than a pointer to it,
 *          so a binding built as a compound literal or on the installing
 *          function's stack stays valid for the life of the program. The
 *          `ctx` cookie is still the caller's to keep alive.
 *
 * @note Written only by ::ra8_fs_set_lock, which is an init-time call.
 * @warning Never modify directly; a half-updated binding would leave the
 *          library able to take a lock it cannot drop.
 * @since 0.1.0
 */
static ra8_fs_lock_t s_lock;

/**
 * @var s_lock_installed
 * @brief True once a complete binding has been installed.
 *
 * @details Kept separate from `s_lock` so "no lock" is one predictable branch
 *          rather than a NULL test on a function pointer that the compiler
 *          must reload.
 *
 * @note Written only by ::ra8_fs_set_lock.
 * @warning Never modify directly.
 * @since 0.1.0
 */
static bool s_lock_installed;

/* `priv_lock_acquire()`: see header for the documented contract. */
void priv_lock_acquire(void)
{
  if (!s_lock_installed) {
    return;
  }
  s_lock.acquire(s_lock.ctx);
}

/* `priv_lock_release()`: see header for the documented contract. */
void priv_lock_release(void)
{
  if (!s_lock_installed) {
    return;
  }
  s_lock.release(s_lock.ctx);
}

ra8_err_t ra8_fs_set_lock(const ra8_fs_lock_t* lock)
{
  if (lock == nullptr) {
    s_lock_installed = false;
    s_lock           = (ra8_fs_lock_t){};
    return k_ra8_ok;
  }
  if ((lock->acquire == nullptr) || (lock->release == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  s_lock           = *lock;
  s_lock_installed = true;
  return k_ra8_ok;
}
