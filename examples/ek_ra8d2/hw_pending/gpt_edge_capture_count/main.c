/**
 * @file examples/ek_ra8d2/hw_pending/gpt_edge_capture_count/main.c
 * @brief GPT hardware input-capture + external event/pulse counting demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the two GPT edge-driven driver features that
 * ``gpt_capture_input`` only *approximates* in software:
 *
 * - **Input capture (issue #185)**: GPT0 runs as a free-running 32-bit
 *   up-counter. ``ra8_gpt_capture_configure`` arms GTICASR so that each
 *   rising edge on the GTIOC0A pin latches the live GTCNT value into
 *   GTCCRA (hardware timestamp -- no CPU jitter). The loop polls the
 *   GTST.TCFA capture flag, reads the latch with ``ra8_gpt_capture_read``,
 *   and the delta between two consecutive latches is the measured signal
 *   period in counter ticks.
 * - **External event / pulse counting (issue #186)**: GPT1 is switched
 *   out of internal-clock counting by ``ra8_gpt_event_count_configure``
 *   (GTUPSR = GTIOC1A rising). GTCNT then increments once per external
 *   rising edge, so ``ra8_gpt_read`` returns an accumulated pulse count.
 *   The commented quadrature variant (up = GTIOC1A rising, down =
 *   GTIOC1A falling) turns the same channel into an encoder up/down
 *   counter.
 *
 * The measured values are published in J-Link-readable globals so a
 * future bench run can assert them against a known stimulus frequency.
 *
 * @par Required hardware (why this is hw_pending):
 * The capture / count inputs need a real edge source. Either:
 *   1. an external square-wave signal generator on the GTIOC0A and
 *      GTIOC1A pads, or
 *   2. a jumper loop-back from a GPT PWM output pin (e.g. run
 *      ``gpt_pwm_demo`` on another channel and wire its GTIOCnA output
 *      to these capture inputs), or
 *   3. a quadrature encoder for the up/down variant.
 * The stock EK-RA8D2 also has no cleanly-broken-out GTIOCnA pad (the
 * candidate pins collide with the debug UART per the Hardware User's
 * Manual pin tables); see the TODO on the pin table below. Until a
 * carrier board or jumper wiring exists this app is not bench-validated.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_gpt.h"
#include "ra8_gpt_capture.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/**
 * @enum gpt_ecc_period_const_t
 * @brief 32-bit counter tunables (free-run period + capture window).
 *
 * @details
 * ``k_gpt_ecc_period`` is the full-scale GTPR so GTCNT behaves as a
 * 32-bit free-running accumulator. A captured delta is treated as a
 * genuine signal period only when it falls inside
 * [``k_gpt_ecc_min_valid_ticks``, ``k_gpt_ecc_max_valid_ticks``]: the
 * lower bound rejects sub-microsecond noise / contact bounce, the upper
 * bound rejects the first (uninitialised) latch and any counter-wrap
 * artefact.
 */
typedef enum : uint32_t {
  k_gpt_ecc_period          = 0xFFFFFFFFUL, /**< GTPR full-scale.            */
  k_gpt_ecc_min_valid_ticks = 0x00000064UL, /**< 100: glitch-reject floor.   */
  k_gpt_ecc_max_valid_ticks = 0x7FFFFFFFUL, /**< Wrap / stale-latch ceiling. */
} gpt_ecc_period_const_t;

/**
 * @enum gpt_ecc_poll_const_t
 * @brief Main-loop poll cadence in milliseconds.
 */
typedef enum : uint16_t {
  k_gpt_ecc_poll_period_ms = 5U, /**< Sample GTST / GTCNT every 5 ms. */
} gpt_ecc_poll_const_t;

/**
 * @enum gpt_ecc_layout_t
 * @brief GPT channel + LED selection for the demo.
 */
typedef enum : uint8_t {
  k_gpt_ecc_capture_channel = 0U, /**< GPT0: input-capture channel. */
  k_gpt_ecc_count_channel   = 1U, /**< GPT1: event-count channel.   */
} gpt_ecc_layout_t;

