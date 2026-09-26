/**
 * @file arch.h
 * @brief The arch tier contract: what every CPU-architecture backend supplies.
 *
 * @details
 * `arch/` is the lowest tier in the platform structure described by #692. A new
 * instruction-set architecture is a new `arch/<isa>/` directory implementing
 * this one header; the ports tier above it means no logic library notices which
 * ISA it was built for.
 *
 * This header is the CONTRACT, not an implementation. It declares the symbols an
 * arch backend must define and documents the ones it may decline. There is no
 * backend in this tree yet: today the Armv8-M primitives are misfiled in Ring-1
 * `libs/ra8_core/` (`ra8_scb.h`, `ra8_systick.h`, `ra8_exception.h`,
 * `ra8_boot_entry.h`) and scattered across per-app boot files, which is why the
 * "Ring 1 is host==target" claim in `docs/RING_AND_WORLD.md` is not true yet.
 * Migrating those translation units is a later slice of #694; this slice fixes
 * the target they migrate to, so the moves that follow are mechanical rather
 * than a design argument per file.
 *
 * ## The four requirement classes
 *
 * Each declaration below sits in exactly one class, and the class decides what a
 * backend owes:
 *
 *   - **MUST** -- bare metal cannot boot without it. Every backend defines it.
 *   - **MUST, CPU-side interrupt controller** -- the CPU's own controller only.
 *     The RA8 two-level routing does not generalise: the SoC event router
 *     (`libs/ra8_hal/src/ra8_icu.c`, `libs/ra8_hal/src/ra8_elc.c`) is a SoC
 *     fact and stays in the HAL.
 *   - **MUST-if-RTOS** -- required only when an RTOS is linked. ThreadX's
 *     low-level init, already scoped per-core in the vendored tree, is this
 *     surface.
 *   - **OPTIONAL, capability-gated** -- implement it, or decline it in the
 *     core's `caps.h` with a reason. Silence is not a third option: the
 *     port-completeness gate (epic invariant #4) fails a capability flag that
 *     is set with no backend translation unit behind it, and fails a cleared
 *     flag with no documented decline.
 *
 * ## Where the capability flags live
 *
 * Capabilities are a property of the CPU CORE, not of the ISA, because this tree
 * already contains the divergence that proves it: the RA8D2 carries a Cortex-M85
 * with a data cache and a Cortex-M33 CPU1 without one, both Armv8-M. So the
 * flags are declared per core, in `arch/core/<core>/caps.h`, and `arch.h`
 * consumes whichever one the build selected.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "caps.h"

/**
 * @defgroup grp_arch arch tier
 * @brief CPU-architecture contract shared by every `arch/<isa>/` backend.
 * @{
 */

/**
 * @brief Lowest interrupt priority value the backend accepts.
 *
 * @details
 * Armv8-M encodes priority in the high bits of an 8-bit field, so the numeric
 * range a backend honours is a per-ISA fact rather than a portable constant. A
 * backend narrows this by clamping in ::arch_irq_set_priority; callers that need
 * the real width read ::arch_irq_priority_bits.
 */
#define K_ARCH_IRQ_PRIORITY_LOWEST (255U)
/**
 * @brief Highest (most urgent) interrupt priority value, on every ISA.
 */
#define K_ARCH_IRQ_PRIORITY_HIGHEST (0U)
/**
 * @enum arch_fault_kind_t
 * @brief Portable classification of a trap, for ::arch_fault_report.
 *
 * @details
 * The per-ISA fault taxonomies do not line up, so the contract carries the
 * coarse classes every architecture can produce and leaves the raw cause
 * registers in ::arch_fault_info_t::raw for an ISA-aware decoder. `ra8_exception`
 * already performs exactly this split for Armv8-M; the enum is that split made
 * portable.
 */
