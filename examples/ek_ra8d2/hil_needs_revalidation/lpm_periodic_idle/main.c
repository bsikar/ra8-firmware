/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_periodic_idle/main.c
 * @brief Periodic-app deep-idle loop: wake-do-work-standby on ULPT0 self-wake
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The "periodic apps" increment of the deep-idle foundation (roadmap
 * issue #153). Where ``lpm_ulpt_standby`` proves the bare wake path,
 * this app wraps it into the shape a real periodic application takes:
 * each period it does a small unit of work, then sleeps the CPU in
 * Software Standby until the on-chip Ultra-Low-Power Timer wakes it,
 * and repeats for a fixed number of periods before parking.
 *
 * Per-period sequence:
 *   1. **Work** -- toggle the user LED (a visible heartbeat) and emit
 *      ``"lpm_periodic_idle: work"`` over the SCI8 J-Link console.
 *   2. **Arm** -- start ULPT0 counting down a fixed period and confirm
 *      ``ULPTCR.TCSTF = 1`` (count actually started).
 *   3. **Standby** -- ``ra8_lpm_enter_sleep`` drops to Software Standby
 *      (LPSCR.LPMD = 0x5); every clock domain gates except LOCO and the
 *      always-on wake detectors.
 *   4. **Self-wake** -- the ULPT0 underflow cancels Software Standby
 *      through the same two-halves ICU path the standby demo proves.
 *   5. **Repeat** for ``k_lpi_period_count`` periods, then emit
 *      ``"lpm_periodic_idle PASS"`` and park.
 *
 * The exact, bench-proven wake path is reused verbatim from
 * ``lpm_ulpt_standby`` -- no new wake mechanism is invented:
 *   1. ``ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti, ...)`` links the
 *      ULPT0 underflow event into an ICU IELSRn slot and enables the
 *      matching NVIC line -- HUM 11.6.2.1 (p 482): "corresponding
 *      IELSRn register must be set before executing a WFI instruction",
 *      and Table 11.3 footnote *28 (p 434): the interrupt "must be
 *      enabled by NVIC_ISERn".
 *   2. ``ra8_lpm_arm_wupen1_bits(k_ra8_lpm_wupen1_ulpt0u)`` arms the async
 *      WUPEN1.ULP0U standby-cancel detector (HUM Ch 14.2.20 p 552).
 *      ULPT0_ULPTI is a valid SSTBY cancel source per Table 11.4
 *      (p 434).
 *
 * Why ULPT and not the RTC: with ULPTMR1.TCK1 = 0 the ULPT counts from
 * ULPTLCLK -- the LOCO-derived 32.768 kHz clock (HUM Table 9.2 p 320) --
 * which keeps running through Software Standby while LOCOCR.LCSTP = 0
 * (HUM Table 11.3 footnote *2 p 433). LOCO is internal, so unlike the
 * RTC alarm (which depends on the bare-board sub-clock crystal) this
 * wake source does not rely on the intermittent SOSC.
 *
 * @note ra8_emulator does not model Software-Standby clock-gating: it
 *       fast-forwards WFI to the next SysTick, so the periodic loop
 *       advances in the fake (boot, the work banners, and PASS all
 *       print) but the genuine ULPT0 self-wake -- the LOCO-clocked
 *       underflow cancelling Software Standby -- can only be confirmed
 *       on the bench, exactly as ``lpm_ulpt_standby`` was. This example
 *       is correct-by-construction against that proven sibling pending
 *       that bench run.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_time.h"
#include "ra8_ulpt.h"
#include "ra8_ulpt_regs.h"