/**
 * @var k_gpt_ecc_pin_capture
 * @brief GTIOC0A input pad for the capture channel (placeholder).
 *
 * @details
 * TODO(board-rev): the EK-RA8D2 breaks out no conflict-free GTIOC0A
 * pad -- the candidate pins overlap the debug UART per HUM Ch 20.6 pin
 * tables. This P408 assignment is illustrative so the app builds and
 * the ``ra8_pfs_route_peripheral`` call is exercised; update it (or move
 * to a carrier board) before bench validation.
 *
 * @note Consumed by ``gpt_ecc_route_pins_or_halt``.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_gpt_ecc_pin_capture = RA8_PIN(k_ra8_port_4, k_ra8_pin_8);

/**
 * @var k_gpt_ecc_pin_count
 * @brief GTIOC1A input pad for the event-count channel (placeholder).
 *
 * @details TODO(board-rev): same EK-RA8D2 pad-availability caveat as
 * ``k_gpt_ecc_pin_capture``; P409 is illustrative.
 *
 * @note Consumed by ``gpt_ecc_route_pins_or_halt``.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_gpt_ecc_pin_count = RA8_PIN(k_ra8_port_4, k_ra8_pin_9);

/**
 * @var g_gpt_ecc_last_period
 * @brief Most recent valid input-capture delta, in GPT counter ticks.
 *
 * @details Updated only when a captured delta passes
 * ``gpt_ecc_period_valid``. Zero until the first valid capture.
 * @note Read externally by J-Link / a future HIL probe.
 * @warning Written from the main loop; do not modify externally.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_ecc_last_period = 0U;

/**
 * @var g_gpt_ecc_capture_events
 * @brief Count of valid input-capture edges observed so far.
 *
 * @note Read externally by J-Link / a future HIL probe.
 * @warning Written from the main loop; do not modify externally.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_ecc_capture_events = 0U;

/**
 * @var g_gpt_ecc_pulse_count
 * @brief Latest accumulated external pulse count (GPT1 GTCNT).
 *
 * @details Reflects GTUPSR-driven event counting: the number of
 * external rising edges the count channel has tallied.
 * @note Read externally by J-Link / a future HIL probe.
 * @warning Written from the main loop; do not modify externally.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_ecc_pulse_count = 0U;

/**
 * @var g_gpt_ecc_tick
 * @brief Pin-independent liveness counter.
 *
 * @details Bumped whenever the capture channel's free-running GTCNT
 * advanced between two samples. Proves the GPT counter is alive even
 * when no edge source is wired (the unattended-bench case), separately
 * from the pin-driven capture / count signals above.
 * @note Read externally by J-Link / a future HIL probe.
 * @warning Written from the main loop; do not modify externally.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_ecc_tick = 0U;

/**
 * @brief Park the CPU after a fatal init failure.
 *
 * @details Spins in a low-power wait so a debugger can inspect state.
 * @pre A setup step returned an error.
 * @pre No further progress is safe.
 * @post Control never returns to the caller.
 * @post No registers are modified.
 * @note Not thread-safe; terminal path.
 * @since 0.1.0
 */
static void gpt_ecc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Classify a captured delta as a genuine signal period.
 *
 * @details Windows the delta to reject noise (too short) and the first
 * / wrapped latch (too long). This is the demo's one compound decision.
 *
 * @param[in] delta_ticks Difference between two consecutive GTCCRA
 *                        latches, in GPT counter ticks (0 .. UINT32_MAX).
 * @return true when ``k_gpt_ecc_min_valid_ticks <= delta_ticks <=
 *         k_gpt_ecc_max_valid_ticks``, false otherwise.
 *
 * @pre The two source latches came from an armed capture register.
 * @pre ``delta_ticks`` was computed as an unsigned subtraction.
 * @post No state is modified (pure predicate).
 * @post The return reflects only ``delta_ticks``.
 *
 * @par MC/DC:
 * Compound decision ``(delta >= min) && (delta <= max)`` -- two
 * conditions, N+1 = 3 vectors (in-window true; below-floor false;
 * above-ceiling false). The companion host test
 * ``tests/test_app_gpt_edge_capture_count.c`` exercises each vector.
 *
 * @note Thread safety: reentrant (no shared state).
 * @since 0.1.0
 */
static bool gpt_ecc_period_valid(uint32_t delta_ticks)
{
  return (delta_ticks >= (uint32_t)k_gpt_ecc_min_valid_ticks) &&
         (delta_ticks <= (uint32_t)k_gpt_ecc_max_valid_ticks);
}

