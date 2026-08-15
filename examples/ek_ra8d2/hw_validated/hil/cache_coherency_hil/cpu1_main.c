/**
 * @file examples/ek_ra8d2/hw_validated/hil/cache_coherency_hil/cpu1_main.c
 * @brief CPU1 (Cortex-M33) cache-coherency responder
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details Built as a separate ELF (-mcpu=cortex-m33) and embedded into
 *          the M85 image by ``ra8_add_cpu1_image()``. Polls the shared
 *          SRAM struct at ``0x22100000`` for a ``ping_seq`` advance; on
 *          each new ping it echoes the transformed payload
 *          (``ping_payload`` + ::k_cache_coherency_delta) and acks via
 *          ``pong_seq``. The Cortex-M33 has no data cache, so it always
 *          sees SRAM directly; the coherency burden is one-sided (the
 *          M85 D-cache), which the non-cacheable MPU region on the M85
 *          side removes. No IPC peripheral is used -- see
 *          ``cache_coherency_shared.h`` for the rationale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "cache_coherency_shared.h"

extern uint32_t g_ra8_ls_cpu1_stack_top;
extern uint32_t g_ra8_ls_cpu1_data_start;
extern uint32_t g_ra8_ls_cpu1_data_end;
extern uint32_t g_ra8_ls_cpu1_data_load;
extern uint32_t g_ra8_ls_cpu1_bss_start;
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @brief CPU1 responder loop.
 * @details See file header. Each ping is echoed as ``ping_payload + delta``
 *          so the M85 can verify the round-trip carried data end-to-end.
 * @pre Vector table installed.
 * @pre Shared block reachable at ::k_cache_coherency_shared_addr.
 * @post Loop never exits.
 * @post ``pong_seq`` tracks ``ping_seq`` after each reply.
 * @note Single-threaded entry.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_cpu1_main(void)
{
  volatile cache_coherency_shared_t* shared        = internal_shared();
  uint32_t                           last_observed = 0U;

  while (1) {
    /* Spin until CPU0 bumps the ping sequence. The reads are volatile,
     * so the compiler emits a fresh load each iteration. */
    while (shared->ping_seq == last_observed) {
      __asm volatile("nop");
    }
    last_observed = shared->ping_seq;

    /* Read the payload after the sequence advance, transform it by the
     * fixed delta, and ack. The cacheless M33 sees SRAM directly; the
     * M85 side keeps coherency via its non-cacheable MPU region. */
    uint32_t got         = shared->ping_payload;
    shared->pong_payload = got + (uint32_t)k_cache_coherency_delta;
    __asm volatile("dsb" ::: "memory");
    shared->pong_seq = last_observed;
  }
}

/**
 * @brief CPU1 reset handler.
 *
 * @details Runs the minimal C-runtime init the M33 image needs before
 * branching into ``cpu1_main``:
 *   1. Copy ``.data`` from its MRAM_CPU1 load image into SRAM_CPU1.
 *   2. Zero ``.bss`` in SRAM_CPU1.
 *
 * @pre Initial SP loaded by hardware from the first slot of
 *      ``.cpu1_vectors`` (= ``g_ra8_ls_cpu1_stack_top``).
 * @pre CPU1 has just exited reset via the ACTREQ handshake driven by
 *      ``ra8_cpu1_release`` on CPU0.
 * @post ``.data`` mirrors the MRAM_CPU1 load image.
 * @post ``.bss`` is zero-filled.
 * @post Never returns; ``cpu1_main`` enters its responder loop.
 *
 * @note Called only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  /* Copy .data from MRAM_CPU1 load address into SRAM_CPU1. */
  uint32_t* dst = &g_ra8_ls_cpu1_data_start;
  uint32_t* src = &g_ra8_ls_cpu1_data_load;
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }
  /* Zero .bss in SRAM_CPU1. */
  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }
  internal_cpu1_main();
}

/**
 * @brief Default fault handler.
 * @details All M33 exception slots route here.
 * @pre Hardware fault occurred.
 * @pre Caller is the M33 exception entry path.
 * @post CPU1 stops making forward progress.
 * @post Watchdog (if enabled) eventually resets the chip.
 * @note Used as default for all exception slots.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_fault_handler(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @var g_cpu1_vector_table
 * @brief Minimal Armv8-M vector table for CPU1.
 * @details Initial-SP slot overridden by SYSC.MSPC1 at release.
 * @note Placed in .cpu1_vectors by the linker.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
#ifndef RA8_OFF_TARGET
/* Vector table only built for the cross-compiled M33 image. The host
 * build does not link this TU as an executable -- it is compile-checked
 * only -- so we can drop the table without losing test coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&internal_fault_handler,
  (uintptr_t)&internal_fault_handler,
  (uintptr_t)&internal_fault_handler,
  (uintptr_t)&internal_fault_handler,
  (uintptr_t)&internal_fault_handler,
  (uintptr_t)&internal_fault_handler,
};
#endif
