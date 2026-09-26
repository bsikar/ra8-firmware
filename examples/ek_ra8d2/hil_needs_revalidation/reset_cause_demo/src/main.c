/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/reset_cause_demo/src/main.c
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

#include "ra8_boot_entry.h"
#include "ra8_check.h"
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

void main(void)
{
  RA8_BOOT_REQUIRE(ra8_time_init(k_reset_cause_cpu_hz_at_reset), "time_init");
  ra8_isr_globals_enable();

  RA8_BOOT_REQUIRE(ra8_reset_init(), "reset_init");

  ra8_reset_cause_t cause = k_ra8_reset_cause_unknown;
  RA8_BOOT_REQUIRE(ra8_reset_get_cause(&cause), "reset_get_cause");
  g_reset_cause_initial = (uint32_t)cause;
  ra8_log_info_val("reset_cause_demo", "boot cause", (uint32_t)cause);

  if (cause != k_ra8_reset_cause_software) {
    /* Cold boot (POR or other) -- trigger the SW-reset path that the HIL
     * gate validates. ra8_reset_software_reset does not return on target. */
    ra8_delay_ms(k_reset_cause_settle_ms);
    ra8_reset_software_reset();
    ra8_fatal_error("BOOT", "software_reset returned", (uint32_t)k_ra8_err_invalid_state);
  }

  /* Post-reset boot: cause == software. Advance the HIL counter so
   * jlink_memprobe sees movement and the gate passes. */
  while (1) {
    g_reset_cause_loop += 1U;
    ra8_delay_ms(k_reset_cause_loop_period_ms);
  }
}
