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
 * The PAL send/recv primitives (ra_net_pal_send_frame,
 * ra_usb_pal_ep_open, ...) are real functions backed by
 * in-memory rings; on hardware the same rings are wired into
 * the GWCA descriptor engine and USB pipe FIFOs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "key_vault.h"
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
 * @brief Wave 10 secure key vault demo.
 *
 * @details
 * Programmes a deterministic test key into slot 0, runs a
 * challenge through the NSC veneer, and logs the first 4 bytes
 * of the digest so the SWO console shows the digest path worked
 * end-to-end. The raw key never leaves the secure world.
 */
static void internal_run_key_vault_demo(void)
{
  static const uint8_t k_demo_key[k_ra_key_vault_key_bytes] = {
    0xCAU, 0xFEU, 0xBAU, 0xBEU, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U, 0x23U, 0x45U,
    0x67U, 0x89U, 0xABU, 0xCDU, 0xEFU, 0xFEU, 0xDCU, 0xBAU, 0x98U, 0x76U, 0x54U,
    0x32U, 0x10U, 0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
  };
  static const uint8_t k_demo_chal[k_ra_key_vault_chal_bytes] = {
    0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xA8U, 0xA9U, 0xAAU,
    0xABU, 0xACU, 0xADU, 0xAEU, 0xAFU, 0xB0U, 0xB1U, 0xB2U, 0xB3U, 0xB4U, 0xB5U,
    0xB6U, 0xB7U, 0xB8U, 0xB9U, 0xBAU, 0xBBU, 0xBCU, 0xBDU, 0xBEU, 0xBFU,
  };
  static uint8_t s_demo_digest[k_ra_key_vault_digest_bytes];

  ra_err_t err = ra_key_vault_init();
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "ra_key_vault_init failed", (uint32_t)err);
    return;
  }
  err = ra_key_vault_store(0U, k_demo_key);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "key_vault_store failed", (uint32_t)err);
    return;
  }
  err = ra_nsc_key_vault_challenge(0U, k_demo_chal, s_demo_digest);
  if (err != k_ra_ok) {
    ra_log_error_val(s_tag, "key_vault_challenge failed", (uint32_t)err);
    return;
  }
  const uint32_t prefix = ((uint32_t)s_demo_digest[0] << 24) | ((uint32_t)s_demo_digest[1] << 16) |
                          ((uint32_t)s_demo_digest[2] << 8) | (uint32_t)s_demo_digest[3];
  ra_log_info_val(s_tag, "key vault digest[0..3]", prefix);
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
  internal_run_key_vault_demo();

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
