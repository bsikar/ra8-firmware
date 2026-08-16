/**
 * @file examples/ek_ra8d2/hw_pending/hal_timebase_demo/main.c
 * @brief HAL SysTick + DWT timebase demo for EK-RA8D2 (SCI8 @ 115200)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the `ra8_core` SysTick / DWT timebase primitive
 * (::ra8_systick_reload_for, ::ra8_systick_configure,
 * ::ra8_systick_current_value, ::ra8_dwt_cyccnt_enable /
 * ::ra8_dwt_cyccnt_reset / ::ra8_dwt_cyccnt_read) on real silicon, ALONGSIDE
 * the existing raw-poke `ra8_time` path, so the two can be diffed on the bench
 * (issue #582). Both program the same architectural SysTick registers; this app
 * shows the HAL primitive producing the identical reload the raw path uses, and
 * uses the DWT cycle counter to measure the wall-clock length of an
 * `ra8_delay_ms()` produced by the raw path.
 *
 * Sequence:
 *   1. `ra8_cgc_init()` -- XTAL + PLL1 up, CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
 *   2. `ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, ...)` -- the live core clock.
 *   3. `ra8_time_init(cpuclk0_hz)` -- the raw-poke path arms SysTick + DWT and
 *      backs `ra8_delay_ms()`.
 *   4. `ra8_board_uart_console_init(115200)` -- SCI8 J-Link OB console.
 *   5. `ra8_dwt_cyccnt_enable()` -- the HAL primitive owns the DWT enable.
 *   6. `ra8_systick_reload_for(cpuclk0_hz, 1000, &reload)` -- HAL arithmetic;
 *      print the reload so it can be compared against SYST_RVR on the bench.
 *   7. `ra8_systick_configure(reload, k_ra8_systick_clk_cpu, true)` -- the HAL
 *      path arms the same SysTick the raw path did (same reload, same control
 *      bits), so `ra8_delay_ms()` keeps working.
 *   8. Loop: reset DWT, `ra8_delay_ms(1000)`, read DWT -> print the measured
 *      cycles and derived microseconds, and toggle LED1 as a heartbeat.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1. You should see a
 * `reload=999999` line once, then a `hal timebase ms=1000 cycles=<n> us=<n>`
 * line per second with LED1 toggling in lock-step. On a 1 GHz core the measured
 * microseconds land near 1000000 for a 1000 ms delay.
 *
 * @author Brighton Sikarskie
 * @date 2026-08-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_systick.h"
#include "ra8_time.h"

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_demo_baud       = 115200U,  /**< Console baud.                         */
  k_demo_period_ms  = 1000U,    /**< Heartbeat + measured-delay period.    */
  k_demo_tick_hz    = 1000U,    /**< 1 kHz -> 1 ms SysTick tick.           */
  k_demo_u32_digits = 10U,      /**< Max decimal digits in a uint32_t.     */
  k_demo_hz_per_mhz = 1000000U, /**< Hz per MHz -> cycles-per-us divisor.  */
  k_demo_dec_base   = 10U,      /**< Decimal radix for the number emitter. */
} hal_timebase_config_t;

/** @brief Label emitted once with the HAL-computed SysTick reload. */
static const uint8_t k_demo_lbl_reload[] = "reload=";
/** @brief Label opening each per-second measurement line. */
static const uint8_t k_demo_lbl_ms[] = "hal timebase ms=";
/** @brief Field separator before the DWT cycle count. */
static const uint8_t k_demo_lbl_cycles[] = " cycles=";
/** @brief Field separator before the derived microseconds. */
static const uint8_t k_demo_lbl_us[] = " us=";
/** @brief Line terminator. */
static const uint8_t k_demo_crlf[] = "\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @details Parks the core so a boot failure is a visible dead LED / silent
 *          console rather than undefined execution. Only a debugger or an
 *          external reset resumes it.
 *
 * @pre Called only after a fatal error during boot.
 * @pre The console may or may not be up; no output is attempted here.
 * @post The CPU is parked in a WFI spin.
 * @post No further application code runs.
 *
 * @note Never returns.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a byte span to the SCI8 console, ignoring flow-control errors.
 *
 * @details Thin wrapper over the board console so the callers below read as a
 *          sequence of field emissions. A transient console error is
 *          non-fatal for a demo, so the return is intentionally discarded.
 *
 * @param[in] data Pointer to the bytes to emit. Must be non-NULL.
 * @param[in] len  Number of bytes to emit. Zero is a no-op.
 *
 * @pre `data` points to at least `len` readable bytes.
 * @pre The console has been initialised.
 * @post `len` bytes have been queued to the console (best effort).
 * @post No application state is modified.
 *
 * @note Not thread-safe; called only from the single-threaded main loop.
 * @since 0.1.0
 */
static void demo_emit(const uint8_t* data, size_t len)
{
  (void)ra8_board_uart_console_write(data, len);
}

/**
 * @brief Emit a uint32_t as decimal ASCII over the console.
 *
 * @details Formats @p value into a fixed 10-byte scratch buffer from the least
 *          significant digit up, then emits the significant span. Uses only
 *          single-condition, statically bounded loops (at most 10 iterations)
 *          so it carries no compound decision and no unbounded loop.
 *
 * @param[in] value Value to print in base ten.
 *
 * @pre The console has been initialised.
 * @pre None beyond the above.
 * @post The decimal text of @p value has been queued to the console.
 * @post No application state is modified.
 *
 * @note Not thread-safe; called only from the single-threaded main loop.
 * @since 0.1.0
 */
