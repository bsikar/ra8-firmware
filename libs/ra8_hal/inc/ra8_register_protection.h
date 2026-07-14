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
 * @brief Scoped PRCR unlock block.
 *
 * @details
 * Expands to a `for`-loop that runs exactly once, with the unlock in
 * the init clause and the re-lock in the increment clause. Using a
 * `for` instead of a plain do-while lets callers `break` or `return`
 * out of the body without leaving PRCR unlocked.
 *
 * @param unlock_val One of the `k_ra8_prcr_unlock_*` values from
 *                   `ra8_system_regs.h`.
 */
#define RA8_PROTECTED_WRITE(unlock_val)                                                            \
  for (uint32_t ra8_prot_once_ = ((*ra8_sys_prcr() = (uint16_t)(unlock_val)), 1U);                 \
       ra8_prot_once_ != 0U;                                                                       \
       (*ra8_sys_prcr() = (uint16_t)k_ra8_prcr_lock_all), ra8_prot_once_ = 0U)

#ifdef __cplusplus
}
#endif
