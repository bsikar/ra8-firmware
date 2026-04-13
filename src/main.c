/**
 * @file main.c
 * @brief ra8d2-firmware entry point -- minimal bring-up + LED blink
 *
 * @details
 * First application target: configure LED1 on P6_00 as an output and
 * toggle it in a spin loop. The chip defaults to MOCO (8 MHz) so we
 * do not need a real PLL bring-up to see blinking.
 *
 * ## Boot sequence
 *
 *  1. `Reset_Handler` (vector_table.c) copies `.data` from MRAM to
 *     SRAM and zeroes `.bss`.
 *  2. Reset_Handler jumps here.
 *  3. We run `ra_infrastructure_init()` to enable the FPU and prime
 *     the log / pin-validator layer.
 *  4. We call `ra_cgc_init()` (currently a no-op on MOCO).
 *  5. We configure LED1 as a GPIO output and loop forever, toggling.
 *
 * ## What this firmware is NOT doing yet
 *
 *  - No PLL (runs at 8 MHz MOCO).
 *  - No ITCM / DTCM cache enable (will come with the rest of
 *    infrastructure_init).
 *  - No IWDT refresh (OFS0 keeps the IWDT disabled until opted in).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_infrastructure.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"

static const char* s_tag = "MAIN";

/**
 * @brief Crude busy-wait used by the blink loop.
 *
 * @details
 * Runs a fixed number of `nop` instructions. Wall-clock time depends
 * on the current system clock; at the default MOCO speed (8 MHz) this
 * gives roughly half a second per call.
 *
 * @param[in] iterations Outer loop count.
 */
static void internal_spin_delay(uint32_t iterations)
{
  for (uint32_t i = 0U; i < iterations; i++) {
    __asm__ volatile("nop");
  }
}

/**
 * @brief Main entry.
 *
 * @return Never returns.
 *
 * @note ``int32_t`` is the project's fixed-width canonical name for
 *       ``int`` (see the no-bare-integer-type rule). GCC's
 *       ``-Wmain-return-type`` warning is pattern-based and rejects
 *       anything except the literal ``int``; the surrounding pragma
 *       silences it for this single definition while keeping the
 *       fixed-width spelling consistent with the rest of the tree.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  ra_infrastructure_init();

  ra_err_t err = ra_cgc_init();
  RA_ERROR_CHECK(err);

  err = ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low);
  RA_ERROR_CHECK(err);

  ra_log_info(s_tag, "entering blink loop");

  enum : uint32_t {
    k_ra_blink_spin = 400000U,
  };

  while (1) {
    (void)ra_gpio_toggle(k_ra_pin_led1);
    internal_spin_delay(k_ra_blink_spin);
  }

  /* Unreachable. */
  return 0;
}
#pragma GCC diagnostic pop
