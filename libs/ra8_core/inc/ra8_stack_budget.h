/**
 * @file ra8_stack_budget.h
 * @brief Greppable stack-frame deviation marker
 * @ingroup grp_core
 *
 * @details
 * Static stack-overflow is the silent equivalent of dynamic allocation
 * (NASA Power-of-10 Rule 3). This project caps individual stack frames
 * at 2048 bytes via the per-target `-Wstack-usage=N` warning attached
 * by `cmake/ra8_warnings.cmake`, and aggregates a project-wide report
 * via `scripts/checks/stack_usage_check.py`. See `docs/STACK_USAGE.md`
 * for the full procedure.
 *
 * When a function legitimately needs more stack than its module's
 * default budget, the deviation MUST be marked at the function site
 * with `RA8_STACK_BUDGET(N)` so it is greppable and so the next reader
 * sees the documented bound before refactoring. The macro itself is a
 * no-op marker; its job is to:
 *
 * 1. Be greppable -- `git grep RA8_STACK_BUDGET` lists every approved
 *    deviation in the codebase.
 * 2. Force a code-review conversation about *why* the frame is large
 *    (the per-app `STACK_USAGE_BYTES` ceiling must also be raised in
 *    the per-app `CMakeLists.txt`).
 * 3. Pin the documented bound `N` next to the function so a
 *    refactoring reader can reconcile the value against the
 *    `.su` file.
 *
 * Usage (place as the first statement inside the function body so the
 * preceding Doxygen block continues to be detected by doxy_audit.py):
 * @code{.c}
 * ra8_err_t ra8_rsip_protected_rsa_decrypt(...)
 * {
 *     RA8_STACK_BUDGET(1720); // approved frame ceiling per .su file
 *     // Holds a 512-byte modulus + handle + IV scratch on the stack
 *     // so the key material is scrubbed when the frame unwinds rather
 *     // than living forever in .bss.
 * }
 * @endcode
 *
 * Compliance:
 * - IEC 61508-3 Table A.4 (Highly Recommended: limited stack).
 * - DO-178C Section 6: resource-usage analysis must be substantiated.
 * - CWE-121 / CWE-674: bounded stack guard.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @def RA8_STACK_BUDGET
 * @brief Marker macro recording an approved stack-frame deviation.
 *
 * @details
 * Expands to nothing; the macro exists purely so `git grep` finds the
 * deviation site and so a code reviewer must touch this annotation to
 * change the budget. The numeric argument `N` is the maximum frame
 * size in bytes that the function is permitted to consume, as
 * verified against its `<file>.su` entry.
 *
 * @param[in] bytes Approved upper bound on this function's stack frame
 *                  (units: bytes, must match the corresponding `.su` line).
 *
 * @note Place as the first statement inside the function body (so
 *       the preceding Doxygen block remains adjacent to the function
 *       signature for doxy_audit.py). The deviation must also be
 *       documented in the function's `@details` Doxygen block
 *       (justification + alternative considered).
 *
 * @warning This macro does NOT raise the per-target `-Wstack-usage=N`
 *          ceiling -- the per-app `STACK_USAGE_BYTES` value in the
 *          app's `CMakeLists.txt` must be raised in lockstep.
 *
 * @see docs/STACK_USAGE.md  Full deviation procedure.
 * @see scripts/checks/stack_usage_check.py  Project-wide aggregator.
 *
 * @since 0.1.0
 */
#define RA8_STACK_BUDGET(bytes) ((void)0) /* approved frame budget = bytes */
