/**
 * @file examples/ek_ra8d2/hw_pending/blink_m33_hal/cpu1_main.c
 * @brief CPU1 (Cortex-M33) image: blink LED1 through the HAL PCNTR primitive
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This is the HAL-based twin of `hw_validated/hil/blink_m33/cpu1_main.c`. It runs
 * on the RA8D2's secondary Cortex-M33, is compiled as a wholly separate
 * `-ffreestanding` ELF and embedded into the M85 ELF as a `.cpu1_image` blob, and
 * the M85 releases it via `ra8_cpu1_release` (HUM Ch 2.9.1) before sleeping.
 *
 * The ONE difference from the raw twin is the register access: instead of casting
 * `0x404000C0` to a `volatile uint32_t*` and hand-rolling the combined `{PODR,PDR}`
 * PCNTR1 store, it drives board LED1 (BLUE, P600 = PORT6 pin 0) through
 * `ra8_pcntr_set_output()` -- the CPU1-safe, header-only HAL primitive (issue
 * #580). No `ra8_hal` object is linked; only the freestanding-clean inline
 * accessor is included, so the image stays exactly as freestanding as before.
 * This app exists ALONGSIDE the raw `blink_m33` (which is kept as the deliberate
 * raw-MMIO reference), demonstrating that the primitive substitutes at the M33
 * call site.
 *
 * @note The M33 deliberately does NOT call `ra8_log`: the ra8_emulator echoes only
 *       the primary core's ITM, so an M33 log line would be invisible. The
 *       proof-of-life is the LED1 transition the M85 never drives.
 * @note LED1 powers up routed to PORT, so no PmnPFS / PWPR pin-function setup is
 *       needed; the primitive touches only PCNTR1 (direction + output level).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "blink_m33_hal.h"
#include "ra8_port_constants.h"

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
 * @enum m33_blink_const_t
 * @brief Blink-loop delay bound.
 * @details The pin masks and PCNTR field layout now live inside
 *          `ra8_pcntr_set_output()`; the CPU1 image only owns the visible cadence.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_blink_spins = 3000000U, /**< Bounded busy-delay spins per LED half-cycle. */
} m33_blink_const_t;

/**
 * @brief CPU1 application loop: blink LED1 (BLUE, P600) via the HAL primitive.
 *
 * @details Each pass calls `blink_m33_hal_step()` -- the shared one-iteration step
 * (`blink_m33_hal.h`) that toggles the level and drives LED1 to it through
 * `ra8_pcntr_set_output()` (the register access + HUM citation live in the
 * primitive). A bounded busy-delay paces the blink. The same step is exercised by
 * the host app-level test, so the tested logic is exactly this loop's. The M85
 * never writes this pin, so the transitions prove the M33 is running. See the
 * file header.
 *
 * @return This function never returns.
 * @note Control never leaves the blink loop.
 *
 * @pre `cpu1_reset_handler` has initialised `.data`/`.bss`.
 * @pre The IOPORT module clock is on (always-on after reset).
 * @post PORT6 pin 0 is configured as an output.
 * @post The LED1 level alternates on the configured cadence forever.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_main(void)
{
  ra8_level_t level = k_ra8_level_low;

  while (1) {
    for (volatile uint32_t d = 0U; d < (uint32_t)k_blink_spins; d++) {
      __asm volatile("nop");
    }
    /* One blink iteration: toggle + drive LED1 via the shared HAL step. The host
     * app-level test drives this exact function, so it is not a re-implementation. */
    level = blink_m33_hal_step(level);
  }
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then enter `cpu1_main`.
 *
 * @details Copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes
 * `.bss` before any C code runs; the linker exports the region bounds as
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
 * @details Every M33 exception slot routes here. The core stops making forward
 * progress; on hardware a watchdog (if enabled) eventually resets.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post The M33 makes no further forward progress.
 * @post The LED stops toggling, which is observable from the M85.
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
/* The vector table is only meaningful in the cross-compiled M33 image. The host
 * unit-test build compile-checks this TU but never links it as an executable, so
 * dropping the table there costs no coverage. */
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
