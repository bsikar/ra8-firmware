/**
 * @file ra8_register_protection.h
 * @brief Scoped PRCR unlock helper
 * @ingroup grp_hal_system
 *
 * @details
 * Many CGC / LVD / LPM registers on the RA8D2 are gated by the PRCR
 * (Protect Register). The unlock sequence is:
 *
 * @code{.c}
 *   ra8_sys_prcr_unlock_cgc();
 *   // ... writes ...
 *   ra8_sys_prcr_lock_all();
 * @endcode
 *
 * It is easy to forget the re-lock on error paths. This header
 * provides the `RA8_PROTECTED_WRITE` helper macro which:
 *
 *  1. Unlocks the requested protection group.
 *  2. Executes the caller's block.
 *  3. Re-locks (unconditionally -- also on early return).
 *
 * Usage:
 *
 * @code{.c}
 *   RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_cgc) {
 *       *ra8_sys_sckscr() = k_ra8_cksel_hoco;
 *   }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_system_regs.h"

/**
 * @brief Open a scoped PRCR unlock: write @p unlock_val, return the loop flag.
 *
 * @details
 * Side-effecting init half of ::RA8_PROTECTED_WRITE. Writing the unlock
 * password to PRCR is the whole point of the guard; returning the sentinel
 * `1U` lets the macro seed its run-once `for` counter without a comma
 * operator, so the expansion is MISRA-C 2012 12.3 / 14.2 clean at every
 * call site (the previous inline-comma form charged both rules per use).
 *
 * @param[in] unlock_val One of the `k_ra8_prcr_unlock_*` values from
 *                       `ra8_system_regs.h`.
 * @return Always `1U` -- the sentinel that makes the guard body run once.
 * @retval 1 Unconditional; PRCR now holds @p unlock_val.
 *
 * @pre @p unlock_val carries the 0xA5 password in its upper byte.
 * @pre Caller is single-threaded init context or has interrupts masked.
 * @post PRCR equals @p unlock_val (the requested group is unlocked).
 * @post The return value is non-zero so the scope body executes.
 * @note Not thread-safe; the PRCR is a single global write-protect register.
 * @since 0.1.0
 */
static inline uint32_t ra8_prot_scope_begin(uint16_t unlock_val)
{
  *ra8_sys_prcr() = unlock_val;
  return 1U;
}

/**
 * @brief Close a scoped PRCR unlock: re-lock every group, return the exit flag.
 *
 * @details
 * Side-effecting update half of ::RA8_PROTECTED_WRITE. Delegates the actual
 * write to ::ra8_sys_prcr_lock_all and returns the sentinel `0U`, which the
 * macro assigns to its run-once counter so the `for` third clause modifies
 * only that counter -- keeping the expansion MISRA-C 2012 14.2 well-formed.
 *
 * @return Always `0U` -- the sentinel that ends the run-once loop.
 * @retval 0 Unconditional; PRCR is re-locked.
 *
 * @pre A matching ::ra8_prot_scope_begin unlocked PRCR earlier in the scope.
 * @pre Caller is single-threaded init context or has interrupts masked.
 * @post Every PRCR protection group is re-locked.
 * @post The return value is zero so the guard loop terminates.
 * @note Not thread-safe; the PRCR is a single global write-protect register.
 * @since 0.1.0
 */
static inline uint32_t ra8_prot_scope_end(void)
{
  ra8_sys_prcr_lock_all();
  return 0U;
}

/**
 * @brief Scoped PRCR unlock block.
 *
 * @details
 * Expands to a `for`-loop that runs exactly once, with the unlock in
 * the init clause and the re-lock in the increment clause. Using a
 * `for` instead of a plain do-while lets callers `break` or `return`
 * out of the body without leaving PRCR unlocked. The unlock and re-lock
 * writes are hidden behind ::ra8_prot_scope_begin / ::ra8_prot_scope_end
 * so the expansion carries no comma operator and each clause touches only
 * the loop counter -- MISRA-C 2012 12.3 and 14.2 clean at every call site.
 *
 * @param[in] unlock_val One of the `k_ra8_prcr_unlock_*` values from
 *                       `ra8_system_regs.h`.
 */
#define RA8_PROTECTED_WRITE(unlock_val)                                                            \
  for (uint32_t ra8_prot_once_ = ra8_prot_scope_begin((uint16_t)(unlock_val));                     \
       ra8_prot_once_ != 0U;                                                                       \
       ra8_prot_once_ = ra8_prot_scope_end())

#ifdef __cplusplus
}
#endif
