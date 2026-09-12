/**
 * @file ra8_sbrk_trap.h
 * @brief The newlib heap syscall, replaced by a halting trap
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / CORE] {World: S}
 *
 * @details
 * NASA Power of 10 Rule 3 forbids dynamic allocation after init, and target
 * firmware has no heap at all. Firmware is freestanding and links with `-nostdlib`
 * without newlib or libnosys. Standard allocator calls fail closed at link time.
 * ``ra8_sbrk_trap.c`` provides a defense-in-depth tripwire so any legacy or
 * external routine attempting to invoke ``_sbrk`` halts safely via ``ra8_fatal_error``.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) -- toolchain ABI fixes this reserved symbol spelling.

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
void* _sbrk(ptrdiff_t incr);

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)

#ifdef __cplusplus
}
#endif