static void demo_emit_u32(uint32_t value)
{
  uint8_t  buf[k_demo_u32_digits];
  uint32_t idx = (uint32_t)k_demo_u32_digits;

  if (value == 0U) {
    idx--;
    buf[idx] = (uint8_t)'0';
  } else {
    for (uint32_t d = 0U; d < (uint32_t)k_demo_u32_digits; d++) {
      if (value == 0U) {
        break;
      }
      idx--;
      buf[idx] = (uint8_t)('0' + (uint8_t)(value % (uint32_t)k_demo_dec_base));
      value /= (uint32_t)k_demo_dec_base;
    }
  }

  demo_emit(&buf[idx], (size_t)((uint32_t)k_demo_u32_digits - idx));
}

/**
 * @brief Bring up CGC + raw ra8_time + console + LED + the HAL timebase.
 *
 * @details
 * Runs the raw `ra8_time_init()` path first (it arms SysTick + DWT and backs
 * `ra8_delay_ms()`), then re-arms the SAME SysTick through the HAL primitive
 * with the reload the HAL arithmetic derives -- demonstrating the primitive
 * substitutes for the raw poke with no timing change. Panic-halts on any
 * failure. On success the HAL-computed reload has been emitted once.
 *
 * @param[out] out_cpuclk_hz Receives the live CPUCLK0 frequency in Hz.
 *
 * @return Error code.
 * @retval k_ra8_ok               Everything up; `*out_cpuclk_hz` is valid.
 * @retval k_ra8_err_invalid_arg  `out_cpuclk_hz` is NULL.
 *
 * @pre `out_cpuclk_hz` is a valid, writable `uint32_t`.
 * @pre Reset_Handler and SystemInit have completed.
 * @post On success SysTick + DWT are running and the console is up.
 * @post On any HAL failure the function does not return (panic-halt).
 *
 * @note Not thread-safe; one-shot boot helper.
 * @since 0.1.0
 */
static ra8_err_t demo_setup_or_halt(uint32_t* out_cpuclk_hz)
{
  if (out_cpuclk_hz == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    demo_panic_halt();
  }

  /* The timebase primitive owns the DWT enable and the SysTick arithmetic. */
  ra8_dwt_cyccnt_enable();

  uint32_t reload = 0U;
  if (ra8_systick_reload_for(cpuclk0_hz, (uint32_t)k_demo_tick_hz, &reload) != k_ra8_ok) {
    demo_panic_halt();
  }
  /* Re-arm the SAME SysTick via the HAL path: same reload + control bits the
   * raw ra8_time_init() programmed, so ra8_delay_ms() keeps ticking. */
  if (ra8_systick_configure(reload, k_ra8_systick_clk_cpu, true) != k_ra8_ok) {
    demo_panic_halt();
  }

  demo_emit(k_demo_lbl_reload, sizeof(k_demo_lbl_reload) - 1U);
  demo_emit_u32(reload);
  demo_emit(k_demo_crlf, sizeof(k_demo_crlf) - 1U);

  *out_cpuclk_hz = cpuclk0_hz;
  return k_ra8_ok;
}

/**
 * @brief Emit one measurement line: DWT-timed length of a 1 s delay.
 *
 * @details Zeroes the DWT counter, runs `ra8_delay_ms(k_demo_period_ms)` (the
 *          raw path), samples the elapsed cycles, and derives microseconds as
 *          `cycles / (cpuclk_hz / 1e6)`. The divisor is floored at one so a
 *          sub-MHz clock cannot divide by zero.
 *
 * @param[in] cpuclk_hz Live CPUCLK0 in Hz, used to convert cycles to us.
 *
 * @pre The DWT cycle counter is enabled and the console is up.
 * @pre `cpuclk_hz` is the clock `ra8_delay_ms()` was calibrated against.
 * @post One `hal timebase ...` line has been emitted.
 * @post The DWT counter has been zeroed and has resumed counting.
 *
 * @note Not thread-safe; called only from the single-threaded main loop.
 * @since 0.1.0
 */
static void demo_emit_measurement(uint32_t cpuclk_hz)
{
  uint32_t cycles_per_us = cpuclk_hz / (uint32_t)k_demo_hz_per_mhz;
  if (cycles_per_us == 0U) {
    cycles_per_us = 1U;
  }

  ra8_dwt_cyccnt_reset();
  ra8_delay_ms((uint32_t)k_demo_period_ms);
  const uint32_t cycles = ra8_dwt_cyccnt_read();
  const uint32_t micros = cycles / cycles_per_us;

  demo_emit(k_demo_lbl_ms, sizeof(k_demo_lbl_ms) - 1U);
  demo_emit_u32((uint32_t)k_demo_period_ms);
  demo_emit(k_demo_lbl_cycles, sizeof(k_demo_lbl_cycles) - 1U);
  demo_emit_u32(cycles);
  demo_emit(k_demo_lbl_us, sizeof(k_demo_lbl_us) - 1U);
  demo_emit_u32(micros);
  demo_emit(k_demo_crlf, sizeof(k_demo_crlf) - 1U);
}

/**
 * @brief Application entry. Brings up the timebase then measures forever.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the measure + blink loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Never returns.
 * @since 0.1.0
 */
void main(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (demo_setup_or_halt(&cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }

  ra8_isr_globals_enable();

  while (1) {
    demo_emit_measurement(cpuclk0_hz);
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
  }

  demo_panic_halt();
}
