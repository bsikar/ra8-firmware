/**
 * @file ra8_sbrk_trap.c
 * @brief newlib _sbrk() trap stub -- enforces NASA Power of 10 Rule 3
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * Target firmware is freestanding and links with `-nostdlib` without newlib
 * or libnosys. General-purpose allocators (`malloc`, `free`, etc.) are unavailable
 * and fail closed at link time with undefined symbol errors.
 *
 * This file provides a strong `_sbrk` stub as defense-in-depth:
 *  1. If any legacy object or external routine attempts to resolve or invoke
 *     `_sbrk`, it halts loudly via `ra8_fatal_error` rather than silently
 *     corrupting memory or resolving to an unbounded bump allocator.
 *  2. In clean freestanding target firmware with zero heap callers, this object's
 *     sections are automatically discarded by linker garbage collection (`--gc-sections`).
 *  3. Linker scripts define no `end` anchor and no `.heap` section.
 *
 * Project policy (NASA Power of 10 Rule 3: "No dynamic memory after
 * initialisation") forbids any heap use in libs/ and src/.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sbrk_trap.h"

#include <stddef.h> // ra8-keep-include: `ptrdiff_t` used directly

#include "ra8_error_handler.h"

/* The newlib-nano heap allocator calls ``_sbrk`` by that exact name,
 * so we must use a reserved identifier here. clang-tidy's
 * bugprone-reserved-identifier / cert-dcl51-cpp complain correctly
 * but we override because the name is fixed by newlib. */

/* No coverage-exclusion marker here: the trap body is exercised
 * deterministically by the host white-box test
 * tests/hal/src/test_ra8_sbrk_trap_cov.c, which renames `_sbrk` to `_sbrk_cov`, mocks
 * the fatal-error sink, and proves the function never returns. On a
 * correctly-built firmware image this stays unreached at run time (glibc
 * malloc on the host resolves its own break), but the three lines are
 * covered on host, so no marker is needed. */
void* _sbrk(ptrdiff_t incr)
{
  (void)incr;
  ra8_fatal_error("SBRK", "_sbrk called -- firmware is heap-free", 0U);
  __builtin_unreachable();
}