/**
 * @enum lpi_const_t
 * @brief Demo tunables for the periodic-idle loop.
 *
 * @details
 * Groups every integer constant the loop needs so no magic numbers
 * leak into the control flow.
 *
 * @invariant ``k_lpi_period_ticks`` fits the 32-bit ULPT counter.
 * @invariant ``k_lpi_period_count`` is small enough that the bounded
 *            outer loop terminates in well under a second of wall time
 *            on the bench.
 *
 * @see lus_const_t in lpm_ulpt_standby for the wake-path twin.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lpi_baud = 115200U, /**< SCI8 console baud rate. */
  /* ULPTLCLK = LOCO / 1 = 32.768 kHz; 0x4000 (16384) ticks ~= 0.5 s. A
   * sub-second period keeps the bench heartbeat prompt without spinning
   * the console. */
  k_lpi_period_ticks = 0x4000U, /**< Lpi period ticks. */
  /* Bounded spin (NASA P10 Rule 2) waiting for ULPTCR.TCSTF to confirm
   * the count actually started before we drop into standby. */
  k_lpi_tcstf_poll_limit = 0x100000U, /**< Lpi tcstf poll limit. */
  /* Number of wake-work-standby periods before the app reports PASS and
   * parks. Bounds the outer loop (NASA P10 Rule 2). */
  k_lpi_period_count = 8U, /**< Lpi period count. */
} lpi_const_t;

/**
 * @enum lpi_chan_t
 * @brief ULPT channel selector for this demo.
 *
 * @details Only ULPT0 is used; its underflow is the wake source.
 *
 * @invariant The selected channel matches the ELC event armed in
 *            ``lpi_arm_wake`` (``k_ra8_elc_event_ulpt0_ulpti``).
 *
 * @see lpi_arm_wake
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_lpi_channel = 0U, /**< ULPT0 (its underflow is the wake source). */
} lpi_chan_t;

/**
 * @enum lpi_led_t
 * @brief Heartbeat LED selector.
 *
 * @details The blue user LED (LED1) is toggled once per period so a
 *          bench observer sees the wake cadence without the UART.
 *
 * @invariant The id is a valid ``ra8_board_led_id_t``.
 *
 * @see ra8_board_led_toggle
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_lpi_led = (uint8_t)k_ra8_board_led1, /**< Blue user LED heartbeat. */
} lpi_led_t;

/** @brief Boot banner -- emitted once before the first work step. */
static const uint8_t k_lpi_boot_msg[] = "lpm_periodic_idle: boot\r\n";

/** @brief Per-period work banner -- one per active period. */
static const uint8_t k_lpi_work_msg[] = "lpm_periodic_idle: work\r\n";

/** @brief Completion banner -- emitted once after the last period. */
static const uint8_t k_lpi_pass_msg[] = "lpm_periodic_idle PASS\r\n";

/**
 * @var g_lpm_periodic_wake_count
 * @brief Liveness counter -- bumped in the ULPT0 underflow ISR.
 *
 * @details
 * Incremented from ``lpi_ulpt_isr`` each time a ULPT0 underflow cancels
 * Software Standby. Exposed (non-static, volatile) so a J-Link session
 * can watch the genuine wake cadence independent of the loop's own
 * period counter.
 *
 * @note Written only by the ISR; a single 32-bit load elsewhere is
 *       atomic on the Cortex-M85.
 * @warning Do not modify outside the ISR.
 * @since 0.1.0
 */
volatile uint32_t g_lpm_periodic_wake_count = 0U;

/**
 * @brief Park the CPU forever on an unrecoverable bring-up failure.
 *
 * @details
 * Spins in WFI so an attached debugger can inspect state. Reached only
 * when a precondition of the deep-idle loop cannot be met.
 *
 * @pre A required init or loop step returned a non-``k_ra8_ok`` status.
 * @post Control never leaves this function.
 *
 * @note Not interrupt-safe by intent; it is a terminal trap.
 * @since 0.1.0
 */
[[noreturn]] static void lpi_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief ULPT0 underflow interrupt service routine.
 *
 * @details
 * Real path: ULPT0 counter underflow -> ULPT0_ULPTI ELC event ->
 * IELSRn link -> NVIC pend -> ra8_isr_dispatch -> here. Cancelling
 * Software Standby is handled by the WUPEN1.ULP0U detector + the NVIC
 * pend; this handler only records liveness. The underflow source flag
 * (ULPTCR.TUNF) is cleared by the ``ra8_ulpt_stop`` in the loop's
 * re-arm step, so the ISR stays minimal and allocation-free.
 *
 * @param[in] ctx Unused registration context.
 *
 * @pre Registered for ``k_ra8_elc_event_ulpt0_ulpti`` via ra8_isr_register.
 * @post ``g_lpm_periodic_wake_count`` has advanced by exactly one.
 *
 * @note Not re-entrant; a single ULPT channel drives it.
 * @since 0.1.0
 */
