/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_mailbox/src/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) responder image
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33.
 * It is compiled as a wholly separate ELF (`-mcpu=cortex-m33`) and embedded
 * into the M85 ELF as a `.cpu1_image` blob; the M85 releases this core at
 * runtime via `ra8_cpu1_release` (HUM Ch 2.9.1 "CPU control registers").
 *
 * Its job is intentionally tiny so the dual-core plumbing is what stands out:
 *   1. On boot it stamps ::k_dualcore_m33_signature into the shared mailbox so
 *      the M85 can prove the M33 came out of reset.
 *   2. Then it loops: wait for the M85 to bump `request_seq`, read `request`,
 *      write back `request * 3 + 1`, bump its liveness counter, and ack by
 *      setting `reply_seq`.
 *
 * @note The M33 deliberately does NOT call `ra8_log`. On hardware each core has
 *       its own CoreSight ITM, and the ra8_emulator only echoes the
 *       primary core's ITM stream, so an M33 `ra8_log` line would be invisible
 *       in the fake. The M33's proof-of-life is therefore the value it
 *       writes into the mailbox (the M85 reads it and logs it on the M33's
 *       behalf), which is honest in both the emulator and on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "dualcore_mailbox.h"
#include "ra8_err.h"

/** @brief CPU1 stack top (slot 0 of the M33 vector table). */
extern uint32_t g_ra8_ls_cpu1_stack_top;
/** @brief CPU1 `.data` run-region start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_start;
/** @brief CPU1 `.data` run-region end. */
extern uint32_t g_ra8_ls_cpu1_data_end;
/** @brief CPU1 `.data` load image (in MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_load;
/** @brief CPU1 `.bss` start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_bss_start;
/** @brief CPU1 `.bss` end. */
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @brief CPU1 application loop: answer the M85's mailbox requests.
 *
 * @details Stamps the boot signature, then services request/reply rounds
 * forever. Each `request` is transformed by `* k_dualcore_m33_mul +
 * k_dualcore_m33_add` so a correct `reply` proves the M33 executed code
 * rather than the M85 reading back its own write. See the file header.
 *
 * @return This function never returns.
 * @note Control never leaves the service loop.
 *
 * @pre `cpu1_reset_handler` has initialised `.data`/`.bss`.
 * @pre The shared mailbox at ::k_dualcore_mailbox_addr is reachable.
 * @post ::k_dualcore_m33_signature has been written to the mailbox.
 * @post The loop never exits.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  volatile dualcore_mailbox_t* mb          = dualcore_mailbox();
  uint32_t                     last_seen   = 0U;
  uint32_t                     reply_count = 0U;

  /* Announce we are alive before servicing anything: the M85 polls this
   * field to confirm the M33 left reset and is executing user code. */
  mb->m33_signature = (uint32_t)k_dualcore_m33_signature;
  __asm volatile("dsb" ::: "memory");

  while (1) {
    /* Spin until the M85 advances the request sequence. The volatile read
     * forces a fresh load each iteration. Bound: the M85 drives this. */
    while (mb->request_seq == last_seen) {
      __asm volatile("nop");
    }
    last_seen = mb->request_seq;

    /* Transform the operand. Reading `request` after observing the seq bump
     * guarantees we see the payload the M85 wrote before bumping the seq. */
    const uint32_t operand = mb->request;
    const uint32_t answer = (operand * (uint32_t)k_dualcore_m33_mul) + (uint32_t)k_dualcore_m33_add;

    mb->reply = answer;
    reply_count++;
    mb->m33_replies = reply_count;
    /* Drain the write buffer so the M85 sees `reply` + `m33_replies` before
     * it sees the `reply_seq` ack on the next store. */
    __asm volatile("dsb" ::: "memory");
    mb->reply_seq = last_seen;
  }
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then enter `cpu1_main`.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs
 * this copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes
 * `.bss`. Skipping this leaves file-scope globals holding whatever pattern
 * SRAM held at power-on. The linker exports the region bounds as
 * `g_ra8_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @note Control passes to `cpu1_main`, which loops forever.
 *
 * @pre Hardware loaded the initial SP from `.cpu1_vectors[0]`.
 * @pre The M85 released this core via the CPU1ACTCSR handshake.
 * @post `.data` mirrors its MRAM_CPU1 load image.
 * @post `.bss` is zero-filled.
 *
 * @note Entered only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  uint32_t* dst = &g_ra8_ls_cpu1_data_start;
  uint32_t* src = &g_ra8_ls_cpu1_data_load;
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_main();
}

/**
 * @brief CPU1 default fault handler: park the core.
 *
 * @details Every M33 exception slot routes here. The core stops making
 * forward progress; on hardware a watchdog (if enabled) eventually resets.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post The M33 makes no further forward progress.
 * @post `m33_replies` stops advancing, which the M85 can observe.
 *
 * @note Shared default for all CPU1 exception vectors.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fault_handler(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @var g_cpu1_vector_table
 * @brief Minimal Armv8-M (baseline) vector table for the M33 image.
 * @details Slot 0 is the initial SP, slot 1 the reset handler; the remaining
 *          core-exception slots share the fault handler. The M85 points
 *          CPU1INITVTOR at this table when it releases the core.
 * @note Placed in the `.cpu1_vectors` section by `linker_script_cpu1.ld`.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
#ifndef RA8_OFF_TARGET
/* The vector table is only meaningful in the cross-compiled M33 image. The
 * host unit-test build compile-checks this TU but never links it as an
 * executable, so dropping the table there costs no coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
