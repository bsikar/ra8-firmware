/**
 * @file ra8_fpu_probe.h
 * @brief Double-precision FPU probe -- codegen witness for the RA8P1 DP-FPU
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The two supported RA8 parts do NOT differ in FPU width (settled in issue
 * #225): FSP's CMSIS device headers declare `__FPU_PRESENT 1` and `__FPU_DP 0`
 * for the primary Cortex-M85 of BOTH parts (`R7KA8P1KF_core0.h` is
 * byte-identical to `R7KA8D2KF_core0.h` in that block), and the "half, single,
 * and double-precision" sentence in the RA8P1 datasheet appears verbatim in the
 * RA8D2 datasheet, so it describes the licensed Cortex-M85 r1p1 FPU rather than
 * an RA8P1 delta. Both parts therefore build `-mfpu=fpv5-sp-d16`, where `double`
 * arithmetic cannot run on the FPU and the compiler lowers it to soft-float
 * library calls (`__aeabi_dmul`, `__aeabi_dadd`, ...).
 *
 * A double-precision image is still reachable, as the bench switch the
 * on-silicon benchmark (#229) needs and never by default:
 * `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ra8p1.cmake -DRA8P1_DP_FPU=ON`
 * appends `-mfpu=fpv5-d16` and defines `RA8_FPU_DP_ENABLED`, and the same
 * `double` arithmetic then compiles to hardware `.f64` opcodes (`vmul.f64`,
 * `vadd.f64` / `vfma.f64`). Whether this silicon executes them at all is
 * unmeasured; it needs an RA8P1 EK.
 *
 * `ra8_fpu_dp_madd()` is a deliberately tiny `double` computation that every
 * build compiles. Disassembling its object (`arm-none-eabi-objdump -d`) is the
 * witness: an `RA8P1_DP_FPU=ON` build shows `.f64` VFP opcodes; every default
 * build shows soft-float `bl __aeabi_d*` calls. It doubles as a runtime sanity
 * check whose numeric result is validated by the host unit test.
 *
 * ::RA8_FPU_DP_SELECTED reports which of the two the compiler actually chose,
 * and the guard below refuses a build where that disagrees with the switch, so
 * the toolchain file and the code cannot drift apart silently.
 *
 * @note Host-friendly: pure numeric leaf function, touches no hardware, so it
 *       runs unchanged under `RA8_OFF_TARGET` and in the host unit tests
 *       (which compute it on the host's native binary64 hardware).
 *
 * @see cmake/toolchain-ra8p1.cmake  Carries the opt-in `RA8P1_DP_FPU` switch.
 * @see ra8_device.h                  RA8D2/RA8P1 compile-time device switch.
 * @see docs/reference/ra8p1_vs_ra8d2.md  The sourced #225 resolution.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/**
 * @def RA8_FPU_DP_SELECTED
 * @brief 1 when the compiler selected a double-precision FPU, else 0.
 *
 * @details
 * Read off ACLE's `__ARM_FP` bitmap, whose `0x8` bit means the selected FPU
 * supports double precision. Measured with the pinned Arm GNU Toolchain
 * 13.3.Rel1 on `-mcpu=cortex-m85 -mfloat-abi=hard`: `-mfpu=fpv5-sp-d16` gives
 * `__ARM_FP == 0x4` (single only), `-mfpu=fpv5-d16` gives `__ARM_FP == 0xE`
 * (half + single + double). A host build defines no `__ARM_FP` at all, so this
 * is 0 off-target, which is correct: the host runs `double` on its own
 * hardware, not on an RA8 FPU.
 */
#if defined(__ARM_FP) && (((__ARM_FP) & 0x8) != 0)
#define RA8_FPU_DP_SELECTED 1
#else
#define RA8_FPU_DP_SELECTED 0
#endif

/* The toolchain file defines RA8_FPU_DP_ENABLED only for an opt-in
 * RA8P1_DP_FPU=ON build, which is also the only build whose -mfpu carries double
 * precision. Either half without the other means the flags and the intent have
 * drifted -- a silent soft-float image where a DP benchmark was asked for, or
 * .f64 opcodes in an image nothing asked to be DP -- so refuse the build and say
 * which way round it went. Only checked on a target build; a host build has no
 * -mfpu to disagree with. */
#ifdef __ARM_FP
#if defined(RA8_FPU_DP_ENABLED) && (RA8_FPU_DP_SELECTED == 0)
#error "RA8_FPU_DP_ENABLED is set but -mfpu selected no double-precision FPU (see #225)"
#endif
#if !defined(RA8_FPU_DP_ENABLED) && (RA8_FPU_DP_SELECTED == 1)
#error "-mfpu selected a double-precision FPU without RA8P1_DP_FPU=ON (see #225)"
#endif
#endif

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
