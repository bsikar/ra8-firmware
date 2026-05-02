/**
 * @file ra_sbrk_trap.c
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

#include <stdint.h>

#include "ra_error_handler.h"

/**
 * @brief Trap stub for newlib's program-break function.
 *
 * @details
 * newlib calls ``_sbrk`` whenever its internal heap allocator needs
 * more RAM. Since this project is heap-free, the only way this
 * function gets called is if someone accidentally introduces a
 * newlib allocator dependency (``malloc`` / ``strdup`` / the non-nano
 * ``printf`` family, etc.). We trap straight into the project's
 * ``internal_ra_fatal_error`` which masks IRQs, logs the violation,
 * drops into ``__BKPT(0)`` if a debugger is attached, and parks on
 * ``__WFI``.
 *
 * @param[in] incr Byte count newlib wants to advance the break by.
 *                 Ignored -- we never return a valid block.
 * @return Never returns.
 *
 * @note Thread safety: irrelevant -- the call never returns.
 * @since 0.1.0
 */
/* The newlib-nano heap allocator calls ``_sbrk`` by that exact name,
 * so we must use a reserved identifier here. clang-tidy's
 * bugprone-reserved-identifier / cert-dcl51-cpp complain correctly
 * but we override because the name is fixed by newlib. */
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
void* _sbrk(int32_t incr);

/**
 * @brief Trap-stub definition for newlib `_sbrk()`.
 *
 * @details Never returns. See the matching declaration above for the
 *          full rationale and policy notes.
 *
 * @param[in] incr Byte count newlib wants to advance the break by.
 *                 Ignored.
 *
 * @return Does not return.
 * @retval None
 *
 * @pre Build links this strong symbol ahead of newlib's libc.
 * @pre Caller is newlib's malloc path -- not user code.
 * @post Control never returns to the caller; firmware halts.
 * @post Fatal-error sink emits the violation tag.
 *
 * @note Not thread-safe and not intended to be -- never returns.
 *
 * @since 0.1.0
 */
/* GCOVR_EXCL_START -- trap function; by design never reached in a
 * correctly-built firmware image. If coverage suddenly lights up
 * here, a caller has dragged malloc() into the link. */
void* _sbrk(int32_t incr)
{
  (void)incr;
  internal_ra_fatal_error("SBRK", "_sbrk called -- firmware is heap-free", 0U);
}
/* GCOVR_EXCL_STOP */
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
