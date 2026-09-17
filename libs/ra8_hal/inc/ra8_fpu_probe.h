/**
 * @file ra8_fpu_probe.h
 * @brief Double-precision FPU probe -- codegen witness for the RA8P1 DP-FPU
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The two supported RA8 parts differ in FPU width even though both are a
 * Cortex-M85:
 *
 * - **RA8D2**: single-precision FPv5 (`__FPU_DP == 0`). The RA8D2 toolchain
 *   file compiles with `-mfpu=fpv5-sp-d16`, so `double` arithmetic cannot run
 *   on the FPU and the compiler lowers it to soft-float library calls
 *   (`__aeabi_dmul`, `__aeabi_dadd`, ...).
 * - **RA8P1**: double-precision FPv5 (the M85 DP-FPU). The RA8P1 toolchain file
 *   (`cmake/toolchain-ra8p1.cmake`) overrides to `-mfpu=fpv5-d16`, so the same
 *   `double` arithmetic compiles to hardware DP-FPU opcodes (`vmul.f64`,
 *   `vadd.f64` / `vfma.f64`).
 *
 * `ra8_fpu_dp_madd()` is a deliberately tiny `double` computation that both
 * device builds compile. Disassembling its object (`arm-none-eabi-objdump -d`)
 * is the witness: the RA8P1 build shows `.f64` VFP opcodes; the RA8D2 build
 * shows soft-float `bl __aeabi_d*` calls. It doubles as a runtime DP-FPU sanity
 * check whose numeric result is validated by the host unit test.
 *
 * @note Host-friendly: pure numeric leaf function, touches no hardware, so it
 *       runs unchanged under `RA8_OFF_TARGET` and in the host unit tests
 *       (which compute it on the host's native binary64 hardware).
 *
 * @see cmake/toolchain-ra8p1.cmake  Overrides `-mfpu` to the DP-FPU for RA8P1.
 * @see ra8_device.h                  RA8D2/RA8P1 compile-time device switch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Evaluate the double-precision product-sum `a * b + c`.
 *
 * @details
 * A minimal IEEE-754 binary64 (`double`) computation used as a compile-target
 * witness for the FPU width (see the file header). On a DP-FPU build the body
 * lowers to hardware `.f64` opcodes; on an SP-FPU build it lowers to soft-float
 * calls. The math itself -- multiply then add, or a fused multiply-add -- is
 * identical in both cases and is exercised for numeric correctness by the host
 * unit test.
 *
 * @param[in] a First multiplicand, any finite `double`.
 * @param[in] b Second multiplicand, any finite `double`.
 * @param[in] c Addend, any finite `double`.
 *
 * @return The product-sum evaluated in `double` precision.
 * @retval a*b+c The IEEE-754 binary64 result (subject to a single rounding, or
 *               fewer if the compiler contracts to a fused multiply-add).
 *
 * @pre `a`, `b`, `c` are ordinary `double` values (no NaN/Inf constraint --
 *      they propagate per IEEE-754).
 * @pre The build selected an FPU via `-mfpu` (hard-float ABI in this project).
 * @post No hardware or global state is modified.
 * @post For finite inputs whose exact product-sum is representable, the result
 *       equals `a * b + c`.
 *
 * @note Thread-safe: pure function, no shared or static state.
 *
 * @see cmake/toolchain-ra8p1.cmake
 * @since 0.1.0
 */
double ra8_fpu_dp_madd(double a, double b, double c);

#ifdef __cplusplus
}
#endif
