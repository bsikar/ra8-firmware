/**
 * @file ra8_board_ek_ra8d2_bringup.c
 * @brief EK-RA8D2 substrate prologue: clocks, module stop, timebase,
 *        console, LEDs, interrupts, in one audited order.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Holds the order the substrate actually requires, so an application no
 * longer inherits it from whichever sibling it was copied from. Every
 * step is a call this library or the HAL already exposes publicly; this
 * unit adds sequencing and one failure arm, no register access of its
 * own.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_bringup.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/**
 * @brief Bring up each user LED named by a ::ra8_board_bringup_led_mask_t.
 *
 * @details
 * Walks the LED ids in enumeration order, LED1 first, so a failure on a
 * contested pin reports the same LED regardless of which other bits the
 * caller set.
 *
 * @param[in] leds_mask Validated mask; no bit outside
 *                      ::k_ra8_board_bringup_leds_all.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every selected LED is configured as an output, off.
 * @retval other    Propagated unchanged from ``ra8_board_led_init``.
 *
 * @pre HAL pin validator initialised (single-threaded boot context).
 * @post Selected LEDs up to the first failure are off and owned.
 *
 * @note Not thread-safe; board bring-up context only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bringup_leds(uint32_t leds_mask)
{
  for (uint8_t led = 0U; led < (uint8_t)k_ra8_board_led_count; ++led) {
    const uint32_t bit = (uint32_t)1U << led;
    if ((leds_mask & bit) != 0U) {
      const ra8_err_t err = ra8_board_led_init((ra8_board_led_id_t)led);
      if (err != k_ra8_ok) {
        return err;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Steps 1 to 4: clock tree, module stop, live rates, timebase.
 *
 * @details
 * The half of the prologue every application needs whatever else it
 * asks for. Reads both rates back from the clock generator rather than
 * restating the board constants, so what the caller receives is what
 * the timebase and the console divisor were computed against.
 *
 * @param[out] out Live CPUCLK0 and PCLKA rates. Written only on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Tree up, module-stop table reset, SysTick running.
 * @retval other    Propagated unchanged from the first failing step.
 *
 * @pre Single-threaded board boot context.
 * @post On failure @p out is unchanged.
 *
 * @note Not thread-safe; board bring-up context only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bringup_substrate(ra8_board_bringup_out_t* out)
{
  /* Step 1: PLL1 clock tree. ra8_board_clocks_init wraps ra8_cgc_init and
   * is the seed this facade grew out of; it stays the only caller-visible
   * way to do just the clocks. */
  ra8_board_clock_rates_t rates = {};
  ra8_err_t               err   = ra8_board_clocks_init(&rates);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 2: module-stop refcounts. ra8_board_uart_console_init documents
   * this as a precondition, and every peripheral enabled later needs the
   * table reset first. */
  err = ra8_mstp_init();
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 3: read the rates back live. */
  uint32_t cpuclk0_hz = 0U;
  err                 = ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t pclka_hz = 0U;
  err               = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 4: 1 kHz SysTick against the live CPU rate. */
  err = ra8_time_init(cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }

  *out = (ra8_board_bringup_out_t){
    .cpuclk0_hz = cpuclk0_hz,
    .pclka_hz   = pclka_hz,
  };
  return k_ra8_ok;
}

ra8_err_t ra8_board_bringup(const ra8_board_bringup_cfg_t* cfg, ra8_board_bringup_out_t* out)
{
  if ((cfg == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->leds_mask & ~(uint32_t)k_ra8_board_bringup_leds_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  ra8_board_bringup_out_t rates = {};
  ra8_err_t               err   = internal_bringup_substrate(&rates);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 5: console, only when asked for. */
  if (cfg->console_baud != 0U) {
    err = ra8_board_uart_console_init(cfg->console_baud);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  /* Step 6: user LEDs, only those named. */
  err = internal_bringup_leds(cfg->leds_mask);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 7: unmask interrupts last, so nothing dispatches into a
   * half-initialised driver. */
  if (cfg->enable_interrupts) {
    ra8_isr_globals_enable();
  }

  *out = rates;
  return k_ra8_ok;
}
