/**
 * @file libs/ra8_board_ek_ra8d2/inc/ra8_boot_intrinsics.h
 * @brief Tiny MMIO / barrier intrinsics shared by the boot files
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 1 / BSP] {World: S}
 *
 * @details
 * One documented home for the volatile MMIO accessors and CPU barriers
 * that the shared boot translation units (``system_init.c``,
 * ``trustzone_init.c``) both need, so the helpers are not duplicated and
 * documented per file. All are ``static inline`` -- each including TU
 * gets its own internal-linkage copy, so there is no shared state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @brief Volatile 32-bit MMIO read.
 * @details Single volatile load; the cast fixes the access width.
 * @param[in] addr Absolute MMIO address (must be 4-byte aligned).
 * @return The 32-bit word at @p addr.
 * @retval value Little-endian word read from @p addr.
 * @pre @p addr is a mapped, 4-byte-aligned MMIO address.
 * @pre The owning peripheral's bus clock is enabled.
 * @post No state is mutated.
 * @post The read has completed (volatile, not reordered).
 * @note Thread-safe (pure read).
 * @since 0.1.0
 */
static inline uint32_t ra8_boot_read32(uintptr_t addr)
{
  return *(volatile uint32_t*)addr;
}

/**
 * @brief Volatile 32-bit MMIO write.
 * @details Single volatile store of @p value to @p addr.
 * @param[in] addr  Absolute MMIO address (must be 4-byte aligned).
 * @param[in] value 32-bit word to store.
 * @pre @p addr is a mapped, 4-byte-aligned MMIO address.
 * @pre The owning peripheral's bus clock is enabled.
 * @post @p addr holds @p value.
 * @post The store has been issued (volatile).
 * @note Not thread-safe; callers serialise during single-threaded boot.
 * @since 0.1.0
 */
static inline void ra8_boot_write32(uintptr_t addr, uint32_t value)
{
  *(volatile uint32_t*)addr = value;
}

/**
 * @brief Data synchronisation barrier (no-op under ``RA8_OFF_TARGET``).
 * @details Completes all prior memory accesses before continuing.
 * @pre Running on the target core (or the host fake).
 * @pre Used after an MMIO write that must take effect before the next step.
 * @post All earlier explicit memory accesses have completed.
 * @post Subsequent instructions observe those effects.
 * @note ISR-safe.
 * @since 0.1.0
 */
static inline void ra8_boot_dsb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

/**
 * @brief Instruction synchronisation barrier (no-op under ``RA8_OFF_TARGET``).
 * @details Flushes the pipeline so later instructions see prior context changes.
 * @pre Running on the target core (or the host fake).
 * @pre A system-control register affecting fetch/exec was just written.
 * @post The pipeline is flushed; later instructions use the new context.
 * @post No data state is mutated.
 * @note ISR-safe.
 * @since 0.1.0
 */
static inline void ra8_boot_isb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("isb 0xF" ::: "memory");
#endif
}

/**
 * @brief Mask IRQs / set PRIMASK (no-op under ``RA8_OFF_TARGET``).
 * @details Disables maskable interrupt delivery for the critical boot window.
 * @pre Running on the target core (or the host fake).
 * @pre Called early in Reset before handlers are armed.
 * @post Maskable interrupts are disabled (PRIMASK=1).
 * @post No data state is mutated.
 * @note ISR-safe.
 * @since 0.1.0
 */
static inline void ra8_boot_disable_irq(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("cpsid i" ::: "memory");
#endif
}
