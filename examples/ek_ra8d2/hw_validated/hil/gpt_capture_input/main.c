/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_capture_input/main.c
 * @brief GPT input-capture-style demo: measure SW1 press period
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Programs GPT0 as a free-running 32-bit up-counter with prescaler
 * PCLKD/64. On each falling edge of SW1 (active-low) the application
 * latches the current GPT counter value and computes the delta from
 * the previous press: that delta is the measured "input capture
 * period" in GPT counter ticks. The lowest LED is toggled on every
 * captured edge so a human can see live activity.
 *
 * Note: the RA8D2 GPT supports hardware GTIOC pin capture, and the HAL
 * now exposes it via ra8_gpt_capture_configure / ra8_gpt_capture_read.
 * This demo deliberately stays on the free-run + counter-read
 * primitives and approximates input capture in software via SW1
 * polling, so it needs no external signal source and stays HIL-able on
 * a bare EVM. The real hardware-capture + external-event-count path is
 * demonstrated by the gpt_edge_capture_count hw_pending app.
 *
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
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_capture_period         = 0xFFFFFFFFUL, /**< GPT capture period.           */
  k_gpt_capture_poll_period_ms = 10U,          /**< GPT capture poll period ms.   */
  k_gpt_capture_min_delta_ms   = 50U,          /**< GPT capture minimum delta ms. */
} gpt_capture_const_t;

/**
 * @var g_gpt_capture_tick
 * @brief HIL liveness counter -- incremented every iteration where
 *        the GPT counter advanced relative to the previous sample
 *        (proves GTCNT is actually counting).
 *
 * @details Read by hil_jlink_memprobe.sh. SW1-button-driven input
 * capture is out-of-scope for automated HIL (would need a wire
 * loopback), so the gate just confirms the free-running counter
 * the demo uses for delta measurement is alive.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_capture_tick = 0U;

/** @brief Channel + LED selection. */
typedef enum : uint8_t {
  k_gpt_capture_channel = 0U, /**< GPT capture channel. */
} gpt_capture_layout_t;

/**
 * @brief Park the CPU after a fatal initialization failure.
 *
 * @details Repeatedly executes WFI, preserving the failed peripheral state
 *          for an attached debugger until reset.
 *
 * @pre Called only from the boot or terminal foreground path.
 * @pre The caller does not require this function to return.
 * @post The core remains in the WFI loop until external intervention.
 * @post No application data changes after entry.
 * @note Not thread-safe; this is a terminal single-threaded path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LEDs + SW1 + GPT0 up.
 *
 * @details Initializes the clock tree and delay service, configures LED1 and
 *          SW1, then starts GPT0 as a PCLKD/64 free-running counter. A failed
 *          prerequisite transfers control to the panic helper.
 *
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, GPT0 is counting and SW1/LED1 are ready for polling.
 * @post The delay service uses the measured CPU clock.
 * @note Not thread-safe; it owns the demo's peripheral configuration.
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
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_sw_init(k_ra8_board_sw1) != k_ra8_ok) {
    internal_panic_halt();
  }
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_64,
    .period     = (uint32_t)k_gpt_capture_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  if (ra8_gpt_init((uint8_t)k_gpt_capture_channel, &cfg) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Read current GPT counter ticks, or return UINT32_MAX on err.
 *
 * @details Delegates to the channel-zero HAL accessor and maps any HAL error
 *          onto a sentinel that the foreground liveness probe can ignore.
 *
 * @return Current free-running GPT count or the error sentinel.
 * @retval UINT32_MAX The counter read failed.
 * @retval 0..UINT32_MAX-1 A sampled counter value.
 *
 * @pre GPT0 was initialized and auto-started by ``internal_setup_or_halt``.
 * @pre No other context is reconfiguring GPT0 concurrently.
 * @post GPT0 remains running and unmodified.
 * @post The returned value is either a direct sample or ``UINT32_MAX``.
 * @note Not thread-safe with concurrent GPT reconfiguration.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_read(void)
{
  uint32_t val = 0U;
  if (ra8_gpt_read((uint8_t)k_gpt_capture_channel, &val) != k_ra8_ok) {
    return UINT32_MAX;
  }
  return val;
}

/**
 * @brief Detect a falling edge on SW1.
 *
 * @details Samples SW1 once, compares released-to-pressed state, and updates
 *          the caller-owned prior sample only when the board read succeeds.
 *
 * @par MC/DC:
 * Compound decision: ``prev == released && now == pressed``. Two
 * atomic conditions x N+1 = 3 vectors -- both true (edge), prev
 * pressed (no edge), now released (no edge). Companion host test
 * exercises each vector.
 *
 * @param[in,out] prev Last sampled state (updated on return).
 * @return true on the falling edge, false otherwise.
 * @retval true The sample changed from released to pressed.
 * @retval false No falling edge was observed or the board read failed.
 *
 * @pre ``prev`` points to writable caller-owned state.
 * @pre SW1 was initialized by ``internal_setup_or_halt``.
 * @post On a successful read, ``*prev`` contains the new switch state.
 * @post On a failed read, ``*prev`` is unchanged.
 * @note Not thread-safe when multiple callers share ``prev``.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_edge(ra8_board_sw_state_t* prev)
{
  ra8_board_sw_state_t now = k_ra8_board_sw_released;
  if (ra8_board_sw_read(k_ra8_board_sw1, &now) != k_ra8_ok) {
    return false;
  }
  bool fell = (*prev == k_ra8_board_sw_released) && (now == k_ra8_board_sw_pressed);
  *prev     = now;
  return fell;
}

/**
 * @brief Run the software input-capture and GPT liveness loop.
 *
 * @details Initializes the demo, tracks falling SW1 edges as counter deltas,
 *          and independently increments the exported liveness counter whenever
 *          successive GPT samples advance.
 *
 * @pre Reset startup and SystemInit completed successfully.
 * @pre The board wiring matches the EK-RA8D2 LED1 and SW1 definitions.
 * @post During operation, each detected edge toggles LED1.
 * @post A terminal control-flow escape parks the core in the panic helper.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  ra8_board_sw_state_t last_state = k_ra8_board_sw_released;
  uint32_t             last_tick  = 0U;
  uint32_t             prev_count = 0U;
  bool                 have_prev  = false;

  while (1) {
    if (internal_edge(&last_state)) {
      const uint32_t now        = internal_read();
      const uint32_t last_delta = now - last_tick;
      last_tick                 = now;
      (void)last_delta;
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }
    /* Liveness check independent of SW1 -- proves GTCNT is counting
     * even when the button isn't pressed (which it never is on the
     * unattended HIL bench). */
    const uint32_t now_count = internal_read();
    if (now_count != UINT32_MAX) {
      if (have_prev && (now_count != prev_count)) {
        g_gpt_capture_tick += 1U;
      }
      prev_count = now_count;
      have_prev  = true;
    }
    ra8_delay_ms(k_gpt_capture_poll_period_ms);
  }
  internal_panic_halt();
}
