/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_ulpt_standby/main.c
 * @brief ULPT0-underflow wake from Software Standby (LPSCR.LPMD = 0x5)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Deep-idle foundation demo: the CPU spends almost all of its time in
 * Software Standby (every clock domain gated except LOCO and the
 * always-on wake detectors) and is woken purely by the on-chip
 * Ultra-Low-Power Timer, with no off-chip crystal in the loop.
 *
 * Why ULPT and not the RTC: with ULPTMR1.TCK1 = 0 the ULPT counts from
 * ULPTLCLK -- the LOCO-derived 32.768 kHz clock (HUM Table 9.2 p 320) --
 * which keeps running through Software Standby while LOCOCR.LCSTP = 0
 * (HUM Table 11.3 footnote *2 p 433). LOCO is internal, so unlike the
 * RTC alarm (which depends on the bare-board sub-clock crystal) this
 * wake source does not rely on the intermittent SOSC.
 *
 * The wake path that the earlier ULPT standby attempt was missing: on
 * the RA8D2 a Software-Standby wake needs BOTH halves of the ICU wired,
 * not just the WUPEN bit.
 *   1. ``ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti, ...)`` links the
 *      ULPT0 underflow event into an ICU IELSRn slot and enables the
 *      matching NVIC line -- HUM 11.6.2.1 (p 482): "corresponding IELSRn
 *      register must be set before executing a WFI instruction", and
 *      Table 11.3 footnote *28 (p 434): the interrupt "must be enabled
 *      by NVIC_ISERn".
 *   2. ``ra8_lpm_arm_wupen1_bits(k_ra8_lpm_wupen1_ulpt0u)`` arms the
 *      async WUPEN1.ULP0U standby-cancel detector (HUM Ch 14.2.20
 *      p 552). ULPT0_ULPTI is a valid SSTBY cancel source per Table
 *      11.4 (p 434).
 *
 * Boot flow:
 *   1. CGC + SysTick + UART (SCI8) + ULPT + LPM bring-up.
 *   2. ``ra8_isr_register`` the ULPT0 underflow + arm WUPEN1.ULP0U.
 *   3. Emit boot banner ``"lpm_ulpt: boot\r\n"``.
 *   4. Loop: start ULPT0 (0.5 s), ``ra8_lpm_enter_sleep`` Software
 *      Standby, and on the underflow wake print ``"lpm_ulpt: wake\r\n"``.
 *
 * Unlike ``lpm_software_standby_demo`` (RTC / SOSC), the wake here runs
 * off LOCO, so the periodic ``"lpm_ulpt: wake"`` banner is a real,
 * bench-gatable wake signal (see ``hil.conf``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_time.h"
#include "ra8_ulpt.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_lus_baud = 115200U, /**< SCI8 console baud rate. */
  /* ULPTLCLK = LOCO / 1 = 32.768 kHz; 0x4000 (16384) ticks ~= 0.5 s. A
   * sub-second period keeps the bench wake banner prompt without
   * spinning the console. */
  k_lus_period_ticks = 0x4000U, /**< Lus period ticks. */
  /* Bounded spin (NASA P10 Rule 2) waiting for ULPTCR.TCSTF to confirm
   * the count actually started before we drop into standby. */
  k_lus_tcstf_poll_limit = 0x100000U, /**< Lus tcstf poll limit. */
} lus_const_t;

/** @brief ULPT channel selector. */
typedef enum : uint8_t {
  k_lus_channel = 0U, /**< ULPT0 (its underflow is the wake source). */
} lus_chan_t;

/** @brief Boot banner -- emitted once before the first standby entry. */
static const uint8_t k_lus_boot_msg[] = "lpm_ulpt: boot\r\n";

/** @brief Wake banner -- this is the HIL gate string (one per wake). */
static const uint8_t k_lus_wake_msg[] = "lpm_ulpt: wake\r\n";

/**
 * @var g_lpm_ulpt_wake_count
 * @brief Liveness counter -- bumped in the ULPT0 underflow ISR.
 *
 * @details
 * Incremented from ``lus_ulpt_isr`` each time a ULPT0 underflow cancels
 * Software Standby. Exposed (non-static, volatile) so a J-Link session
 * can watch the wake cadence without the UART.
 *
 * @note Written only by the ISR; a single 32-bit load elsewhere is
 *       atomic on the Cortex-M85.
 * @warning Do not modify outside the ISR.
 * @since 0.1.0
 */
volatile uint32_t g_lpm_ulpt_wake_count = 0U;

static void lus_panic_halt(void)
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
 * IELSRn link -> NVIC pend -> vector 16 + IRQ -> ra8_isr_dispatch ->
 * here. Cancelling Software Standby is handled by the WUPEN1.ULP0U
 * detector + the NVIC pend; this handler only records liveness. The
 * underflow source flag (ULPTCR.TUNF) is cleared by the
 * ``ra8_ulpt_stop`` in the main loop's re-arm step, so the ISR stays
 * minimal and allocation-free.
 *
 * @param[in] ctx Unused registration context.
 *
 * @pre Registered for ``k_ra8_elc_event_ulpt0_ulpti`` via ra8_isr_register.
 * @post ``g_lpm_ulpt_wake_count`` has advanced by exactly one.
 *
 * @note Not re-entrant; a single ULPT channel drives it.
 * @since 0.1.0
 */
