/**
 * @file ra8_hw_intrinsics.h
 * @brief Shared CPU-intrinsic seam for the HAL drivers (host/target split)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * One documented home for the bare Armv8-M CPU primitives that several HAL
 * peripheral drivers need but that a host unit-test build cannot execute:
 * ``wfi``, the ``dsb`` / ``isb`` barriers, the ``nop`` used to pad calibrated
 * busy-waits, the ``cpsie i`` / ``cpsid i`` global-interrupt gate, and the
 * post-reset spin that never returns on silicon.
 *
 * This header is the single divergence point (issue #293). On the
 * arm-none-eabi target each primitive is a ``static inline`` that expands to
 * exactly the inline asm the drivers used before -- so the shipping code is
 * byte-for-byte unchanged and, critically, the calibrated ``nop`` pads stay
 * inline (no per-iteration call overhead skews the delay). On the host build
 * (``RA8_OFF_TARGET`` defined) this header declares the primitives
 * ``extern`` and their host-safe bodies (wfi -> return, dsb/isb -> compiler
 * barrier, nop -> no-op, irq gate -> no-op, reset spin -> return) live in ONE
 * translation unit, ``tests/mocks/ra8_host_asm_stub.c``. The test harness
 * therefore owns the whole host model and the driver ``.c`` files carry no
 * ``#ifdef RA8_OFF_TARGET`` of their own; the gate
 * ``scripts/checks/check_no_driver_asm_guard.py`` keeps it that way.
 *
 * @note The barriers are Arm architectural instructions (Armv8-M ARM), not
 *       RA8D2 peripheral registers, so no Hardware User's Manual citation
 *       applies to them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifndef RA8_OFF_TARGET

/**
 * @brief Wait For Interrupt: place the core in low-power wait.
 *
 * @details
 * Issues the Armv8-M ``wfi`` instruction with a memory clobber so no prior
 * memory access is reordered past the sleep. On the host this returns
 * immediately (the stub cannot halt an x86 test binary).
 *
 * @pre Sleep-mode configuration registers have already been written.
 * @pre Called with the intended wake source armed.
 * @post The core has slept until a wake event (target) or returned at once
 *       (host).
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_wfi(void)
{
  __asm__ volatile("wfi" ::: "memory");
}

/**
 * @brief Data Synchronisation Barrier (full system).
 *
 * @details
 * Issues ``dsb 0xF`` (SY), stalling the core until every prior explicit
 * memory access -- including the MMIO writes that precede it -- has
 * completed. On the host this collapses to a compiler barrier because
 * ordinary host RAM needs no hardware ordering.
 *
 * @pre Placed after an MMIO write that must retire before the next step.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post All earlier explicit memory accesses have completed.
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_dsb(void)
{
  __asm__ volatile("dsb 0xF" ::: "memory");
}

/**
 * @brief Instruction Synchronisation Barrier (full system).
 *
 * @details
 * Issues ``isb 0xF``, flushing the pipeline so instructions fetched after it
 * observe context changes established by a preceding enable/invalidate
 * sequence. On the host this collapses to a compiler barrier because there is
 * no pipeline to flush.
 *
 * @pre Placed after a system-control write that affects fetch or execution.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post The pipeline is flushed; later instructions see the new context.
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_isb(void)
{
  __asm__ volatile("isb 0xF" ::: "memory");
}

/**
 * @brief No-operation used to pad a calibrated busy-wait.
 *
 * @details
 * Emits a single ``nop`` so the compiler cannot fold a calibrated delay loop
 * away on the target, and stays inline so the delay calibration holds. On the
 * host it is a no-op call: the loop still runs but each iteration costs
 * nothing, and there is no timing requirement to meet.
 *
 * @pre Called from the body of a statically-bounded delay loop.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post One nominal instruction slot has elapsed (target) or nothing (host).
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_nop(void)
{
  __asm__ volatile("nop");
}

/**
 * @brief Enable maskable interrupts (clear PRIMASK).
 *
 * @details
 * Issues ``cpsie i`` so maskable IRQs may dispatch again. On the host this is
 * a no-op because the test harness dispatches interrupts through the
 * ``ra8_fake_irq`` seam rather than the core's PRIMASK.
 *
 * @pre A prior ::ra8_hw_irq_disable opened the critical section.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post Maskable interrupt delivery is enabled (PRIMASK = 0).
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_irq_enable(void)
{
  __asm__ volatile("cpsie i" ::: "memory");
}

/**
 * @brief Disable maskable interrupts (set PRIMASK).
 *
 * @details
 * Issues ``cpsid i`` so subsequent maskable IRQs pend until re-enabled. On
 * the host this is a no-op for the same reason as ::ra8_hw_irq_enable.
 *
 * @pre Opening a critical section that must not be pre-empted.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post Maskable interrupt delivery is disabled (PRIMASK = 1).
 * @post No general-purpose register is modified.
 * @note ISR-safe; touches no driver state.
 * @since 0.1.0
 */
static inline void ra8_hw_irq_disable(void)
{
  __asm__ volatile("cpsid i" ::: "memory");
}

/**
 * @brief Barrier-then-spin used after requesting a system reset.
 *
 * @details
 * On silicon a ``dsb`` guarantees the reset-request store retires, then the
 * core spins forever until the reset actually lands -- the function never
 * returns and control must not fall through into the caller's tail. On the
 * host it returns immediately so a unit test can inspect the register write
 * it just staged.
 *
 * @pre The reset-request register (e.g. AIRCR.SYSRESETREQ) was just written.
 * @pre Running on the Cortex-M85 target or the host stub.
 * @post The pending reset store has retired (target) before the core spins.
 * @post Never returns on target; returns at once on the host.
 * @note ISR context is irrelevant: the core is on its way down.
 * @since 0.1.0
 */
static inline void ra8_hw_wait_for_reset(void)
{
  __asm__ volatile("dsb 0xF" ::: "memory");
  for (;;) {
    /* GCOVR_EXCL_LINE -- never reached on target; host uses the stub. */
  }
}

#else /* RA8_OFF_TARGET -- host bodies live in the stub TU */

/* Host build: ra8_hw_wfi is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_wfi(void);
/* Host build: ra8_hw_dsb is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_dsb(void);
/* Host build: ra8_hw_isb is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_isb(void);
/* Host build: ra8_hw_nop is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_nop(void);
/* Host build: ra8_hw_irq_enable is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_irq_enable(void);
/* Host build: ra8_hw_irq_disable is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_irq_disable(void);
/* Host build: ra8_hw_wait_for_reset is defined in tests/mocks/ra8_host_asm_stub.c;
   see implementation for details. */
void ra8_hw_wait_for_reset(void);

#endif /* RA8_OFF_TARGET */