typedef enum arch_fault_kind_e : uint8_t {
    k_arch_fault_kind_unknown = 0U,   /**< Backend could not classify the trap. */
    k_arch_fault_kind_hard = 1U,      /**< Unrecoverable / escalated fault.     */
    k_arch_fault_kind_memory = 2U,    /**< Access violated a protection region. */
    k_arch_fault_kind_bus = 3U,       /**< Transaction rejected by the fabric.  */
    k_arch_fault_kind_usage = 4U,     /**< Undefined instruction, misalignment. */
    k_arch_fault_kind_secure = 5U,    /**< Security-state transition violation. */
    k_arch_fault_kind_debug = 6U,     /**< Debug-monitor entry.                 */
} arch_fault_kind_t;

/**
 * @struct arch_fault_info_t
 * @brief One trap, described portably plus its verbatim ISA cause words.
 *
 * @details
 * `raw` is deliberately opaque to portable code: a backend fills as many words
 * as its ISA has cause registers and sets `raw_count` to that number. Armv8-M
 * needs eight (CFSR, HFSR, DFSR, MMFAR, BFAR, AFSR, SFSR, SFAR), which is what
 * ::K_ARCH_FAULT_RAW_MAX is sized for.
 */
typedef struct arch_fault_info_s {
    arch_fault_kind_t kind;  /**< Portable class of the trap.                       */
    bool address_valid;      /**< True when `address` was captured, not inferred.   */
    uint32_t address;        /**< Faulting address when `address_valid`.            */
    uint32_t pc;             /**< Program counter of the faulting instruction.      */
    uint8_t raw_count;       /**< Number of populated entries in `raw`.             */
    uint32_t raw[8];         /**< Verbatim ISA cause registers, oldest field first. */
} arch_fault_info_t;

/**
 * @name MUST: C runtime and core bring-up
 *
 * The sequence is shared (`.data` copy, `.bss` zero, `__libc_init_array`); only
 * the body of each step is per-ISA. `libs/ra8_core/inc/ra8_boot_entry.h`, which
 * 278 first-party translation units reach today, is the Armv8-M shape of this.
 * @{
 */

/**
 * @brief Copy initialised data and zero `.bss`, then run static constructors.
 *
 * @details
 * Called from the reset vector before any C code with static storage duration
 * may be trusted. A backend implements the memory moves in whatever way its ISA
 * and linker-script symbols require, then calls `__libc_init_array()`.
 */
void arch_runtime_init(void);

/**
 * @brief Put the CPU core into the state the rest of the firmware assumes.
 *
 * @details
 * Vector-table base (VTOR on Armv8-M, `mtvec` on RISC-V), FPU enable, and the
 * interrupt priority model. Runs after ::arch_runtime_init and before any
 * driver. The peripheral IRQ COUNT is a SoC fact and is not decided here; the
 * arch supplies only the fixed core slots of the trap table.
 */
void arch_cpu_early_init(void);

/**
 * @brief Halt the core until the next interrupt.
 *
 * @details
 * `wfi` on Armv8-M, `wait` on RISC-V, a blocking sleep on a hosted backend. A
 * spurious early return is permitted, so callers treat this as a hint and keep
 * their own loop condition.
 */
void arch_cpu_idle(void);

/**
 * @brief Describe the trap currently being serviced.
 *
 * @param[out] info Filled with the portable classification and the raw cause
 *                  registers. Never partially written: on a backend that cannot
 *                  classify, `kind` is ::k_arch_fault_kind_unknown and
 *                  `raw_count` is zero.
 *
 * @details
 * Called from the backend's own fault entry point, which is one of the four
 * genuinely un-portable leaves and stays in assembly per ISA.
 */
void arch_fault_report(arch_fault_info_t *info);

/** @} */

/**
 * @name MUST: CPU-side interrupt controller
 *
 * NVIC on Armv8-M, CLIC/PLIC on RISC-V. The SoC event router that multiplexes
 * peripheral events onto these lines is NOT part of this contract.
 * @{
 */

/**
 * @brief Number of significant priority bits the core implements.
 *
 * @return A value in 1..8. Callers scale their own priority scheme by it rather
 *         than hardcoding a shift.
 */
uint8_t arch_irq_priority_bits(void);

/**
 * @brief Enable one interrupt line at the CPU-side controller.
 * @param[in] irq Line number, in the range the SoC declares.
 */
