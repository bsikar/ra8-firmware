/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/reset_cause_demo/main.c
 * @brief HIL gate for the ra8_reset driver -- software-reset round-trip
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the ra8_reset HAL end-to-end on the bench:
 *
 *   1. POR / cold boot lands here with ``RSTSR1.SWRF == 0`` and the
 *      decoded cause is ``k_ra8_reset_cause_power_on`` (or some other
 *      pre-reset cause, depending on how the chip woke up).
 *   2. The app waits briefly, then calls ``ra8_reset_software_reset()``
 *      which writes ``AIRCR.SYSRESETREQ`` and the chip reboots.
 *   3. The post-reset boot sees ``cause == k_ra8_reset_cause_software``;
 *      the app drops into a tight loop that increments
 *      ``g_reset_cause_loop`` at ~20 Hz.
 *
 * The HIL gate uses ``jlink_memprobe`` mode (see ``hil.conf``) to
 * sample ``g_reset_cause_loop`` twice over a 5 s window and assert it
 * advanced by at least 3. The counter only advances on the second
 * (software-cause) boot, so a passing probe proves:
 *
 *   - ``ra8_reset_init`` / ``ra8_reset_get_cause`` decode the SWRF flag.
 *   - ``ra8_reset_software_reset`` actually triggers ``SYSRESETREQ``.
 *   - The Reset_Handler / SystemInit / main path is alive after a SW reset.
 *
 * The counter and the captured initial cause are both ``volatile``
 * non-static globals so the linker keeps them visible to J-Link.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-01
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_reset.h"
#include "ra8_time.h"

/**
 * @enum reset_cause_demo_const_t
 * @brief Internal constants for the reset-cause demo.
 */
typedef enum : uint32_t {
  k_reset_cause_cpu_hz_at_reset = 8400000U, /**< MOCO ~8.4 MHz before CGC bring-up. */
  k_reset_cause_settle_ms       = 200U,     /**< Pause before triggering SW reset.  */
  k_reset_cause_loop_period_ms  = 50U,      /**< 20 Hz advance for HIL probe.       */
} reset_cause_demo_const_t;

/**
 * @var g_reset_cause_loop
 * @brief HIL-probed counter -- advances only after the second
 *        (software-cause) boot.
 *
 * @details
 * Read externally by ``hil_jlink_memprobe.sh`` to assert that the
 * software-reset path completed and the post-reset boot reached the
 * loop in main(). ``volatile`` keeps the increment out of the
 * optimiser's hands; non-static keeps the symbol in nm.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_reset_cause_loop = 0U;

/**
 * @var g_reset_cause_initial
 * @brief Decoded reset cause observed on this boot, captured as soon as
 *        ``ra8_reset_init`` returns.
 *
 * @details
 * Cold boot writes the POR value (or whatever cause the chip woke up
 * with); after the SW reset path, this reads ``k_ra8_reset_cause_software``.
 * Useful for debugging a failing HIL probe over J-Link.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_reset_cause_initial = 0U;

/**
 * @brief Park the CPU forever in WFI -- used after the SW reset write
 *        or if HAL init fails.
 *
 * @details Compiled into a tight ``WFI`` loop so the chip is idle but
 *          observable from J-Link.
 *
 * @pre Caller has finished any work that must happen before park.
 * @pre IRQs are either disabled or harmless.
 * @post CPU spins in WFI; ``g_reset_cause_loop`` is no longer advancing.
 * @post No further C-level state changes.
 *
 * @note Not thread-safe (single CPU).
 * @since 0.1.0
 */
static void reset_cause_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra8_time_init(k_reset_cause_cpu_hz_at_reset) != k_ra8_ok) {
    reset_cause_panic_halt();
  }
  ra8_isr_globals_enable();

  if (ra8_reset_init() != k_ra8_ok) {
    reset_cause_panic_halt();
  }

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  if (ra8_reset_get_cause(&cause) != k_ra8_ok) {
    reset_cause_panic_halt();
  }
  g_reset_cause_initial = (uint32_t)cause;
  ra8_log_info_val("reset_cause_demo", "boot cause", (uint32_t)cause);

  if (cause != k_ra8_reset_cause_software) {
    /* Cold boot (POR or other) -- trigger the SW-reset path that the HIL
     * gate validates. ra8_reset_software_reset does not return on target. */
    ra8_delay_ms(k_reset_cause_settle_ms);
    ra8_reset_software_reset();
    reset_cause_panic_halt();
  }

  /* Post-reset boot: cause == software. Advance the HIL counter so
   * jlink_memprobe sees movement and the gate passes. */
  while (1) {
    g_reset_cause_loop += 1U;
    ra8_delay_ms(k_reset_cause_loop_period_ms);
  }
}
#pragma GCC diagnostic pop
