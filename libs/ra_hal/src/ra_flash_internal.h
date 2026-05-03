/**
 * @file ra_flash_internal.h
 * @brief Test-access surface for ra_flash internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_flash facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
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
 * @brief Pure (state-free) reimplementation of @c internal_window_allows.
 *
 * @details Returns true iff @p addr / @p len falls within the
 *          inclusive lower bound @p win_low and exclusive upper bound
 *          @p win_high. The "no window installed" sentinel is
 *          @c (win_low == 0 && win_high == 0).
 *
 *          Promoted as a pure helper so tests can drive the line-722
 *          ``win_low == 0 && win_high == 0`` AND-decision under
 *          -fcoverage-mcdc on the production source. The
 *          state-reading wrapper @c internal_window_allows simply
 *          forwards to this function.
 *
 * @param[in] addr     Start address of the candidate region.
 * @param[in] len      Length in bytes of the candidate region.
 * @param[in] win_low  Inclusive lower bound of the allow window.
 * @param[in] win_high Exclusive upper bound of the allow window.
 *
 * @return Boolean window predicate.
 * @retval true  Region is permitted (or no window installed).
 * @retval false Region overlaps outside the installed window.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives line 722 ``s_rt.win_low == 0U && s_rt.win_high == 0U``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
bool ra_flash_internal_window_allows_pure(uintptr_t addr,
                                          uint32_t  len,
                                          uintptr_t win_low,
                                          uintptr_t win_high);

#ifdef __cplusplus
}
#endif
