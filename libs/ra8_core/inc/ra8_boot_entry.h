/**
 * @file ra8_boot_entry.h
 * @brief Boot entry points shared between a vector table and its startup code
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / BOOT] {World: S}
 *
 * @details
 * Two symbols cross the boundary between an image's ``vector_table.c`` and
 * its ``system_init.c``: ``Reset_Handler`` calls ``SystemInit`` before it
 * touches ``.data`` / ``.bss``, and the unused exception slots weak-alias
 * to ``Default_Handler``. Every handler that the vector table names
 * directly is reached by the CPU through VTOR and needs no prototype;
 * these two are reached by ordinary C linkage and do.
 *
 * They used to be restated as bare ``void SystemInit(void);`` lines in
 * two dozen boot translation units -- the two board boot directories, the
 * shared ``src/app`` pair, and every application that keeps its own boot
 * files. Twenty-four copies of a prototype are twenty-four chances for a
 * signature to drift with nothing to catch it, so this header holds the
 * one copy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Earliest C code the image runs: bring the core up before RAM init.
 *
 * @details
 * Called by ``Reset_Handler`` before ``.data`` is copied and ``.bss`` is
 * zeroed, so it must not touch initialised or zero-initialised statics.
 * Each image supplies its own definition -- the shared board one under
 * the board boot directories, or an application-local
 * ``system_init.c`` where the clock tree or memory map diverges.
 *
 * The name is the CMSIS convention; the startup code reaches it by that
 * name, which is why it has external linkage.
 *
 * @pre The CPU is at the reset vector with interrupts masked.
 * @pre ``.data`` has not been copied and ``.bss`` has not been zeroed.
 * @post The clock tree, cache and memory controller are configured.
 * @post It is safe for ``Reset_Handler`` to initialise RAM and call `main`.
 *
 * @note Not thread-safe; single-threaded reset context only.
 * @warning Must not read or write any static that startup has yet to
 *          initialise -- at this point they hold whatever SRAM held.
 *
 * @see Default_Handler()
 *
 * @since 0.1.0
 */
void SystemInit(void);

/**
 * @brief Catch-all handler for every exception slot nothing else claims.
 *
 * @details
 * The core exception vectors are declared weak and aliased here, so an
 * unclaimed exception lands in this function instead of an unmapped
 * address. An application overrides any one of them by defining a
 * non-weak function with the matching handler name.
 *
 * @pre The CPU has taken an exception with no dedicated handler.
 * @pre The vector table is the one at VTOR.
 * @post Control does not return to the interrupted context.
 * @post The image is halted or reset, per the board's policy.
 *
 * @note Callable from interrupt context; it is an exception handler.
 * @warning Reaching this means an exception was not wired up. Treat it as
 *          a bug in the image, not as a recovery path.
 *
 * @see SystemInit()
 *
 * @since 0.1.0
 */
void Default_Handler(void);

#ifdef __cplusplus
}
#endif
