/**
 * @file ra8_host_asm_stub.c
 * @brief Host-safe definitions of the bare CPU intrinsics (issue #293)
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The HAL drivers reach the Armv8-M CPU primitives through the shared seam
 * ``ra8_hw_intrinsics.h``. On the arm-none-eabi target that header inlines the
 * real asm; on the host unit-test build it declares the primitives ``extern``
 * and this single translation unit supplies their host-safe bodies:
 *
 *   - ``ra8_hw_wfi``            -> return (an x86 test binary must not halt)
 *   - ``ra8_hw_dsb`` / ``_isb`` -> compiler barrier (host RAM needs no HW fence)
 *   - ``ra8_hw_nop``            -> return (a calibrated pad has no host meaning)
 *   - ``ra8_hw_irq_enable/disable`` -> return (IRQs dispatch via ra8_fake_irq)
 *   - ``ra8_hw_wait_for_reset`` -> return (so a test can inspect the reset write)
 *
 * Keeping every host substitution here is what lets the driver ``.c`` files
 * stay free of any ``#ifdef RA8_OFF_TARGET`` -- the seam, not the driver,
 * owns the divergence. Each function's documented contract lives on its
 * declaration in ``ra8_hw_intrinsics.h``; per the definition-site policy the
 * bodies below carry no restating comment.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA8_OFF_TARGET

#include "ra8_hw_intrinsics.h"

void ra8_hw_wfi(void)
{
  /* Returning at once is the whole point on the host. */
}

void ra8_hw_dsb(void)
{
  __asm__ volatile("" ::: "memory");
}

void ra8_hw_isb(void)
{
  __asm__ volatile("" ::: "memory");
}

void ra8_hw_nop(void)
{
  /* A calibrated delay pad has no meaning on the host. */
}

void ra8_hw_irq_enable(void)
{
  /* Host IRQs dispatch through the ra8_fake_irq seam, not PRIMASK. */
}

void ra8_hw_irq_disable(void)
{
  /* Host IRQs dispatch through the ra8_fake_irq seam, not PRIMASK. */
}

void ra8_hw_wait_for_reset(void)
{
  /* Return so the test can inspect the reset-request write it staged. */
}

#else
/* On-target build: this translation unit is empty. */
typedef int ra8_host_asm_stub_placeholder_t;
#endif /* RA8_OFF_TARGET */
