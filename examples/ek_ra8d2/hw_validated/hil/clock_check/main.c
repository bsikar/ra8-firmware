/**
 * @file examples/ek_ra8d2/hw_validated/hil/clock_check/main.c
 * @brief Clock bring-up HIL test for EK-RA8D2 (CGC: HOCO + PLL)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip from reset-default MOCO (~8.4 MHz) up to its rated
 * operating speed via the CGC driver, then blinks the user LEDs at a
 * stopwatch-checkable 1 Hz so we can confirm SysTick timing matches
 * the new clock rate.
 *
 * Sequence:
 *   1. ``ra8_cgc_init()`` -- HOCO on, PLL1 locked, CPUCLK0 + bus
 *      clocks live. This is the first time the CGC driver runs on
 *      real silicon end-to-end.
 *   2. ``ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz)`` -- read
 *      back the configured rate so SysTick can use the actual value
 *      rather than a hardcoded constant.
 *   3. ``ra8_time_init(hz)`` -- programme SysTick for a 1 ms tick at
 *      the new clock rate.
 *   4. ``ra8_gpio_output_init()`` for each LED, then a 1 Hz toggle
 *      loop using ``ra8_delay_ms(500)``.
 *
 * Verification: the LEDs should toggle at exactly 1 Hz on a stopwatch
 * (within whatever PLL accuracy the chip guarantees -- typically
 * tens of ppm). If the period is off by orders of magnitude, the
 * reported CPUCLK0 value disagrees with the actual silicon clock and
 * something in the CGC bring-up went wrong.
 *
 * Compared to ``examples/blink_hal``: that demo runs on MOCO ~8.4 MHz
 * and ``ra8_delay_ms(500)`` is therefore ~500 ms. This demo runs on
 * the real CPUCLK0 (e.g. ~480 MHz post-PLL on Cortex-M85), so any
 * SysTick-arithmetic bug in ``ra8_time.c`` will show up immediately
 * as a wrong-by-100x blink rate.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_time_constants.h"

/** @brief Half-period of the visible blink, in milliseconds. */
typedef enum : uint32_t {
  k_clock_check_half_period_ms = 500U, /**< Clock check half period ms. */
} clock_check_period_t;

/**
 * @var g_clock_check_match
 * @brief HIL liveness counter -- incremented once per blink iteration
 *        when every queried clock-tree domain (CPUCLK0, CPUCLK1,
 *        ICLK, PCLKA..E, FCLK, MRICLK) reports its expected target
 *        frequency exactly.
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving the CGC bring-up not only completed without
 * faulting but also produced the targeted PLL1P-derived clock tree.
 * If the chip silently fell back to MOCO, the readback would not
 * match ``k_ra8_cpuclk0_hz`` and this counter would freeze.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_clock_check_match = 0U;

/**
 * @var g_clock_check_mismatch
 * @brief HIL failure counter -- incremented whenever any clock-tree
 *        readback disagrees with its expected target value (or the
 *        ra8_cgc_get_clock_hz call itself errors), or when an LED
 *        toggle fails.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches "PLL never locked", "wrong divider
 * programmed", and "CGC driver returned a stale value" -- previously
 * invisible because alive-mode could only see that the toggle loop
 * kept running, not what frequency it was actually running at.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_clock_check_mismatch = 0U;

/**
 * @struct clock_check_expected_t
 * @brief Pair of (clock id, expected post-PLL frequency in Hz).
 *
 * @details
 * The verification table walks every clock identifier the CGC driver
 * publishes and compares its runtime readback against the constant
 * defined in ``ra8_time_constants.h``.
 *
 * @invariant ``expected_hz`` matches the value programmed by
 *            ``ra8_cgc_init`` for ``id``.
 * @since 0.1.0
 */
typedef struct {
  ra8_clock_id_t id;          /**< Clock-tree domain identifier.        */
  uint32_t       expected_hz; /**< Target frequency after PLL bring-up. */
} clock_check_expected_t;