void arch_irq_enable(uint32_t irq);

/**
 * @brief Disable one interrupt line at the CPU-side controller.
 * @param[in] irq Line number, in the range the SoC declares.
 */
void arch_irq_disable(uint32_t irq);

/**
 * @brief Set the priority of one interrupt line.
 * @param[in] irq      Line number, in the range the SoC declares.
 * @param[in] priority ::K_ARCH_IRQ_PRIORITY_HIGHEST (most urgent) through
 *                     ::K_ARCH_IRQ_PRIORITY_LOWEST. A backend clamps a value
 *                     finer than ::arch_irq_priority_bits can express.
 */
void arch_irq_set_priority(uint32_t irq, uint32_t priority);

/**
 * @brief Raise one interrupt line in software.
 * @param[in] irq Line number, in the range the SoC declares.
 */
void arch_irq_set_pending(uint32_t irq);

/**
 * @brief Whether one interrupt line is currently being serviced.
 * @param[in] irq Line number, in the range the SoC declares.
 * @return True when the core is inside that handler.
 */
bool arch_irq_is_active(uint32_t irq);

/**
 * @brief Mask interrupts and return the previous mask state.
 *
 * @return An opaque token to hand back to ::arch_irq_unlock. Nesting is
 *         supported precisely because the previous state is returned rather
 *         than assumed.
 */
uint32_t arch_irq_lock(void);

/**
 * @brief Restore the interrupt mask captured by ::arch_irq_lock.
 * @param[in] key The token ::arch_irq_lock returned.
 */
void arch_irq_unlock(uint32_t key);

/** @} */

/**
 * @name MUST: barriers
 * @{
 */

/** @brief Data synchronisation barrier: prior accesses have completed. */
void arch_barrier_data_sync(void);

/** @brief Data memory barrier: prior accesses are ordered before later ones. */
void arch_barrier_mem(void);

/** @brief Instruction synchronisation barrier: flush the fetched pipeline. */
void arch_barrier_inst_sync(void);

/** @} */

/**
 * @name MUST-if-atomics-native, else an interrupt-locked C fallback
 *
 * A core without native atomics still supplies these; the backend implements
 * them over ::arch_irq_lock. ::ARCH_HAS_NATIVE_ATOMICS tells a caller whether
 * the operation is lock-free, which changes whether it is safe from a fault
 * handler, not whether it exists.
 * @{
 */

/**
 * @brief Compare and swap one 32-bit word.
 * @param[in,out] target   Word to update.
 * @param[in]     expected Value the caller believes `target` holds.
 * @param[in]     desired  Value to store when the comparison succeeds.
 * @return True when `desired` was stored.
 */
bool arch_atomic_cas(volatile uint32_t *target, uint32_t expected, uint32_t desired);

/**
 * @brief Add to one 32-bit word and return the value before the add.
 * @param[in,out] target Word to update.
 * @param[in]     delta  Amount to add, modulo 2^32.
 * @return The previous value of `*target`.
 */
uint32_t arch_atomic_add(volatile uint32_t *target, uint32_t delta);

/**
 * @brief Atomically read one 32-bit word.
 * @param[in] target Word to read.
 * @return The value read.
 */
uint32_t arch_atomic_load(const volatile uint32_t *target);

/**
 * @brief Atomically write one 32-bit word.
 * @param[out] target Word to write.
 * @param[in]  value  Value to store.
 */
void arch_atomic_store(volatile uint32_t *target, uint32_t value);

/** @} */

#if ARCH_HAS_RTOS_CONTEXT

/**
 * @name MUST-if-RTOS: the bring-your-own-RTOS surface
 *
 * Gated by ::ARCH_HAS_RTOS_CONTEXT so a bare-metal build of a core that has no
 * RTOS port does not owe these three symbols. Context switch and the fault entry
 * it shares are the first of the four un-portable leaves: per-ISA register
 * save/restore, written in assembly, and honestly labelled as such.
 * @{
 */

/**
 * @brief Build an initial stack frame for a new thread.
 * @param[in] stack_top Highest address of the thread stack, already aligned.
 * @param[in] entry     Function the thread begins executing.
 * @param[in] arg       Single argument handed to `entry`.
 * @return The stack pointer value the scheduler stores for the new thread.
 */
