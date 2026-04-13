/**
 * @file main.c
 * @brief ra8d2-firmware entry point -- Wave 8 single-world demo
 *
 * @details
 * Wave 8 lifts main.c from the Wave 0 LED-blink stub into a single-
 * world integration smoke test that exercises every Wave 7 PAL +
 * NSC veneer scaffold. Each subsystem reports through ra_log so a
 * failure on real hardware is immediately visible on the SWO console.
 *
 * Sequence:
 *
 *  1. ``Reset_Handler`` (vector_table.c) copies ``.data`` from MRAM
 *     to SRAM and zeroes ``.bss``, then calls ``SystemInit`` which
 *     programmes the cache + MPU.
 *  2. ``ra_infrastructure_init()`` -- log + pin validator + stack canary.
 *  3. ``ra_cgc_init()`` -- clock generator (no-op on MOCO).
 *  4. ``ra_nsc_periph_init()`` -- secure substrate dance
 *     (ra_mstp + ra_pwr + ra_isr + ra_dma).
 *  5. ``ra_net_pal_init()`` -- ethernet PAL on top of ra_eth.
 *  6. ``ra_usb_pal_init(k_ra_usb_speed_fs)`` -- USB device PAL.
 *  7. ``ra_nsc_log_emit("MAIN", "wave 8 demo up")`` to demonstrate
 *     the secure-side log veneer.
 *  8. LED1 blink loop with periodic ra_stack_canary_check().
 *
 * Stub primitives (ra_net_pal_send_frame, ra_usb_pal_ep_open, ...)
 * still return ``k_ra_err_not_supported`` -- the demo logs the
 * not-supported error for visibility but continues. Wave 7.1b /
 * 7.2b will replace those with real implementations.
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
#include "ra_net_pal.h"
#include "ra_nsc.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_usb_pal.h"

static const char* s_tag = "MAIN";

/**
 * @brief Default MAC address shipped on the EK board.
 *
 * @details
 * Locally-administered, second nibble of the first byte set to 2
 * per IEEE 802 OUI rules. Real boards override via OFS or board
 * EEPROM; this keeps the demo deterministic for now.
 */
static const ra_net_pal_mac_t k_demo_mac = {
  .bytes = {0x02U, 0xEAU, 0x8DU, 0x20U, 0x00U, 0x01U},
};

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
 * @brief Bring up the Wave 7 PAL stack on top of the secure substrate.
 *
 * @details
 * Each step logs success or the propagated error code so a failure on
 * real hardware is observable from the SWO console without a debugger
 * attached. The function is best-effort: a PAL that fails to come up
 * does not stop the LED blink demo, since LED1 is still useful as a
 * "main loop is alive" indicator.
 */
static void internal_bring_up_pals(void)
{
  ra_err_t err = ra_nsc_periph_init();
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_nsc_periph_init failed", (uint32_t)err);
  } else {
    ra_log_info(s_tag, "secure substrate up");
  }

  err = ra_net_pal_init(&k_demo_mac);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_net_pal_init failed", (uint32_t)err);
  } else {
    ra_log_info(s_tag, "net PAL ready");
  }

  err = ra_usb_pal_init(k_ra_usb_speed_fs);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_usb_pal_init failed", (uint32_t)err);
  } else {
    ra_log_info(s_tag, "usb PAL ready");
  }

  /* Drive the secure-side log veneer once so the linker pulls
   * libs/ra_nsc/src/ra_nsc_log.o into the final image and so the
   * SWO console shows a "PAL stack up" line at startup. */
  err = ra_nsc_log_emit("MAIN", "wave 8 demo up");
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_nsc_log_emit failed", (uint32_t)err);
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

  internal_bring_up_pals();

  ra_log_info(s_tag, "entering blink loop");

  enum : uint32_t {
    k_ra_blink_spin = 400000U,
  };

  while (1) {
    (void)ra_gpio_toggle(k_ra_pin_led1);
    internal_spin_delay(k_ra_blink_spin);

    /* Periodic stack canary check; logs (does not panic) if the
     * sentinel pattern was overwritten. The cross-build links
     * this into MRAM next to the rest of main. */
    if (ra_stack_canary_check() != k_ra_ok) {
      ra_log_error(s_tag, "stack canary corruption detected");
    }
  }

  /* Unreachable. */
  return 0;
}
#pragma GCC diagnostic pop
