/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sbrk_trap.h
 * @brief The newlib heap syscall, replaced by a halting trap
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / CORE] {World: S}
 *
 * @details
 * NASA Power of 10 Rule 3 forbids dynamic allocation after init, and this
 * firmware has no heap at all. ``ra8_sbrk_trap.c`` defines a strong
 * ``_sbrk`` so any code that reaches newlib's allocator halts loudly at
 * the exact call rather than silently corrupting whatever sits past the
 * break.
 *
 * The symbol is external because the C library resolves it -- newlib's
 * ``malloc`` calls ``_sbrk`` by name, and nothing in this tree calls it
 * at all. This header exists so the definition and the host death-test
 * that drives it agree on one prototype instead of restating it: they had
 * a copy each, and nothing checked them against each other.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)

/**
 * @brief Trap that replaces newlib's heap-extension syscall.
 *
 * @details
 * Reports a fatal error and never returns. Reaching this means something
 * called ``malloc``, ``calloc``, ``realloc``, ``strdup`` or a C++
 * ``operator new`` on the target, which the firmware's zero-heap design
 * forbids -- so it halts at the call site rather than handing back
 * storage that does not exist.
 *
 * @param[in] incr Bytes newlib wants the program break advanced by.
 *                 Ignored: the trap never satisfies the request.
 *
 * @return Never returns. The signature matches newlib's expectation so
 *         the strong definition overrides the library's own.
 * @retval NULL Unreachable; present only to satisfy the newlib contract.
 *
 * @pre The fatal-error sink is available (it is from reset).
 * @pre The image linked this TU, so the trap overrides newlib's `_sbrk`.
 * @post Control never returns to the caller; the firmware halts.
 * @post The fatal-error sink has emitted the violation tag.
 *
 * @note Not thread-safe and not intended to be -- it never returns.
 * @warning Do not call this directly. It exists for the C library to
 *          resolve and for the host death-test to drive.
 *
 * @par Example:
 * @code
 * // Host death test: prove the trap never returns.
 * (void)_sbrk(32);  // does not come back
 * @endcode
 *
 * @see ra8_fatal_error()
 *
 * @since 0.1.0
 */
void* _sbrk(int32_t incr);

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)

#ifdef __cplusplus
}
#endif
