/**
 * @file ra_fpu_probe.c
 * @brief Double-precision FPU probe implementation (see ra_fpu_probe.h)
 *
 * @details
 * Holds the single `double` product-sum whose object-code lowering witnesses
 * the FPU width of the build target: hardware `.f64` opcodes on the RA8P1
 * DP-FPU build (`-mfpu=fpv5-d16`) versus soft-float `__aeabi_d*` calls on the
 * RA8D2 SP-FPU build (`-mfpu=fpv5-sp-d16`). The authoritative contract lives on
 * the declaration in `ra_fpu_probe.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra_fpu_probe.h"

double ra_fpu_dp_madd(double a, double b, double c)
{
  return (a * b) + c;
}
