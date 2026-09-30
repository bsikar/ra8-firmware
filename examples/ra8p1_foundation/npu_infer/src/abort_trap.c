/**
 * @file abort_trap.c
 * @brief C runtime abort() trap for the TFLite-micro link
 *
 * @par Tag
 * [Ring 3 / App] {World: S}
 *
 * @details
 * The vendored TensorFlow Lite for Microcontrollers runtime calls `abort()`
 * from its tensor-accessor error paths (`kernel_util.cc` and `micro_utils.cc`).
 * This app links `-nostdlib`, so there is no libc to resolve that symbol and
 * the image failed to link at all (issue #2500) with five undefined references.
 *
 * Rather than pull in newlib for one symbol, this translation unit provides a
 * strong `abort` that routes into the project's fatal-error policy and never
 * returns. Reaching it means a TFLite kernel hit an unrecoverable tensor
 * state, which on this target is a halt, not a wind-down: there is nothing to
 * unwind to and no host to return an exit status to.
 *
 * Scope note: this shim is deliberately app-local rather than living in
 * `ra8_core` beside `ra8_sbrk_trap.c`. A strong `abort` in `ra8_core` would
 * be linked into the host test binaries too, where it would override glibc's
 * `abort` and hijack every death test and failed assertion in the suite --
 * `tests/hal/src/test_ra8_sbrk_trap_cov.c` calls `abort()` on purpose to prove
 * the sbrk trap never returns. `npu_infer` is the only app that pulls the
 * TFLite-micro runtime, and the `ra8p1_foundation` tier is excluded from both
 * the unified RA8D2 build and the host build, so the symbol stays where the
 * dependency is.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h> // ra8-keep-include: canonical `abort` declaration this TU overrides

#include "ra8_error_handler.h"

/**
 * @brief Trap that replaces the C library's `abort()` for this image.
 *
 * @details
 * Reports a fatal error and never returns. The TFLite-micro runtime calls
 * `abort()` when a tensor accessor is handed an index it cannot satisfy;
 * on freestanding firmware that is a halt at the call site rather than a
 * silent fall-through into undefined behaviour.
 *
 * @return Never returns. The signature matches the C library's so this
 *         strong definition resolves the runtime's references.
 *
 * @pre The fatal-error sink is available (it is from reset).
 * @pre The image linked this TU, so the trap resolves TFLite's `abort`.
 * @post Control never returns to the caller; the firmware halts.
 * @post The fatal-error sink has emitted the violation tag.
 *
 * @note Not thread-safe and not intended to be -- it never returns.
 * @warning Do not call this directly. It exists for the TFLite-micro
 *          runtime to resolve.
 *
 * @see ra8_fatal_error()
 * @see ra8_sbrk_trap.c
 *
 * @since 0.1.0
 */
void abort(void)
{
  ra8_fatal_error("ABORT", "abort() called -- tflite-micro runtime fault", 0U);
  __builtin_unreachable();
}
