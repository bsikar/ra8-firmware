/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_background_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) autonomous counter image
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's secondary core, the
 * Cortex-M33. It is compiled as a wholly separate ELF (`-mcpu=cortex-m33`)
 * and embedded into the M85 ELF as a `.cpu1_image` blob; the M85 releases
 * this core at runtime via `ra8_cpu1_release` (HUM Ch 2.9.1 "CPU control
 * registers").
 *
 * Its job is intentionally tiny so the autonomous co-processor pattern is
 * what stands out:
 *
 *   1. Stamp ::k_bg_m33_signature into the shared block so the M85 can prove
 *      the M33 came out of reset and is executing user code.
 *   2. Increment `counter` in the shared block ::k_bg_target_count times.
 *      The M85 does nothing during this phase -- it merely polls `done`.
 *   3. Set `done = 1` to signal completion.
 *   4. Spin forever.
 *
 * @note The M33 deliberately does NOT call `ra8_log`. The ra8_emulator
 *       only echoes the primary core's ITM stream, so an M33 `ra8_log` line
 *       would be invisible in the fake. The M33's proof-of-life is the
 *       signature it writes and the counter it increments; the M85 reads and
 *       logs both on the M33's behalf, which is honest in both the emulator
 *       and on silicon.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "dualcore_background.h"
#include "ra8_attributes.h"

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
 * @brief CPU1 application: stamp signature, count autonomously, signal done.
 *
 * @details Writes the boot signature so the M85 can confirm the M33 left
 * reset, then increments the shared counter ::k_bg_target_count times
 * independently of the M85. Once counting is complete it sets `done = 1`
 * and spins. The M85 does nothing during the count except poll `done` --
 * the key teaching point of this example.
 *
 * @return This function never returns.
 * @note Control stays in the final spin loop.
 *
 * @pre `cpu1_reset_handler` has initialised `.data` and `.bss`.
 * @pre The shared block at ::k_bg_sram_base has been zeroed by the M85.
 * @post `m33_sig` holds ::k_bg_m33_signature.
 * @post `counter` equals ::k_bg_target_count and `done` equals 1.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  volatile dualcore_bg_t* bg = dualcore_bg();

  /* Announce we are alive. The M85 polls this field before waiting on done
   * so that a boot failure is distinguishable from a counting stall. */
  bg->m33_sig = (uint32_t)k_bg_m33_signature;
  __asm volatile("dsb" ::: "memory");

  /* Increment the shared counter k_bg_target_count times. Each store is
   * individually visible to the M85 once the write buffer drains, but we
   * only signal completion after all increments so the M85 reads a stable
   * final value. */
  RA8_LOOP_BOUND(k_bg_target_count);
  for (uint32_t i = 0U; i < (uint32_t)k_bg_target_count; i++) {
    bg->counter = bg->counter + 1U;
  }

  /* Drain the write buffer so the M85 sees the final `counter` value before
   * it sees the `done` flag flip. */
  __asm volatile("dsb" ::: "memory");
  bg->done = 1U;

  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then enter `cpu1_main`.
 *
 * @details The M33 boots with uninitialised RAM. Before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes
 * `.bss`. The linker exports the region bounds as `g_ra8_ls_cpu1_*` symbols.
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
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_data_end);
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_bss_end);
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_main();
}

/**
 * @brief CPU1 default fault handler: park the core.
 *
 * @details Every M33 exception slot routes here. The core stops making forward
 * progress; on hardware a watchdog (if enabled) eventually resets.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post The M33 makes no further forward progress.
 * @post `done` stays 0 (or at whatever value it held at fault time).
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
