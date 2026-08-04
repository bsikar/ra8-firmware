/**
 * @file ra8_sbrk_trap.c
 * @brief newlib _sbrk() trap stub -- enforces NASA Power of 10 Rule 3
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * The project links newlib-nano (via ``--specs=nano.specs``) which
 * pulls in ``malloc`` / ``free`` / the stdio allocator if any caller
 * happens to reference them. Newlib's heap resolves through
 * ``_sbrk(int incr)``, a POSIX-ish contract that returns the old
 * program-break and advances it by ``incr``. Providing our own
 * ``_sbrk`` that always fails at link/run time guarantees:
 *
 *  1. The linker still resolves the newlib symbol (so nano.specs is
 *     happy), meaning we do NOT need to drop nano.specs and lose
 *     size-optimised libc.
 *  2. Any accidental future call to ``malloc`` / ``printf`` / etc.
 *     traps cleanly into the error handler instead of silently
 *     returning a heap pointer from SRAM that the project policy
 *     says must not exist.
 *  3. The old ``.heap`` region in the linker script can be deleted;
 *     4 KB of SRAM frees up.
 *
 * Project policy (CLAUDE.md: "Zero dynamic allocation in firmware
 * (safety-critical)" and NASA Power of 10 Rule 3: "No dynamic memory
 * after initialisation") forbids any heap use in libs/ and src/.
 * This file is the hard enforcement layer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sbrk_trap.h"

#include <stdint.h>

#include "ra8_error_handler.h"

/* The newlib-nano heap allocator calls ``_sbrk`` by that exact name,
 * so we must use a reserved identifier here. clang-tidy's
 * bugprone-reserved-identifier / cert-dcl51-cpp complain correctly
 * but we override because the name is fixed by newlib. */
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming) -- newlib fixes the _sbrk name.

/* No coverage-exclusion marker here: the trap body is exercised
 * deterministically by the host white-box test
 * tests/test_ra8_sbrk_trap_cov.c, which renames `_sbrk` to `_sbrk_cov`, mocks
 * the fatal-error sink, and proves the function never returns. On a
 * correctly-built firmware image this stays unreached at run time (glibc
 * malloc on the host resolves its own break), but the three lines are
 * covered on host, so no marker is needed. */
void* _sbrk(int32_t incr)
{
  (void)incr;
  internal_ra8_fatal_error("SBRK", "_sbrk called -- firmware is heap-free", 0U);
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
