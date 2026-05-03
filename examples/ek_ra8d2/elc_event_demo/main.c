/**
 * @file main.c
 * @brief Event Link Controller (ELC) software-event demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the
 * ``libs/ra_hal/ra_elc.h`` driver. The Event Link Controller lets
 * one peripheral fire another peripheral's input without CPU
 * involvement (HUM Ch 19, p 817..836). This demo:
 *
 *   1. ``ra_cgc_init`` -- clocks up.
 *   2. ``ra_mstp_init`` + console pins (PD02 / PD03 SCI8).
 *   3. ``ra_elc_init`` -- powers on the controller and clears
 *      every ELSR slot.
 *   4. ``ra_elc_link(0, k_ra_elc_event_icu_irq0)`` -- routes
 *      external IRQ0 into ELSR slot 0. The slot is wired to GPT
 *      input-capture in HUM Table 19.2; on a real EVM a button
 *      on IRQ0 would trigger a hardware capture without a single
 *      CPU instruction.
 *   5. Once a second the demo fires
 *      ``ra_elc_software_trigger(0)`` (the 3-step ELSEGR
 *      unlock-arm-set sequence from HUM Ch 19.2.2) so the
 *      runtime never depends on the user pressing a button.
 *      ``ra_elc_is_enabled`` is read back into a status byte and
 *      printed over SCI8.
 *
 * SCI8 prints ``"elc: en=1 trig=NN\r\n"`` once a second; LED1
 * toggles per cycle; LED2 latches on if any ELC call hard-fails.
 *
 * Bare EK-RA8D2 only -- no external pins required because the
 * software trigger replaces a real edge.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_elc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_elc_demo_baud        = 115200U,
  k_elc_demo_period_ms   = 1000U,
  k_elc_demo_sci_channel = 8U,
} elc_demo_const_t;

/** @brief ELC slot / software-event indices used by the demo. */
typedef enum : uint8_t {
  k_elc_demo_slot         = 0U, /**< ELSR0 -- arbitrary; HUM Tbl 19.2 maps to GPT capture. */
  k_elc_demo_sw_event_idx = 0U, /**< ELSEGR0 -- software event 0. */
  k_elc_demo_ascii_zero   = '0',
  k_elc_demo_ascii_one    = '1',
  k_elc_demo_dec_base     = 10U,
  k_elc_demo_uint_dec_max = 10U, /**< Max digits in a uint32 base-10. */
  k_elc_demo_print_buf    = 48U, /**< Max bytes in one print line.    */
} elc_demo_byte_t;

/** @brief Pinout for SCI8 console. */
static const ra_port_pin_t k_elc_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_elc_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void elc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + console + ELC up. Panic-halts on any fail.
 *
 * @since 0.1.0
 */
static void elc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_elc_demo_pin_txd, k_ra_psel_sci_async, "elc_event_demo.txd8") !=
      k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_elc_demo_pin_rxd, k_ra_psel_sci_async, "elc_event_demo.rxd8") !=
      k_ra_ok) {
    elc_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_elc_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_elc_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    elc_demo_panic_halt();
  }

  if (ra_elc_init() != k_ra_ok) {
    elc_demo_panic_halt();
  }
  /* Route IRQ0 -> ELSR0 so a real button press would chain a GPT
   * capture without CPU intervention; the software trigger below
   * keeps the demo self-contained. */
  if (ra_elc_link((uint8_t)k_elc_demo_slot, k_ra_elc_event_icu_irq0) != k_ra_ok) {
    elc_demo_panic_halt();
  }

  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    elc_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    elc_demo_panic_halt();
  }
}

/**
 * @brief Write a decimal uint32 into ``buf``.
 *
 * @param[out] buf Destination buffer (must hold at least 10 chars).
 * @param[in]  v   Value to render.
 * @return Number of ASCII digits written.
 *
 * @pre ``buf`` is non-NULL with >= 10 bytes capacity.
 * @post No null terminator is written.
 * @since 0.1.0
 */
static uint8_t elc_demo_uint_to_dec(uint8_t* buf, uint32_t v)
{
  uint8_t  tmp[k_elc_demo_uint_dec_max];
  uint8_t  n = 0U;
  uint32_t r = v;
  if (r == 0U) {
    buf[0] = (uint8_t)k_elc_demo_ascii_zero;
    return 1U;
  }
  while ((r > 0U) && (n < (uint8_t)k_elc_demo_uint_dec_max)) {
    tmp[n] =
      (uint8_t)((uint8_t)k_elc_demo_ascii_zero + (uint8_t)(r % (uint32_t)k_elc_demo_dec_base));
    r = r / (uint32_t)k_elc_demo_dec_base;
    n++;
  }
  for (uint8_t i = 0U; i < n; i++) {
    buf[i] = tmp[(uint8_t)(n - 1U - i)];
  }
  return n;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Fires ELC software events once a second.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the trigger + blink loop.
 * @post On any HAL hard error LED2 latches ON.
 *
 * @since 0.1.0
 */
/**
 * @brief Format the per-cycle "elc: en=N trig=NN\r\n" line.
 *
 * @param[out] out        Destination buffer.
 * @param[in]  enabled    ELCR.ELCON state.
 * @param[in]  trig_count Software-trigger count.
 * @return Bytes written into @p out.
 *
 * @pre ``out`` is non-NULL with >= k_elc_demo_print_buf capacity.
 * @post No bytes beyond the returned length are touched.
 *
 * @since 0.1.0
 */
static uint32_t elc_demo_format_line(uint8_t* out, bool enabled, uint32_t trig_count)
{
  uint32_t      off      = 0U;
  const uint8_t prefix[] = "elc: en=";
  const uint8_t mid[]    = " trig=";
  const uint8_t suffix[] = "\r\n";
  for (uint32_t i = 0U; i < sizeof(prefix) - 1U; i++) {
    out[off++] = prefix[i];
  }
  out[off++] = enabled ? (uint8_t)k_elc_demo_ascii_one : (uint8_t)k_elc_demo_ascii_zero;
  for (uint32_t i = 0U; i < sizeof(mid) - 1U; i++) {
    out[off++] = mid[i];
  }
  off += elc_demo_uint_to_dec(&out[off], trig_count);
  for (uint32_t i = 0U; i < sizeof(suffix) - 1U; i++) {
    out[off++] = suffix[i];
  }
  return off;
}

int32_t main(void)
{
  elc_demo_setup_or_halt();
  ra_isr_globals_enable();

  uint32_t trig_count = 0U;
  while (1) {
    if (ra_elc_software_trigger((uint8_t)k_elc_demo_sw_event_idx) != k_ra_ok) {
      (void)ra_board_led_on(k_ra_board_led2);
    } else {
      trig_count++;
    }
    bool enabled = false;
    if (ra_elc_is_enabled(&enabled) != k_ra_ok) {
      (void)ra_board_led_on(k_ra_board_led2);
    }
    uint8_t        out[k_elc_demo_print_buf] = {};
    const uint32_t off                       = elc_demo_format_line(out, enabled, trig_count);
    if (ra_sci_write_polling((uint8_t)k_elc_demo_sci_channel, out, off) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_elc_demo_period_ms);
  }

  elc_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