static void lpi_ulpt_isr(void* ctx)
{
  (void)ctx;
  g_lpm_periodic_wake_count++;
}

/**
 * @brief Spin until the ULPT count operation has actually started.
 *
 * @details
 * HUM Section 25.4.7 (p 1214) requires confirming ULPTCR.TCSTF = 1
 * (count operation started) before entering a standby mode -- otherwise
 * the standby transition can gate the sync clock before the counter has
 * begun, leaving it stalled and unable to underflow. ``ra8_ulpt_start``
 * only sets TSTART; this closes the gap from the application side. The
 * spin is bounded by ``k_lpi_tcstf_poll_limit`` (NASA P10 Rule 2).
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return ``k_ra8_ok`` once TCSTF = 1, else the status-read error or
 *         ``k_ra8_err_hw_timeout`` if the bound is reached.
 * @retval k_ra8_ok          Count confirmed running.
 * @retval k_ra8_err_hw_timeout  Bound reached without TCSTF set.
 *
 * @pre ``ra8_ulpt_start`` has set TSTART = 1 on @p channel.
 * @post On ``k_ra8_ok`` the ULPT counter is confirmed running.
 *
 * @note Polls only; performs no register writes, so it is re-entrant.
 * @see ra8_ulpt_get_status
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpi_wait_count_started(uint8_t channel)
{
  const uint8_t tcstf_mask = (uint8_t)(1U << (uint8_t)k_ra8_ulpt_bit_tcstf);
  for (uint32_t i = 0U; i < (uint32_t)k_lpi_tcstf_poll_limit; ++i) {
    uint8_t   status = 0U;
    ra8_err_t err    = ra8_ulpt_get_status(channel, &status);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((status & tcstf_mask) != 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Bring CGC + SysTick + SCI8 + LED + ULPT + LPM block up.
 *
 * @details
 * Same shape as ``lpm_ulpt_standby`` but also initialises the heartbeat
 * LED. The SCI8 J-Link console (PD02 TXD / PD03 RXD) is brought up via
 * the EK-RA8D2 board-support console API. LOCO is kept running so
 * ULPTLCLK survives Software Standby.
 *
 * @pre IRQs disabled (Reset_Handler default).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post LPM block has LPSCR.LPMD = 0 (System Active) until
 *       ``ra8_lpm_enter_sleep`` is called.
 *
 * @note Single-threaded init context; not interrupt-safe.
 * @see lpi_arm_wake
 * @since 0.1.0
 */
static void lpi_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    lpi_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lpi_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lpi_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpi_baud) != k_ra8_ok) {
    lpi_panic_halt();
  }
  if (ra8_board_led_init((ra8_board_led_id_t)k_lpi_led) != k_ra8_ok) {
    lpi_panic_halt();
  }
  if (ra8_ulpt_init() != k_ra8_ok) {
    lpi_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    lpi_panic_halt();
  }
  /* Keep LOCO running (LCSTP = 0) so ULPTLCLK survives Software Standby:
   * HUM Table 11.3 footnote *2 (p 433). */
  if (ra8_lpm_set_clock_stop(k_ra8_lpm_clock_loco, false) != k_ra8_ok) {
    lpi_panic_halt();
  }
}

