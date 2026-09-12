/**
 * @file ra8_freestanding_math.c
 * @brief Project-owned freestanding integer math primitive (abs).
 *
 * @par Tag
 * [Ring 0 / Foundation] {World: Dual}
 *
 * @details
 * Standard ISO C integer absolute value primitives implemented directly for
 * target firmware without libc or newlib dependencies.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_freestanding.h"

#if !defined(RA8_OFF_TARGET) || defined(RA8_TEST_FREESTANDING)

/**
 * @brief Compute absolute value of integer.
 * @details Returns the non-negative magnitude of @p j.
 * @param[in] j Signed integer value.
 * @return Absolute value of @p j.
 * @retval value Non-negative integer.
 * @pre @p j is not INT_MIN.
 * @pre Standard two's complement integer environment.
 * @post Returned value is non-negative.
 * @post Input @p j is unmodified.
 * @note Pure arithmetic function; never allocates; reentrant and thread-safe.
 * @since 0.1.0
 */
int abs(int j)
{
  return (j < 0) ? -j : j;
}
#endif /* !RA8_OFF_TARGET || RA8_TEST_FREESTANDING */
