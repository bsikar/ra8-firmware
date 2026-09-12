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

#if defined(__ARM_EABI__)
/**
 * @brief Provide the errno ABI hook required by the approved ARM libm members.
 *
 * @details The freestanding target deliberately omits newlib, but the ARM
 *          libm wrapper for ``sqrtf`` retains its standard-domain-error hook.
 *          A single boot-time math-error slot is sufficient because target
 *          callers use these wrappers synchronously and never inspect errno;
 *          providing the hook keeps the approved libm implementation while
 *          avoiding a libc dependency.
 *
 * @return Address of the project-owned math-error slot.
 * @retval non-null Always returns the address of the slot.
 *
 * @pre None.
 * @post No heap or libc state is touched.
 * @note This symbol is an ARM newlib ABI compatibility hook, not a public C API.
 * @since 0.1.0
 */
int* __errno(void);

int* __errno(void)
{
  static int s_ra8_libm_errno;
  return &s_ra8_libm_errno;
}
#endif /* __ARM_EABI__                             */
#endif /* !RA8_OFF_TARGET || RA8_TEST_FREESTANDING */