/**
 * @brief Wire the ULPT0 underflow as a Software-Standby wake source.
 *
 * @details
 * Both halves of the RA8D2 wake path are required (see file header):
 * the IELSRn/NVIC link via ``ra8_isr_register`` AND the async
 * WUPEN1.ULP0U detector via ``ra8_lpm_arm_wupen1_bits``. This mirrors
 * the bench-proven ``lpm_ulpt_standby`` arm step exactly.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_isr_init != ok || ra8_isr_register != ok ||
 * ra8_lpm_arm_wupen1_bits != ok``. Three atomic conditions x N+1 = 4
 * vectors -- the all-ok runtime path plus each step's error path
 * (the underlying steps are covered in the HAL host unit tests).
 *
 * @return ``k_ra8_ok`` on success, else the first failing step's error.
 * @retval k_ra8_ok  All three wake-path steps succeeded.
 *
 * @pre ``lpi_setup_or_halt`` has run; interrupts not yet enabled.
 * @pre LPM block initialised so WUPEN writes take effect.
 * @post On success ULPT0_ULPTI vectors to ``lpi_ulpt_isr`` and
 *       WUPEN1.ULP0U is asserted.
 *
 * @note Single-threaded init context; not interrupt-safe.
 * @see lpi_ulpt_isr
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpi_arm_wake(void)
{
  ra8_err_t err = ra8_isr_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti,
                         lpi_ulpt_isr,
                         nullptr,
                         (uint8_t)k_ra8_isr_prio_default,
                         nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0u);
}

/**
 * @brief Do one period's unit of work: heartbeat LED + console banner.
 *
 * @details
 * The "application payload" stand-in for a real periodic task. Toggling
 * the LED gives a bench observer a visible cadence; the banner gives the
 * UART/HIL scrape a per-period marker.
 *
 * @return ``k_ra8_ok`` on success, else the first failing step's error.
 * @retval k_ra8_ok  LED toggled and banner transmitted.
 *
 * @pre ``lpi_setup_or_halt`` initialised the LED and console.
 * @post On ``k_ra8_ok`` the LED state is inverted and the work banner is
 *       queued for transmission.
 *
 * @note Runs with interrupts enabled; touches no shared ISR state.
 * @see lpi_idle_one_period
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpi_do_work(void)
{
  ra8_err_t err = ra8_board_led_toggle((ra8_board_led_id_t)k_lpi_led);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_board_uart_console_write(k_lpi_work_msg, (size_t)(sizeof(k_lpi_work_msg) - 1U));
}

/**
 * @brief Arm ULPT0, drop to Software Standby, and clean up after wake.
 *
 * @details
 * Re-arms the ULPT countdown, confirms ``ULPTCR.TCSTF = 1``, enters
 * Software Standby until the underflow cancels it, then stops ULPT0
 * (``ra8_ulpt_stop`` clears ULPTCR.TUNF) so the next period starts clean.
 * The standby entry is the bench-proven sequence from
 * ``lpm_ulpt_standby``.
 *
 * @return ``k_ra8_ok`` on a clean sleep/wake/cleanup, else the first
 *         failing step's error.
 * @retval k_ra8_ok              Slept and woke cleanly.
 * @retval k_ra8_err_hw_timeout  TCSTF never confirmed (see
 *                              ``lpi_wait_count_started``).
 *
 * @pre ``lpi_arm_wake`` armed the ULPT0 wake path and IRQs are enabled.
 * @pre LOCO is running so ULPTLCLK survives Software Standby.
 * @post On ``k_ra8_ok`` ULPT0 is stopped with TUNF cleared, ready to
 *       re-arm.
 *
 * @note Blocks the caller in Software Standby until the ULPT0 wake.
 * @see lpi_wait_count_started
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpi_idle_one_period(void)
{
  ra8_err_t err = ra8_ulpt_start((uint8_t)k_lpi_channel, (uint32_t)k_lpi_period_ticks);
  if (err != k_ra8_ok) {
    return err;
  }
  err = lpi_wait_count_started((uint8_t)k_lpi_channel);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_ulpt_stop((uint8_t)k_lpi_channel);
}

void main(void)
{
  lpi_setup_or_halt();

  if (lpi_arm_wake() != k_ra8_ok) {
    lpi_panic_halt();
  }
  ra8_isr_globals_enable();

  if (ra8_board_uart_console_write(k_lpi_boot_msg, (size_t)(sizeof(k_lpi_boot_msg) - 1U)) !=
      k_ra8_ok) {
    lpi_panic_halt();
  }

  /* Bounded periodic loop (NASA P10 Rule 2): k_lpi_period_count periods
   * of work-then-standby, each woken by the ULPT0 underflow. */
  for (uint32_t period = 0U; period < (uint32_t)k_lpi_period_count; ++period) {
    if (lpi_do_work() != k_ra8_ok) {
      lpi_panic_halt();
    }
    if (lpi_idle_one_period() != k_ra8_ok) {
      lpi_panic_halt();
    }
  }

  if (ra8_board_uart_console_write(k_lpi_pass_msg, (size_t)(sizeof(k_lpi_pass_msg) - 1U)) !=
      k_ra8_ok) {
    lpi_panic_halt();
  }

  lpi_panic_halt();
}
