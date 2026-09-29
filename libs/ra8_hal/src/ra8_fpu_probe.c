/**
 * @file ra8_fpu_probe.c
 * @brief Double-precision FPU probe implementation (see ra8_fpu_probe.h)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Holds the single `double` product-sum whose object-code lowering witnesses
 * the FPU width of the build target: soft-float `__aeabi_d*` calls on every
 * default build, RA8D2 and RA8P1 alike (`-mfpu=fpv5-sp-d16`; the FPU is not a
 * delta between the parts, see issue #225), versus hardware `.f64` opcodes on an
 * opt-in `RA8P1_DP_FPU=ON` build (`-mfpu=fpv5-d16`). The authoritative contract
 * lives on the declaration in `ra8_fpu_probe.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_fpu_probe.h"

double ra8_fpu_dp_madd(double a, double b, double c)
{
  return (a * b) + c;
}
