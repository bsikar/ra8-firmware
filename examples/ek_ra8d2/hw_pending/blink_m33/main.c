/**
 * @file examples/ek_ra8d2/hw_pending/blink_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) launcher for the M33 blink template
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It is the canonical "target the M33" template (issue #152): the
 * M85 does the minimum -- release the secondary Cortex-M33 and then sleep -- and
 * the M33 owns the work. Here the work is blinking LED1, but any portable app
 * drops into `cpu1_main.c` the same way.
 *
 *   1. Two images, one per core. This M85 ELF carries an embedded Cortex-M33
 *      image (`.cpu1_image`, built from `cpu1_main.c` by the shared
 *      `ra_add_cpu1_image()` recipe). One `.hex` flashes both cores.
 *
 *   2. The co-processor / low-power model. The M85 releases the M33 with
 *      `ra_cpu1_release` (HUM Ch 2.9.1 "CPU control registers"), then drops into
 *      a WFI idle loop. With the heavy M85 asleep and the lean M33 doing the
 *      work, this *is* the low-power posture issue #150 builds on.
 *
 * Everything the M85 does is logged with `ra_log`, which the emulator echoes as
 * `[itm]` lines (the J-Link SWO trace analog). The M33's proof-of-life is the
 * blinking LED the M85 never drives. See `README.md`.
 *
 * @note `ra_log_info` is compiled to a no-op unless the build defines a log
 *       level of INFO or finer (a Debug build). `make sim-blink_m33` builds
 *       Debug so the `[itm]` lines appear.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_dual_core.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra_ls_cpu1_stack_top;

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @details Reached only if `ra_cpu1_release` fails, so the M33 never started.
 * The core spins with the failure already logged.
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
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
 * WFI loop. Modelling the heavy core asleep while the lean M33 runs is the
 * low-power co-processor posture this template exists to demonstrate.
 *
 * @return This function never returns.
 * @retval (none) The core stays asleep between interrupts.
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
 * @details Brings up logging, releases the Cortex-M33 (which then blinks LED1),
 * and idles. See the file header for the teaching narrative.
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
  ra_log_init();
  ra_log_info("M85", "==== RA8D2 dual-core blink template ====");
  ra_log_info("M85", "Cortex-M85 primary core online");
  ra_log_info("M85", "releasing Cortex-M33 secondary core ...");

  const ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  ra_log_info_val("M85", "ra_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra_ok) {
    ra_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  ra_log_info("M85", "M33 released -- it now blinks LED1 (BLUE, P600)");
  ra_log_info("M85", "M85 entering WFI idle -- low-power co-processor model");
  idle_forever();
}