void *arch_context_init(void *stack_top, void (*entry)(void *), void *arg);

/**
 * @brief Switch from the current thread to another.
 * @param[out] save_sp Receives the outgoing thread's stack pointer.
 * @param[in]  load_sp Stack pointer of the incoming thread.
 */
void arch_context_switch(void **save_sp, void *load_sp);

/**
 * @brief Programme the core timer that drives the scheduler tick.
 * @param[in] ticks_per_second Requested tick rate.
 * @return The rate actually programmed, which may differ when the core timer
 *         cannot divide to the request exactly.
 */
uint32_t arch_tick_configure(uint32_t ticks_per_second);

/** @} */

#endif /* ARCH_HAS_RTOS_CONTEXT */

#if ARCH_HAS_MEM_PROTECT

/**
 * @name OPTIONAL: memory protection
 *
 * Declared when the core's `caps.h` sets ::ARCH_HAS_MEM_PROTECT. The flavour
 * (pmsav7, pmsav8, mmu) is named by ::ARCH_MEM_PROTECT_FLAVOUR so a caller can
 * reason about granularity without knowing the ISA.
 * @{
 */

/**
 * @brief Apply one protection region.
 * @param[in] index  Region slot, below ::ARCH_MEM_PROTECT_REGIONS.
 * @param[in] base   Region base address, aligned as the flavour requires.
 * @param[in] size   Region size in bytes.
 * @param[in] attrs  Backend-defined attribute word.
 * @return True when the region was programmed.
 */
bool arch_mem_protect_region_set(uint8_t index, uintptr_t base, size_t size, uint32_t attrs);

/** @brief Enable memory protection with the regions programmed so far. */
void arch_mem_protect_enable(void);

/** @brief Disable memory protection. */
void arch_mem_protect_disable(void);

/** @} */

#endif /* ARCH_HAS_MEM_PROTECT */

#if ARCH_HAS_CACHE

/**
 * @name OPTIONAL: data and instruction cache
 *
 * The second un-portable leaf: the maintenance SEQUENCES are per-core, not just
 * per-ISA. Cortex-M33 CPU1 on this very SoC has no data cache while the M85 CPU0
 * does, which is the concrete reason the flag sits in the core's `caps.h`
 * instead of the ISA's.
 * @{
 */

/**
 * @brief Write dirty data-cache lines covering a range back to memory.
 * @param[in] base Start of the range.
 * @param[in] size Length of the range in bytes.
 */
void arch_cache_clean(uintptr_t base, size_t size);

/**
 * @brief Discard data-cache lines covering a range without writing them back.
 * @param[in] base Start of the range.
 * @param[in] size Length of the range in bytes.
 */
void arch_cache_invalidate(uintptr_t base, size_t size);

/** @} */

#endif /* ARCH_HAS_CACHE */

#if ARCH_HAS_TRUSTZONE_M

/**
 * @name OPTIONAL: TrustZone-M
 *
 * The third un-portable leaf, and the only one with no cross-ISA analogue at
 * all. Armv8-M only. The `{World: S/NS/NSC}` tags described in
 * `docs/RING_AND_WORLD.md` become arch-conditional metadata: meaningful when
 * this flag is set, inert on a backend without it.
 * @{
 */

/**
 * @brief Programme one security-attribution region.
 * @param[in] index Region slot, below ::ARCH_TRUSTZONE_REGIONS.
 * @param[in] base  Region base address.
 * @param[in] limit Last address in the region, inclusive.
 * @param[in] nsc   True to mark the region Non-Secure-Callable.
 * @return True when the region was programmed.
 */
bool arch_trustzone_region_set(uint8_t index, uintptr_t base, uintptr_t limit, bool nsc);

/** @brief Enable security attribution with the regions programmed so far. */
void arch_trustzone_enable(void);

/** @} */

#endif /* ARCH_HAS_TRUSTZONE_M */

/** @} */

#ifdef __cplusplus
}
#endif
