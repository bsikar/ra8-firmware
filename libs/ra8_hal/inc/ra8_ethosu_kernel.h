/**
 * @file ra8_ethosu_kernel.h
 * @brief First-party TFLite-micro Ethos-U custom-op registration probe (RA8P1-only)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The first-party `ra8_ethosu_kernel.cc` provides the real TFLite-micro
 * `tflite::Register_ETHOSU()` operator -- replacing the vendored portable stub
 * that returned `nullptr` -- and dispatches the Ethos-U55 command stream to the
 * NPU through the `ra8_ethosu_shim` / `ra8_npu` driver. That kernel is C++ and
 * only compiles inside a `RA8_USE_TFLITE_MICRO` build (it needs the vendored
 * TFLite include tree), so it is built by `cmake/tflite_micro.cmake`, NOT by the
 * always-globbed `ra8_hal` C source set.
 *
 * This tiny C-callable header exposes ONE probe, ::ra8_ethosu_kernel_available,
 * so a plain C application (which cannot include the C++ TFLite headers) can
 * assert that the real Ethos-U operator is wired: that `Register_ETHOSU()`
 * returns a non-NULL registration whose `invoke` and `custom_name` are set. It
 * is the C entry point an `examples/ra8p1_foundation` app uses to prove the
 * TFLite-micro <-> NPU integration links and registers, independent of a full
 * Vela-compiled model run (which needs the offline Vela compiler, a follow-up).
 *
 * ## Gating
 *
 * RA8P1-only: guarded behind `RA8_HAS_NPU` exactly like `ra8_npu.h` /
 * `ra8_ethosu_shim.h`. On a device without an NPU this header declares nothing
 * and the matching kernel's `Register_ETHOSU()` returns `nullptr` (the CPU-only
 * TFLite path), so nothing here applies.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_device.h"

#ifdef RA8_HAS_NPU

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report whether the real TFLite-micro Ethos-U operator is registered.
 *
 * @details
 * Calls `tflite::Register_ETHOSU()` and validates the returned `TFLMRegistration`
 * is non-NULL, carries a non-NULL `invoke` handler, and its `custom_name` matches
 * `tflite::ethosu_custom_name()`. This is a link-time + registration proof that the
 * vendored `Register_ETHOSU()` stub (which returned `nullptr`) has been replaced
 * by the first-party NPU-dispatching kernel. It does NOT run an inference.
 *
 * @return `int` boolean.
 * @retval 1 The real Ethos-U operator is registered and self-consistent.
 * @retval 0 No usable Ethos-U registration (stub still in place or mismatch).
 *
 * @pre The build enabled `RA8_USE_TFLITE_MICRO` (so the kernel TU is linked).
 * @pre The build targets the RA8P1 (`RA8_HAS_NPU` defined).
 * @post No NPU register is touched (pure registration query).
 * @post No global state is modified.
 *
 * @note Re-entrant; the registration is a function-local static in the kernel.
 * @since 0.1.0
 */
int ra8_ethosu_kernel_available(void);

#ifdef __cplusplus
}
#endif

#endif /* RA8_HAS_NPU */