/* Forward declarations -- definitions appear after main() so the
 * audit_init_order linter sees the canonical CGC -> TIME -> peripheral
 * sequence in source order. */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_clock_check_pins_init(void);
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_clock_check_pins_toggle_all(void);
[[nodiscard]] RA8_INTERNAL static bool      internal_clock_check_verify_all(void);

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @details Preserves the failed clock or GPIO state in a low-activity loop for
 *          an attached debugger.
 *
 * @return None.
 *
 * @pre Called only after a fatal error in boot.
 * @pre Any desired HIL failure state has already been recorded.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No further clock validation or LED transition occurs.
 *
 * @note Interrupt wakeups return immediately to the permanent loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_clock_check_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Application entry. Lights HOCO + PLL, then runs a 1 Hz blink.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the toggle loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_clock_check_panic_halt();
  }

  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_clock_check_panic_halt();
  }

  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_clock_check_panic_halt();
  }

  if (internal_clock_check_pins_init() != k_ra8_ok) {
    internal_clock_check_panic_halt();
  }

  ra8_isr_globals_enable();

  while (1) {
    if (internal_clock_check_pins_toggle_all() != k_ra8_ok) {
      g_clock_check_mismatch += 1U;
      break;
    }
    if (internal_clock_check_verify_all()) {
      g_clock_check_match += 1U;
    } else {
      g_clock_check_mismatch += 1U;
    }
    ra8_delay_ms(k_clock_check_half_period_ms);
  }

  internal_clock_check_panic_halt();
}

/**
 * @brief Configure all three EK-RA8D2 user LEDs as outputs (low).
 *
 * @return Error code from the first failing GPIO init or k_ra8_ok.
 *
 * @retval k_ra8_ok               Every LED pin is now a digital output.
 * @retval k_ra8_err_invalid_arg  A pin id was rejected by the HAL.
 * @retval k_ra8_err_gpio_conflict A pin was already claimed.
 *
 * @pre IOPORT module is reachable (true on reset).
 * @pre Caller is single-threaded init context.
 *
 * @post On success P6_00, P3_03, P10_07 are output-low.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_clock_check_pins_init(void)
{
  ra8_err_t err = ra8_board_led_init(k_ra8_board_led1);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_led_init(k_ra8_board_led2);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_board_led_init(k_ra8_board_led3);
}

/**
 * @brief Toggle all three LED pins (one HAL call each).
 *
 * @return Error code from the first failing toggle or k_ra8_ok.
 *
 * @retval k_ra8_ok               All three pins toggled.
 * @retval k_ra8_err_invalid_arg  A pin id became invalid (shouldn't happen).
 *
 * @pre `internal_clock_check_pins_init()` has succeeded.
 *
 * @post Each LED's output latch is inverted from its prior value.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_clock_check_pins_toggle_all(void)
{
  ra8_err_t err = ra8_board_led_toggle(k_ra8_board_led1);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_led_toggle(k_ra8_board_led2);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_board_led_toggle(k_ra8_board_led3);
}

/**
 * @brief Verify every clock-tree readback equals its post-PLL target.
 *
 * @details
 * Walks a fixed table of (id, expected_hz) tuples and asks the CGC
 * driver what each clock is currently running at. The function
 * returns ``true`` only if every readback succeeds and matches the
 * constant exactly -- the values are programmed deterministically by
 * ``ra8_cgc_init`` so there is no tolerance to allow for.
 *
 * @return true  All queried clocks match their expected target.
 * @return false At least one clock readback errored or disagreed.
 *
 * @pre ``ra8_cgc_init()`` returned ``k_ra8_ok`` earlier this boot.
 * @pre Caller is single-threaded (the CGC driver is non-reentrant).
 *
 * @post No clock-tree state is mutated.
 * @post On false the table walk short-circuits at the first failure.
 *
 * @note Called from the main loop only; not ISR-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static bool internal_clock_check_verify_all(void)
{
  const clock_check_expected_t kExpected[] = {
    {k_ra8_clock_id_cpuclk0, (uint32_t)k_ra8_cpuclk0_hz},
    {k_ra8_clock_id_cpuclk1, (uint32_t)k_ra8_cpuclk1_hz},
    {k_ra8_clock_id_iclk, (uint32_t)k_ra8_iclk_hz},
    {k_ra8_clock_id_pclka, (uint32_t)k_ra8_pclka_hz},
    {k_ra8_clock_id_pclkb, (uint32_t)k_ra8_pclkb_hz},
    {k_ra8_clock_id_pclkc, (uint32_t)k_ra8_pclkc_hz},
    {k_ra8_clock_id_pclkd, (uint32_t)k_ra8_pclkd_hz},
    {k_ra8_clock_id_pclke, (uint32_t)k_ra8_pclke_hz},
    {k_ra8_clock_id_fclk, (uint32_t)k_ra8_fclk_hz},
    {k_ra8_clock_id_mriclk, (uint32_t)k_ra8_mriclk_hz},
  };
  const uint32_t kExpectedCount = (uint32_t)(sizeof(kExpected) / sizeof(kExpected[0]));
  for (uint32_t i = 0U; i < kExpectedCount; ++i) {
    uint32_t hz = 0U;
    if (ra8_cgc_get_clock_hz(kExpected[i].id, &hz) != k_ra8_ok) {
      return false;
    }
    if (hz != kExpected[i].expected_hz) {
      return false;
    }
  }
  return true;
}
