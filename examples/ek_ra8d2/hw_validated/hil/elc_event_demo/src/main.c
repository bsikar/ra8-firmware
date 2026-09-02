/**
 * @file examples/ek_ra8d2/hw_validated/hil/elc_event_demo/src/main.c
 * @brief Event Link Controller (ELC) software-event demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the
 * ``libs/ra8_hal/inc/ra8_elc.h`` driver. The Event Link Controller lets
 * one peripheral fire another peripheral's input without CPU
 * involvement (HUM Ch 19, p 817..836). This demo:
 *
 *   1. ``ra8_cgc_init`` -- clocks up.
 *   2. ``ra8_mstp_init`` + ``ra8_board_uart_console_init`` (PD02 / PD03 SCI8).
 *   3. ``ra8_elc_init`` -- powers on the controller and clears
 *      every ELSR slot.
 *   4. ``ra8_elc_link(0, k_ra8_elc_event_icu_irq0)`` -- routes
 *      external IRQ0 into ELSR slot 0. The slot is wired to GPT
 *      input-capture in HUM Table 19.2; on a real EVM a button
 *      on IRQ0 would trigger a hardware capture without a single
 *      CPU instruction.
 *   5. Once a second the demo fires
 *      ``ra8_elc_software_trigger(0)`` (the 3-step ELSEGR
 *      unlock-arm-set sequence from HUM Ch 19.2.2) so the
 *      runtime never depends on the user pressing a button.
 *      ``ra8_elc_is_enabled`` is read back into a status byte and
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

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_elc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_elc_demo_baud      = 115200U, /**< Elc demo baud.      */
  k_elc_demo_period_ms = 1000U,   /**< Elc demo period ms. */
} elc_demo_const_t;

/** @brief ELC slot / software-event indices used by the demo. */
typedef enum : uint8_t {
  k_elc_demo_slot         = 0U,  /**< ELSR0 -- arbitrary; HUM Tbl 19.2 maps to GPT capture. */
  k_elc_demo_sw_event_idx = 0U,  /**< ELSEGR0 -- software event 0.                          */
  k_elc_demo_ascii_zero   = '0', /**< Elc demo ascii zero.                                  */
  k_elc_demo_ascii_one    = '1', /**< Elc demo ascii one.                                   */
  k_elc_demo_dec_base     = 10U, /**< Elc demo dec base.                                    */
  k_elc_demo_uint_dec_max = 10U, /**< Max digits in a uint32 base-10.                       */
  k_elc_demo_print_buf    = 48U, /**< Max bytes in one print line.                          */
} elc_demo_byte_t;

/**
 * @brief Park forever after a fatal init failure.
 *
 * @details Repeatedly executes WFI so an attached debugger can inspect
 *          the failed bring-up state without further peripheral traffic.
 *
 * @pre Called only after a fatal error in boot.
 * @pre No caller requires recovery without an external reset.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No application state is modified after the first WFI.
 * @note Not thread-safe; this is the terminal single-threaded boot path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + console + ELC up. Panic-halts on any fail.
 *
 * @details Initializes the clocks, timebase, polled console, ELC slot-zero
 *          route, and both status LEDs in dependency order. Any failed HAL
 *          call transfers control to the terminal panic helper.
 *
 * @pre Reset startup has initialized static storage and the vector table.
 * @pre This function is called exactly once before global IRQ enable.
 * @post On return, ELSR0 routes ICU IRQ0 and both LEDs are usable.
 * @post The console and delay service are ready for the foreground loop.
 * @note Not thread-safe; it mutates global peripheral configuration.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_elc_demo_baud) != k_ra8_ok) {
    internal_panic_halt();
  }

  if (ra8_elc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  /* Route IRQ0 -> ELSR0 so a real button press would chain a GPT
   * capture without CPU intervention; the software trigger below
   * keeps the demo self-contained. */
  if (ra8_elc_link((uint8_t)k_elc_demo_slot, k_ra8_elc_event_icu_irq0) != k_ra8_ok) {
    internal_panic_halt();
  }

  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Write a decimal uint32 into ``buf``.
 *
 * @details Extracts digits into a fixed local reverse buffer, then copies
 *          them in display order. Zero takes the explicit single-digit path.
 *
 * @param[out] buf Destination buffer (must hold at least 10 chars).
 * @param[in]  v   Value to render.
 * @return Number of ASCII digits written.
 * @retval 1 A zero or single-digit value was rendered.
 * @retval 2..10 The exact number of digits in a larger value.
 *
 * @pre ``buf`` is non-NULL with >= 10 bytes capacity.
 * @pre The destination does not overlap this function's local storage.
 * @post No null terminator is written.
 * @post The returned prefix of ``buf`` is the base-10 representation of ``v``.
 * @note Reentrant when callers provide distinct destination buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_uint_to_dec(uint8_t* buf, uint32_t v)
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

/**
 * @brief Format the per-cycle "elc: en=N trig=NN\r\n" line.
 *
 * @details Copies fixed prefix, separator, and line-ending fragments around
 *          one enabled digit and the decimal trigger count without using a
 *          NUL-terminated or variadic formatting API.
 *
 * @param[out] out        Destination buffer.
 * @param[in]  enabled    ELCR.ELCON state.
 * @param[in]  trig_count Software-trigger count.
 * @return Bytes written into @p out.
 * @retval 18..27 Complete line length, depending on trigger-count width.
 *
 * @pre ``out`` is non-NULL with >= k_elc_demo_print_buf capacity.
 * @pre ``enabled`` and ``trig_count`` are stable for the duration of the call.
 * @post No bytes beyond the returned length are touched.
 * @post The returned prefix contains exactly one complete CRLF-terminated line.
 * @note Reentrant when callers provide distinct output buffers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_format_line(uint8_t* out, bool enabled, uint32_t trig_count)
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
  off += internal_uint_to_dec(&out[off], trig_count);
  for (uint32_t i = 0U; i < sizeof(suffix) - 1U; i++) {
    out[off++] = suffix[i];
  }
  return off;
}

/**
 * @brief Application entry. Fires ELC software events once a second.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the trigger + blink loop.
 * @post On any HAL hard error LED2 latches ON.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  uint32_t trig_count = 0U;
  while (1) {
    if (ra8_elc_software_trigger((uint8_t)k_elc_demo_sw_event_idx) != k_ra8_ok) {
      (void)ra8_board_led_on(k_ra8_board_led2);
      /* Emit FAIL banner so the HIL negative regex catches a silently
       * broken SW-trigger path; without this the t=N counter would
       * stop advancing but the demo would keep emitting "elc: en=1
       * t=0" indefinitely and the probe would pass. */
      const uint8_t trig_fail[] = "elc: FAIL trigger\r\n";
      (void)ra8_board_uart_console_write(trig_fail, (size_t)(sizeof(trig_fail) - 1U));
    } else {
      trig_count++;
    }
    bool enabled = false;
    if (ra8_elc_is_enabled(&enabled) != k_ra8_ok) {
      (void)ra8_board_led_on(k_ra8_board_led2);
      const uint8_t enabled_fail[] = "elc: FAIL is_enabled\r\n";
      (void)ra8_board_uart_console_write(enabled_fail, (size_t)(sizeof(enabled_fail) - 1U));
    }
    uint8_t        out[k_elc_demo_print_buf] = {};
    const uint32_t off                       = internal_format_line(out, enabled, trig_count);
    if (ra8_board_uart_console_write(out, (size_t)off) != k_ra8_ok) {
      break;
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_elc_demo_period_ms);
  }

  internal_panic_halt();
}