/**
 * @brief Bring up the core clocks, SysTick, and LED1.
 *
 * @details CGC -> SysTick (from CPUCLK0) -> LED1 -> module-stop model.
 * Any failure halts via ``gpt_ecc_panic_halt``.
 * @pre Reset boot code has run (SystemInit set VTOR / FPU).
 * @pre Called once from ``main`` before timer setup.
 * @post On return the clocks, delay timebase, LED1, and MSTP are live.
 * @post On failure control never returns.
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static void gpt_ecc_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
}

/**
 * @brief Route the two GTIOCnA edge-input pins to the GPT peripheral.
 *
 * @details Uses ``ra8_pfs_route_peripheral`` with PSEL
 * ``k_ra8_psel_gpt0`` (GPT channels 0..3). Any failure halts.
 * @pre ``gpt_ecc_clocks_or_halt`` has run.
 * @pre The placeholder pins are conflict-free (see TODO board-rev).
 * @post On return GTIOC0A / GTIOC1A are muxed to the GPT.
 * @post On failure control never returns.
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static void gpt_ecc_route_pins_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_gpt_ecc_pin_capture, k_ra8_psel_gpt0, "gpt_ecc.gtioc0a") !=
      k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_gpt_ecc_pin_count, k_ra8_psel_gpt0, "gpt_ecc.gtioc1a") !=
      k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
}

/**
 * @brief Configure the capture (GPT0) and count (GPT1) timer channels.
 *
 * @details
 * GPT0 starts free-running and GTICASR is armed for GTIOC0A rising-edge
 * capture (#185). GPT1 starts and GTUPSR is armed for GTIOC1A
 * rising-edge counting (#186). Any failure halts.
 * @pre ``gpt_ecc_route_pins_or_halt`` has run.
 * @pre The GPT block clock is enabled by ``ra8_gpt_init``.
 * @post On return both channels are counting; capture / count sources
 *       are armed.
 * @post On failure control never returns.
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static void gpt_ecc_timers_or_halt(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = (uint32_t)k_gpt_ecc_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  if (ra8_gpt_init((uint8_t)k_gpt_ecc_capture_channel, &cfg) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  /* #185 input capture: latch GTCNT into GTCCRA on each GTIOC0A rising edge. */
  if (ra8_gpt_capture_configure((uint8_t)k_gpt_ecc_capture_channel,
                                k_ra8_gpt_ccr_a,
                                (uint32_t)k_ra8_gpt_cap_src_ioca_rising) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  if (ra8_gpt_init((uint8_t)k_gpt_ecc_count_channel, &cfg) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
  /* #186 pulse counting: GTCNT counts GTIOC1A rising edges (up source only).
   * Quadrature variant: pass k_ra8_gpt_cnt_src_ioca_falling as the down
   * source instead of k_ra8_gpt_cnt_src_none for an encoder up/down count. */
  if (ra8_gpt_event_count_configure((uint8_t)k_gpt_ecc_count_channel,
                                    (uint32_t)k_ra8_gpt_cnt_src_ioca_rising,
                                    (uint32_t)k_ra8_gpt_cnt_src_none) != k_ra8_ok) {
    gpt_ecc_panic_halt();
  }
}

/**
 * @brief Service one input-capture poll: latch delta -> globals.
 *
 * @details Checks GTST.TCFA; on a fresh latch reads GTCCRA, computes
 * the delta from ``*prev_latch``, and on a valid period updates the
 * published globals and toggles LED1. Clears TCFA before returning.
 *
 * @param[in,out] prev_latch Previous GTCCRA latch value; updated to the
 *                           current latch when a capture is serviced.
 * @return true when a fresh capture edge was serviced, false otherwise.
 *
 * @pre ``gpt_ecc_timers_or_halt`` armed the capture channel.
 * @pre ``prev_latch`` points to writable storage.
 * @post ``*prev_latch`` holds the newest latch when true is returned.
 * @post GTST.TCFA is cleared when a capture was serviced.
 * @note Not thread-safe; called only from the main loop.
 * @since 0.1.0
 */
static bool gpt_ecc_service_capture(uint32_t* prev_latch)
{
  uint32_t status = 0U;
  if (ra8_gpt_get_status((uint8_t)k_gpt_ecc_capture_channel, &status) != k_ra8_ok) {
    return false;
  }
  if ((status & (uint32_t)k_ra8_gpt_status_ccra) == 0U) {
    return false;
  }
  uint32_t latch = 0U;
  if (ra8_gpt_capture_read((uint8_t)k_gpt_ecc_capture_channel, k_ra8_gpt_ccr_a, &latch) ==
      k_ra8_ok) {
    const uint32_t delta = latch - *prev_latch;
    *prev_latch          = latch;
    if (gpt_ecc_period_valid(delta)) {
      g_gpt_ecc_last_period    = delta;
      g_gpt_ecc_capture_events = g_gpt_ecc_capture_events + 1U;
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }
  }
  (void)ra8_gpt_clear_status((uint8_t)k_gpt_ecc_capture_channel, (uint32_t)k_ra8_gpt_status_ccra);
  return true;
}

void main(void)
{
  gpt_ecc_clocks_or_halt();
  gpt_ecc_route_pins_or_halt();
  gpt_ecc_timers_or_halt();
  ra8_isr_globals_enable();

  uint32_t prev_latch = 0U;
  uint32_t prev_free  = 0U;
  bool     have_prev  = false;

  while (1) {
    /* #185 input capture: hardware edge timestamp -> period. */
    (void)gpt_ecc_service_capture(&prev_latch);

    /* #186 external event counting: accumulated GTIOC1A edge count. */
    uint32_t pulses = 0U;
    if (ra8_gpt_read((uint8_t)k_gpt_ecc_count_channel, &pulses) == k_ra8_ok) {
      g_gpt_ecc_pulse_count = pulses;
    }

    /* Pin-independent liveness: capture-channel free-run counter alive. */
    uint32_t now_free = 0U;
    if (ra8_gpt_read((uint8_t)k_gpt_ecc_capture_channel, &now_free) == k_ra8_ok) {
      if (have_prev) {
        if (now_free != prev_free) {
          g_gpt_ecc_tick = g_gpt_ecc_tick + 1U;
        }
      }
      prev_free = now_free;
      have_prev = true;
    }
    ra8_delay_ms((uint32_t)k_gpt_ecc_poll_period_ms);
  }
  gpt_ecc_panic_halt();
}
