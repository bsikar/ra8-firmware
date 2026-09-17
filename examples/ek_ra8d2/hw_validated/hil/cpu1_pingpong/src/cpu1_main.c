/**
 * @file examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/src/cpu1_main.c
 * @brief CPU1 (Cortex-M33) ping-pong responder
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details Built as a separate ELF (-mcpu=cortex-m33). Polls the
 *          shared SRAM struct at 0x22100000 for a ping_seq advance;
 *          on each new ping writes the pong payload and acks via
 *          pong_seq. No IPC peripheral is used -- see
 *          ``shared_pingpong.h`` for the rationale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "shared_pingpong.h"

extern uint32_t g_ra8_ls_cpu1_stack_top;
extern uint32_t g_ra8_ls_cpu1_data_start;
extern uint32_t g_ra8_ls_cpu1_data_end;
extern uint32_t g_ra8_ls_cpu1_data_load;
extern uint32_t g_ra8_ls_cpu1_bss_start;
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @brief CPU1 application loop.
 * @details See file header.
 * @pre Vector table installed.
 * @pre The fixed shared-SRAM message block is reachable.
 * @post Loop never exits.
 * @post Each new ping sequence is acknowledged through ``pong_seq``.
 * @note Single-threaded entry.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_cpu1_main(void)
{
  volatile cpu1_pingpong_shared_t* shared        = internal_shared();
  uint32_t                         last_observed = 0U;

  while (1) {
    /* Spin until CPU0 bumps the ping sequence. The reads are volatile,
     * so the compiler emits a fresh load each iteration. */
    while (shared->ping_seq == last_observed) {
      __asm volatile("nop");
    }
    last_observed = shared->ping_seq;

    /* Read the payload after the sequence advance so we know CPU0's
     * write of the payload preceded its sequence bump. */
    uint32_t got = shared->ping_payload;
    if (got != (uint32_t)k_cpu1_pingpong_magic_ping) {
      /* Wrong magic -- still ack so CPU0's poll doesn't block. CPU0
       * will count the mismatch via the payload check on its side. */
      shared->pong_payload = 0U;
    } else {
      shared->pong_payload = (uint32_t)k_cpu1_pingpong_magic_pong;
    }
    __asm volatile("dsb" ::: "memory");
    shared->pong_seq = last_observed;
  }
}

/**
 * @brief CPU1 reset handler.
 *
 * @details Runs the minimal C-runtime init the M33 image needs before
 * branching into ``internal_cpu1_main``:
 *   1. Copy ``.data`` from its MRAM_CPU1 load image into SRAM_CPU1.
 *   2. Zero ``.bss`` in SRAM_CPU1.
 *
 * Without these passes the CPU1 image's globals (e.g. the
 * ``s_ipc_channels`` array in ``ra8_ipc.c`` -- ``.bss`` -- and any
 * initialised file-scope statics -- ``.data``) hold whatever pattern
 * SRAM_CPU1 contained at boot. That was the reason CPU1 silently
 * stayed wedged after Agent D embedded the CPU1 binary: the M33
 * jumped straight into ``internal_cpu1_main`` with uninitialised globals, so
 * ``ra8_ipc_init`` / ``ra8_ipc_recv_message`` operated on garbage state
 * structures and the channel pair never came alive.
 *
 * @pre Initial SP loaded by hardware from the first slot of
 *      ``.cpu1_vectors`` (= ``g_ra8_ls_cpu1_stack_top``).
 * @pre CPU1 has just exited reset via the ACTREQ handshake driven by
 *      ``ra8_cpu1_release`` on CPU0.
 * @post ``.data`` mirrors the MRAM_CPU1 load image.
 * @post ``.bss`` is zero-filled.
 * @post Never returns; ``internal_cpu1_main`` enters its infinite shared-SRAM loop.
 *
 * @note Called only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  /* Copy .data from MRAM_CPU1 load address into SRAM_CPU1. The linker
   * defines ``g_ra8_ls_cpu1_data_load`` as LOADADDR(.data) so this
   * works regardless of the absolute MRAM_CPU1 base. */
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
