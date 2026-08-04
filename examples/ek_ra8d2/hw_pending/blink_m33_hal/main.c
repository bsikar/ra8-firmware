/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_pending/blink_m33_hal/main.c
 * @brief CPU0 (Cortex-M85) launcher for the HAL-based M33 blink
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * The primary-core launcher for `blink_m33_hal`, the HAL-based twin of the raw
 * `blink_m33` template. The M85 does the minimum -- bring up logging and the
 * clock tree, release the secondary Cortex-M33 with `ra8_cpu1_release`
 * (HUM Ch 2.9.1 "CPU control registers"), then idle in WFI -- and the M33 owns
 * the work. Here that work is blinking LED1 through the CPU1-safe HAL primitive
 * `ra8_pcntr_set_output()` (issue #580) rather than a raw PCNTR poke.
 *
 * This is `hw_pending`: the raw `blink_m33` is the HIL-validated reference; this
 * HAL variant is behaviourally identical (same LED1 blink, same PCNTR1 effect)
 * but this exact ELF has not had its own bench run, so it is not promoted.
 *
 * @note `ra8_log_info` compiles to a no-op unless the build sets a log level of
 *       INFO or finer (a Debug build). `make emu-blink_m33_hal` builds Debug so
 *       the `[itm]` lines appear.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra8_ls_cpu1_stack_top;

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @details Reached only if `ra8_cpu1_release` fails, so the M33 never started.
 * The core spins with the failure already logged.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A fatal error (CPU1 release failure) has occurred.
 * @pre The failure has already been logged.
 * @post The M85 makes no further forward progress.
 * @post The M33 stays held in reset.
 *
 * @note Mirrors the M33's fault handler for symmetry.
 * @since 0.1.0
 */
[[noreturn]] static void park_forever(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief Idle the M85 forever once the M33 owns the work (low-power posture).
 *
 * @details After releasing the M33 the M85 has nothing to do, so it sleeps in a
 * WFI loop -- the heavy core asleep while the lean M33 runs is the low-power
 * co-processor posture this template demonstrates.
 *
 * @return This function never returns.
 * @note The core stays asleep between interrupts.
 *
 * @pre The M33 has been released successfully.
 * @pre No further M85-side work is pending.
 * @post The M85 consumes minimal power between wake events.
 * @post The M33 continues blinking independently.
 *
 * @note WFI wakes on any pending interrupt; this template enables none, so the
 *       M85 simply stays asleep.
 * @since 0.1.0
 */
[[noreturn]] static void idle_forever(void)
{
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief CPU0 (Cortex-M85) application entry.
 *
 * @details Brings up logging and the clock tree, releases the Cortex-M33 (which
 * then blinks LED1 via `ra8_pcntr_set_output()`), and idles. See the file header.
 *
 * @return Never returns (ends in ::idle_forever).
 * @retval (none) Control stays in the idle loop.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held in reset by hardware until released here.
 * @post The M33 has been released and is blinking LED1.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this template.
 * @since 0.1.0
 */
int main(void)
{
  /* Publish the clock tree the released M33 shares BEFORE logging comes up --
   * the console derives its baud from PCLKA, so bringing the clock up second
   * would fix the baud against the reset clocks. Best-effort: a failure only
   * changes the busy-loop cadence, not the correctness of the blink, so the
   * verdict is carried forward and narrated once logging is available. */
  const ra8_err_t clk_err = ra8_cgc_init();

  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 dual-core blink (HAL PCNTR primitive) ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");
  if (clk_err != k_ra8_ok) {
    ra8_log_info("M85", "clock init failed -- continuing at reset clocks");
  }

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  ra8_log_info("M85", "M33 released -- it now blinks LED1 via ra8_pcntr_set_output()");
  ra8_log_info("M85", "M85 entering WFI idle -- low-power co-processor model");
  idle_forever();
}