static void lus_ulpt_isr(void* ctx)
{
  (void)ctx;
  g_lpm_ulpt_wake_count++;
}

/**
 * @brief Spin until the ULPT count operation has actually started.
 *
 * @details
 * HUM Section 25.4.7 (p 1214) requires confirming ULPTCR.TCSTF = 1
 * (count operation started) before entering a standby mode -- otherwise
 * the standby transition can gate the sync clock before the counter has
 * begun, leaving it stalled and unable to underflow. ``ra8_ulpt_start``
 * only sets TSTART; this closes the gap from the application side.
 *
 * @param[in] channel ULPT channel index (0 or 1).
 *
 * @return ``k_ra8_ok`` once TCSTF = 1, else the status-read error or
 *         ``k_ra8_err_hw_timeout`` if the bound is reached.
 *
 * @pre ``ra8_ulpt_start`` has set TSTART = 1 on @p channel.
 * @post On ``k_ra8_ok`` the ULPT counter is confirmed running.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lus_wait_count_started(uint8_t channel)
{
  const uint8_t tcstf_mask = (uint8_t)(1U << (uint8_t)k_ra8_ulpt_bit_tcstf);
  for (uint32_t i = 0U; i < (uint32_t)k_lus_tcstf_poll_limit; ++i) {
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
 * @brief Bring CGC + SysTick + SCI8 + ULPT + LPM block up.
 *
 * @details
 * Same shape as ``lpm_software_standby_demo`` but swaps the RTC for the
 * ULPT. The SCI8 J-Link console (PD02 TXD / PD03 RXD) is brought up via
 * the EK-RA8D2 board-support console API.
 *
 * @pre IRQs disabled (Reset_Handler default).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 *
 * @post On success every sub-system is armed; on failure the function
 *       panic-halts and never returns.
 * @post LPM block has LPSCR.LPMD = 0 (System Active) until
 *       ``ra8_lpm_enter_sleep`` is called.
 *
 * @since 0.1.0
 */
static void lus_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    lus_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lus_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lus_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lus_baud) != k_ra8_ok) {
    lus_panic_halt();
  }
  if (ra8_ulpt_init() != k_ra8_ok) {
    lus_panic_halt();
  }
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    lus_panic_halt();
  }
  /* Keep LOCO running (LCSTP = 0) so ULPTLCLK survives Software Standby:
   * HUM Table 11.3 footnote *2 (p 433) -- with IWDT unused and
   * LOCOCR.LCSTP = 0, LOCO is not stopped in Software Standby. */
  if (ra8_lpm_set_clock_stop(k_ra8_lpm_clock_loco, false) != k_ra8_ok) {
    lus_panic_halt();
  }
}

/**
 * @brief Wire the ULPT0 underflow as a Software-Standby wake source.
 *
 * @details
 * Both halves of the RA8D2 wake path are required (see file header):
 * the IELSRn/NVIC link via ``ra8_isr_register`` AND the async
 * WUPEN1.ULP0U detector via ``ra8_lpm_arm_wupen1_bits``.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_isr_init != ok || ra8_isr_register != ok ||
 * ra8_lpm_arm_wupen1_bits != ok``. Three atomic conditions x N+1 = 4
 * vectors -- the all-ok runtime path plus each step's error path
 * (covered in the host unit test).
 *
 * @return ``k_ra8_ok`` on success, else the first failing step's error.
 *
 * @pre ``lus_setup_or_halt`` has run; interrupts not yet enabled.
 * @pre LPM block initialised so WUPEN writes take effect.
 *
 * @post On success ULPT0_ULPTI vectors to ``lus_ulpt_isr`` and
 *       WUPEN1.ULP0U is asserted.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lus_arm_wake(void)
{
  ra8_err_t err = ra8_isr_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti,
                         lus_ulpt_isr,
                         nullptr,
                         (uint8_t)k_ra8_isr_prio_default,
                         nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0u);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lus_setup_or_halt();

  if (lus_arm_wake() != k_ra8_ok) {
    lus_panic_halt();
  }
  ra8_isr_globals_enable();

  (void)ra8_board_uart_console_write(k_lus_boot_msg, (size_t)(sizeof(k_lus_boot_msg) - 1U));

  while (1) {
    /* Re-arm the ULPT countdown, then drop into Software Standby until
     * the underflow cancels it. ra8_ulpt_stop clears ULPTCR.TUNF so the
     * next cycle starts clean. */
    if (ra8_ulpt_start((uint8_t)k_lus_channel, (uint32_t)k_lus_period_ticks) != k_ra8_ok) {
      break;
    }
    if (lus_wait_count_started((uint8_t)k_lus_channel) != k_ra8_ok) {
      break;
    }
    if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std) != k_ra8_ok) {
      break;
    }
    if (ra8_ulpt_stop((uint8_t)k_lus_channel) != k_ra8_ok) {
      break;
    }
    if (ra8_board_uart_console_write(k_lus_wake_msg, (size_t)(sizeof(k_lus_wake_msg) - 1U)) !=
        k_ra8_ok) {
      break;
    }
  }
  lus_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
