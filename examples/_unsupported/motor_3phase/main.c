/**
 * @file examples/_unsupported/motor_3phase/main.c
 * @brief Three-phase GPT PWM motor demo for EK-RA8D2 (GPT0/1/2 -> U/V/W)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, PCLKD = 125 MHz, SCICLK = PLL1R / 4),
 * routes the GPT0 / GPT1 / GPT2 GTIOCnA outputs onto a J-tag header
 * suitable for an external three-phase motor-driver IC, opens the
 * three channels as a phase-synchronized triple via
 * ``ra8_gpt_three_phase_open``, programmes a 1 us dead-time pair on
 * each channel, and runs a 2-second-period sine-table sweep that
 * smoothly rotates the three duty cycles 0..100 %. The duty values
 * are reported every 100 ms over the J-Link OB CDC port (SCI8 @
 * 115200 8N1).
 *
 * Sequence:
 *   1. ``ra8_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra8_time_init(cpuclk0_hz)`` for the slow update tick.
 *   3. ``ra8_pfs_route_peripheral()`` for the three GTIOC output
 *      pins. The GPT PSEL is ``k_ra8_psel_gpt0`` (0x03) for
 *      channels 0..3 and ``k_ra8_psel_gpt1`` (0x04) for higher-
 *      numbered channels per HUM Ch 20.4.
 *   4. ``ra8_board_uart_console_init(115200)`` for diagnostic output
 *      (J-Link OB console: SCI8 @ 115200 8N1, PD02 TXD / PD03 RXD).
 *   5. ``ra8_mstp_init()`` then ``ra8_gpt_three_phase_open(&cfg)``
 *      with U = GPT0, V = GPT1, W = GPT2, triangle-wave PWM,
 *      prescaler / 1, period = 0xFFFF (~512 us at 125 MHz PCLKD).
 *   6. ``ra8_gpt_dead_time_set(ch, 125, 125)`` per channel for
 *      ~1 us dead-time at PCLKD = 125 MHz.
 *   7. Initial test pattern: 50 % duty on all three phases via
 *      ``ra8_gpt_three_phase_set_duty(period/2, period/2, period/2)``.
 *   8. Loop:
 *        - Tick a 256-sample sine table at 8 kHz update rate so
 *          one full electrical revolution lands in 2 seconds
 *          (256 samples * 1 ms = 256 ms ... we extend the period
 *          via inner step counter).
 *        - Recompute U/V/W as 0..period scaled by
 *          ``(1 + sin(theta + phase))/2``.
 *        - ``ra8_gpt_three_phase_set_duty`` to push the new triple
 *          via the GTCCRC/E shadow buffers.
 *        - Every 100 ms: print "duty UVW <u> <v> <w>\\r\\n" on
 *          SCI8.
 *
 * Pin-mux notes: the J-tag connector pin allocation for an external
 * motor driver IC was not confirmed against the EK-RA8D2 v1 User's
 * Manual at authoring time; the placeholder pin macros in this
 * file (``s_motor_3phase_pin_u`` etc.) need a hardware bring-up
 * pass before live PWM will reach a motor driver. The application
 * still compiles cleanly and exercises ``ra8_gpt_init``,
 * ``ra8_gpt_three_phase_open``, ``ra8_gpt_pwm_pin_configure``,
 * ``ra8_gpt_dead_time_set``, and ``ra8_gpt_three_phase_set_duty``
 * end-to-end.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/**
 * @brief Compile-time settings for the three-phase motor demo.
 *
 * @details
 * - ``period`` controls the PWM carrier frequency. At PCLKD =
 *   125 MHz with prescaler / 1, period = 0xFFFF gives a carrier of
 *   ~1.91 kHz (62500000 / 32768) for triangle-wave PWM.
 * - ``dead_time`` is in PCLKD ticks. 125 ticks @ 125 MHz = 1 us.
 * - ``update_period_ms`` is the rate at which the sine-rotation
 *   advances. The 2-second mechanical revolution is implemented
 *   as 256 update steps spaced ``2000 / 256`` ~= 8 ms apart.
 */
typedef enum : uint32_t {
  k_motor_3phase_baud          = 115200U,      /**< Motor 3phase baud.          */
  k_motor_3phase_period        = 0x0000FFFFUL, /**< Motor 3phase period.        */
  k_motor_3phase_half_period   = 0x00007FFFUL, /**< Motor 3phase half period.   */
  k_motor_3phase_dead_time     = 125U,         /**< Motor 3phase dead time.     */
  k_motor_3phase_update_ms     = 8U,           /**< Motor 3phase update ms.     */
  k_motor_3phase_print_period  = 100U,         /**< Motor 3phase print period.  */
  k_motor_3phase_revolution_ms = 2000U,        /**< Motor 3phase revolution ms. */
} motor_3phase_config_t;

/**
 * @brief Channel assignments for U/V/W phases.
 *
 * @details
 * GPT0 -> U-phase (GTIOC0A). GPT1 -> V-phase (GTIOC1A). GPT2 ->
 * W-phase (GTIOC2A). The three channels share a common period and
 * are armed by the same GTSTR write inside
 * ``ra8_gpt_three_phase_open``.
 */
typedef enum : uint8_t {
  k_motor_3phase_ch_u = 0U, /**< Motor 3phase channel u. */
  k_motor_3phase_ch_v = 1U, /**< Motor 3phase channel v. */
  k_motor_3phase_ch_w = 2U, /**< Motor 3phase channel w. */
} motor_3phase_channel_t;

/**
 * @brief Sine LUT size and step bookkeeping.
 *
 * @details
 * 256-entry quarter-mirrored sine table is overkill for this demo's
 * audible carrier, but matches what FSP's motor-control example
 * uses. A single ``s_sine_step`` advances on each update tick.
 */
typedef enum : uint16_t {
  k_motor_3phase_sine_size = 256U, /**< Motor 3phase sine size.      */
  k_motor_3phase_sine_mask = 255U, /**< Motor 3phase sine mask.      */
  k_motor_3phase_phase_120 = 85U,  /**< 256 / 3 ~= 85.3 (truncated). */
  k_motor_3phase_phase_240 = 171U, /**< 2 * 256 / 3 ~= 170.6.        */
} motor_3phase_sine_t;

/**
 * @brief Placeholder pin identifiers for the three GTIOCnA outputs.
 *
 * @details
 * TODO: confirm against EK-RA8D2 v1 manual.
 *
 * The exact pad on the J-tag connector for each GTIOCnA depends on
 * the EK-RA8D2 v1 silk-screen layout; until that is verified, point
 * at three adjacent low-port pins so the validator does not collide
 * with the SCI8 / LED pins. Update once the manual table is read.
 */
// TODO(board-rev): no public GTIOC0A on EK-RA8D2 v1; pins conflict with debug UART per UM. Use P105/P103/Pmod-GTIOC10A or a custom carrier board.
static const ra8_port_pin_t s_motor_3phase_pin_u = RA8_PIN(k_ra8_port_4, k_ra8_pin_8);
static const ra8_port_pin_t s_motor_3phase_pin_v = RA8_PIN(k_ra8_port_4, k_ra8_pin_9);
static const ra8_port_pin_t s_motor_3phase_pin_w = RA8_PIN(k_ra8_port_4, k_ra8_pin_10);

/** @brief Diagnostic banner emitted every 100 ms. */
static const uint8_t s_motor_3phase_msg_prefix[] = "duty UVW ";
static const uint8_t s_motor_3phase_msg_sep[]    = " ";
static const uint8_t s_motor_3phase_msg_eol[]    = "\r\n";

/**
 * @brief Compile-time sine table, normalised to 0..period.
 *
 * @details
 * Built up at runtime in ``motor_3phase_setup_or_halt`` using a
 * fixed-point Bhaskara approximation so we avoid pulling in a
 * floating-point sin() implementation. The table holds duty
 * counts directly (0..k_motor_3phase_period), with an offset of
 * half-period so the wave swings symmetrically around the
 * 50%-duty centre line.
 */
static uint32_t s_motor_3phase_sine[k_motor_3phase_sine_size];

/** @brief Current sine-table phase index (advances each update tick). */
static uint16_t s_motor_3phase_step;

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @details Repeatedly enters the architectural wait state while retaining the
 * failed initialization context for debugger inspection.
 *
 * @pre Called only after a fatal error in boot.
 * @pre The application has no safe motor-control recovery path remaining.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No additional PWM or pin configuration is attempted.
 *
 * @note Hardware fault containment must already have disabled unsafe drive
 * outputs before this software-only halt is reached.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_motor_3phase_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Build the sine LUT using a Bhaskara I approximation.
 *
 * @details
 * Computes ``sin(theta)`` for ``theta = 2*pi*i/N`` using the
 * Bhaskara I formula in fixed-point so we never pull in libm. The
 * output is shifted into ``[0, period]`` so the values can be fed
 * directly to ``ra8_gpt_three_phase_set_duty``.
 *
 * The Bhaskara formula:
 *   sin(x) ~= 16 * x * (pi - x) / (5 * pi^2 - 4 * x * (pi - x))
 *   for x in [0, pi].
 *
 * For x in [pi, 2*pi] we reflect the result negative.
 *
 * @pre Called once at startup before any timer update.
 * @pre ``s_motor_3phase_sine`` is not being read by another context.
 *
 * @post ``s_motor_3phase_sine[i]`` holds duty counts for the
 *       sample at phase ``2*pi*i/N``.
 * @post Every generated duty value is clamped to the configured PWM period.
 *
 * @note The approximation is chosen for deterministic integer-only startup,
 * not for precision motor-control applications.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_motor_3phase_build_sine(void)
{
  /* Use a Q16 representation of pi and 2*pi for the Bhaskara math. */
  enum : uint64_t {
    k_q16_pi      = 205887ULL,  /**< pi  * 65536 ~= 205887. */
    k_q16_two_pi  = 411775ULL,  /**< 2pi * 65536 ~= 411775. */
    k_q16_5_pi_sq = 3236018ULL, /**< Q16 5 pi sq.           */
    k_q16_one     = 65536ULL,   /**< Q16 one.               */
    k_q16_numer_k = 16ULL,      /**< Q16 numer k.           */
    k_q16_denom_k = 4ULL,       /**< Q16 denom k.           */
  };
  const uint64_t period_u64  = k_motor_3phase_period;
  const uint64_t half_period = k_motor_3phase_half_period;

  for (uint16_t i = 0U; i < k_motor_3phase_sine_size; i++) {
    const uint64_t theta = ((uint64_t)i * k_q16_two_pi) / k_motor_3phase_sine_size;
    bool           neg   = false;
    uint64_t       x     = theta;
    if (x > k_q16_pi) {
      x   = x - k_q16_pi;
      neg = true;
    }
    const uint64_t pi_minus_x = k_q16_pi - x;
    const uint64_t numer      = k_q16_numer_k * x * pi_minus_x;
    const uint64_t denom_part = k_q16_denom_k * x * pi_minus_x;
    const uint64_t denom      = k_q16_5_pi_sq - denom_part;
    const uint64_t sin_q16    = (denom > 0ULL) ? (numer / denom) : 0ULL;
    const uint64_t scaled     = (sin_q16 * half_period) / k_q16_one;
    uint64_t       duty       = neg ? (half_period - scaled) : (half_period + scaled);
    if (duty > period_u64) {
      duty = period_u64;
    }
    s_motor_3phase_sine[i] = (uint32_t)duty;
  }
}

/**
 * @brief Route the three GTIOC pins.
 *
 * @details Claims and routes the U, V, and W pads in order, returning as soon
 * as a routing request fails so no later pad is touched after an error.
 *
 * @return Error code from the first failing route call, or k_ra8_ok.
 *
 * @retval k_ra8_ok                       All pins routed.
 * @retval k_ra8_err_gpio_invalid_port    Port index out of range.
 * @retval k_ra8_err_gpio_invalid_pin     Pin index out of range.
 * @retval k_ra8_err_gpio_conflict        Pin already claimed.
 *
 * @pre IOPORT module reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success the three GTIOC pins are routed.
 * @post On failure previously successful routes remain claimed.
 *
 * @note Callers treat partial routing as fatal and park until reset.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_motor_3phase_pins_init(void)
{
  ra8_err_t err =
    ra8_pfs_route_peripheral(s_motor_3phase_pin_u, k_ra8_psel_gpt0, "motor_3phase.gtioc0a");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(s_motor_3phase_pin_v, k_ra8_psel_gpt0, "motor_3phase.gtioc1a");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(s_motor_3phase_pin_w, k_ra8_psel_gpt0, "motor_3phase.gtioc2a");
}

/**
 * @brief Configure GTIOR for the three GTIOCnA outputs (active high).
 *
 * @details Applies one common output policy and dead-time pair to each of the
 * synchronized U/V/W channels, stopping at the first driver error.
 *
 * @return Error code from the first failing call, or k_ra8_ok.
 * @retval k_ra8_ok All three outputs and dead-time pairs were configured.
 * @retval k_ra8_err_invalid_arg A channel or timing argument was rejected.
 *
 * @pre ``ra8_gpt_three_phase_open`` already returned k_ra8_ok.
 * @pre IRQs masked.
 *
 * @post Each channel's GTIOR.OAE is set so the pin actually drives.
 * @post Each channel's polarity is active-high with stop-low.
 *
 * @note Partial configuration is not rolled back because the caller halts
 * before enabling normal motor updates.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_motor_3phase_pin_configure_all(void)
{
  enum : uint8_t {
    k_phase_count = 3U, /**< Phase count. */
  };
  const ra8_gpt_pwm_pin_cfg_t pin_cfg = {
    .output_enable    = true,
    .polarity         = k_ra8_gpt_pol_active_high,
    .stop_level       = k_ra8_gpt_stop_low,
    .disable_on_fault = k_ra8_gpt_disable_high_z,
  };
  const uint8_t channels[k_phase_count] = {
    k_motor_3phase_ch_u,
    k_motor_3phase_ch_v,
    k_motor_3phase_ch_w,
  };
  for (uint8_t i = 0U; i < k_phase_count; i++) {
    ra8_err_t err = ra8_gpt_pwm_pin_configure(channels[i], k_ra8_gpt_pin_a, &pin_cfg);
    if (err != k_ra8_ok) {
      return err;
    }
    err = ra8_gpt_dead_time_set(channels[i], k_motor_3phase_dead_time, k_motor_3phase_dead_time);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Bring CGC + SysTick + LED1 up. Halts on any fail.
 *
 * @details
 * Splits the long boot sequence so each step stays under the
 * NASA Power-of-10 60-line function-size cap.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre Peripheral interrupts remain masked during the boot sequence.
 *
 * @post On success the CGC, SysTick, pin mux, and LED1 are live.
 * @post Halts in WFI on init failure.
 *
 * @note Pin routing occurs only after MSTP and time services are available.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_motor_3phase_init_clocks_and_led(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (internal_motor_3phase_pins_init() != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
}

/**
 * @brief Open the J-Link OB console (SCI8 @ 115200 8N1).
 *
 * @details
 * Delegates SCI8 bring-up, PD02 TXD / PD03 RXD routing, and the baud
 * divisor to ``ra8_board_uart_console_init`` so the application carries
 * no board-specific console scaffolding.
 *
 * @pre Clocks initialized.
 * @pre ``ra8_mstp_init`` succeeded.
 *
 * @post The console SCI8 channel is enabled.
 * @post Halts in WFI on init failure.
 *
 * @note Diagnostic output is best-effort after initialization succeeds.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_motor_3phase_init_console(void)
{
  if (ra8_board_uart_console_init((uint32_t)k_motor_3phase_baud) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
}

/**
 * @brief Open the GPT three-phase block, configure pins, prime duty.
 *
 * @details
 * After ``ra8_gpt_three_phase_open`` arms U/V/W with a shared period,
 * the per-pin GTIOR is programmed for active-high output, stop-low,
 * and POEG-fault-Hi-Z, dead-time is set, and the test pattern
 * (50 % duty on every phase) is loaded.
 *
 * @pre ``ra8_mstp_init`` succeeded.
 * @pre Pin mux for the three GTIOC outputs is established.
 *
 * @post All three GPT channels are running synchronized.
 * @post Halts in WFI on init failure.
 *
 * @note The initial 50 percent pattern is loaded only after pin and dead-time
 * configuration have both succeeded.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_motor_3phase_init_pwm(void)
{
  /* MSTP enable is now performed in internal_motor_3phase_init_clocks_and_led so
   * the canonical CGC -> MSTP -> IOPORT -> TIME -> peripheral order is
   * preserved (see audit_init_order). */
  const ra8_gpt_three_phase_cfg_t three_phase_cfg = {
    .channels       = {k_motor_3phase_ch_u, k_motor_3phase_ch_v, k_motor_3phase_ch_w},
    .mode           = k_ra8_gpt_mode_triangle_pwm,
    .prescaler      = k_ra8_gpt_ps_div_1,
    .period_counts  = k_motor_3phase_period,
    .initial_duty_u = k_motor_3phase_half_period,
    .initial_duty_v = k_motor_3phase_half_period,
    .initial_duty_w = k_motor_3phase_half_period,
  };
  if (ra8_gpt_three_phase_open(&three_phase_cfg) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
  if (internal_motor_3phase_pin_configure_all() != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }

  internal_motor_3phase_build_sine();
  s_motor_3phase_step = 0U;

  /* Initial test pattern: 50 % duty on all three phases. */
  if (ra8_gpt_three_phase_set_duty(k_motor_3phase_half_period,
                                   k_motor_3phase_half_period,
                                   k_motor_3phase_half_period) != k_ra8_ok) {
    internal_motor_3phase_panic_halt();
  }
}

/**
 * @brief Convert a 32-bit unsigned integer into ASCII decimal.
 *
 * @details Accumulates decimal digits in reverse order in a bounded local
 * array, then copies them most-significant digit first into ``buf``.
 *
 * @param[in]  value Integer value.
 * @param[out] buf   Caller buffer, must hold at least 11 bytes.
 * @return Number of bytes written into ``buf``.
 * @retval 1 One byte was written for zero.
 * @retval 2..10 The decimal digit count for a nonzero uint32_t value.
 *
 * @pre buf is non-NULL with room for 11 bytes.
 * @pre The caller tracks the returned length rather than expecting a NUL.
 *
 * @post Buffer holds the decimal MSD-first representation.
 * @post Bytes after the returned digit sequence remain unchanged.
 *
 * @note This formatter is intentionally independent of the C stdio library.
 *
 *
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_motor_3phase_u32_to_dec(uint32_t value, uint8_t* buf)
{
  enum : uint8_t {
    k_ascii_zero = 0x30U, /**< Ascii zero.     */
    k_radix      = 10U,   /**< Radix.          */
    k_max_digits = 10U,   /**< Maximum digits. */
  };
  uint8_t  tmp[k_max_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[0] = k_ascii_zero;
    return 1U;
  }
  while (value > 0U) {
    tmp[n] = (uint8_t)(k_ascii_zero + (uint8_t)(value % k_radix));
    n++;
    value /= k_radix;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Print "duty UVW <u> <v> <w>\\r\\n" over SCI8.
 *
 * @details Formats each phase count into one reusable bounded buffer and emits
 * the fixed separators with allocation-free BSP console writes.
 *
 * @param[in] u_duty U-phase compare value.
 * @param[in] v_duty V-phase compare value.
 * @param[in] w_duty W-phase compare value.
 *
 * @pre SCI8 is initialized.
 * @pre Each duty argument is within the configured GPT period.
 *
 * @post Seven writes have been issued on SCI8.
 * @post No heap or dynamic allocations.
 *
 * @note Write failures are ignored because diagnostic loss must not perturb
 * the real-time duty update loop.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_motor_3phase_print_duty(uint32_t u_duty, uint32_t v_duty, uint32_t w_duty)
{
  enum : uint8_t {
    k_dec_buf = 12U, /**< Dec buffer. */
  };
  uint8_t  digits[k_dec_buf];
  uint32_t n = 0U;

  (void)ra8_board_uart_console_write(s_motor_3phase_msg_prefix,
                                     (size_t)(sizeof(s_motor_3phase_msg_prefix) - 1U));
  n = internal_motor_3phase_u32_to_dec(u_duty, digits);
  (void)ra8_board_uart_console_write(digits, (size_t)n);
  (void)ra8_board_uart_console_write(s_motor_3phase_msg_sep,
                                     (size_t)(sizeof(s_motor_3phase_msg_sep) - 1U));
  n = internal_motor_3phase_u32_to_dec(v_duty, digits);
  (void)ra8_board_uart_console_write(digits, (size_t)n);
  (void)ra8_board_uart_console_write(s_motor_3phase_msg_sep,
                                     (size_t)(sizeof(s_motor_3phase_msg_sep) - 1U));
  n = internal_motor_3phase_u32_to_dec(w_duty, digits);
  (void)ra8_board_uart_console_write(digits, (size_t)n);
  (void)ra8_board_uart_console_write(s_motor_3phase_msg_eol,
                                     (size_t)(sizeof(s_motor_3phase_msg_eol) - 1U));
}

/**
 * @brief Advance the U/V/W phase by one update tick.
 *
 * @details Reads three table entries separated by approximately 120 degrees,
 * returns their duties, then advances the shared phase modulo 256.
 *
 * @param[out] out_u Pointer that receives the new U-phase compare value.
 * @param[out] out_v Pointer that receives the new V-phase compare value.
 * @param[out] out_w Pointer that receives the new W-phase compare value.
 *
 * @pre out_u / out_v / out_w are non-NULL.
 * @pre The sine table has been initialized and is not concurrently modified.
 *
 * @post ``s_motor_3phase_step`` advanced by one (mod sine-table size).
 * @post ``*out_u``, ``*out_v``, ``*out_w`` are valid duty counts in
 *       ``[0, k_motor_3phase_period]``.
 *
 * @note The integer phase offsets intentionally approximate one third and two
 * thirds of the 256-entry table.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_motor_3phase_advance(uint32_t* out_u, uint32_t* out_v, uint32_t* out_w)
{
  const uint16_t i_u = s_motor_3phase_step;
  const uint16_t i_v =
    (uint16_t)((s_motor_3phase_step + k_motor_3phase_phase_120) & k_motor_3phase_sine_mask);
  const uint16_t i_w =
    (uint16_t)((s_motor_3phase_step + k_motor_3phase_phase_240) & k_motor_3phase_sine_mask);
  *out_u              = s_motor_3phase_sine[i_u];
  *out_v              = s_motor_3phase_sine[i_v];
  *out_w              = s_motor_3phase_sine[i_w];
  s_motor_3phase_step = (uint16_t)((s_motor_3phase_step + 1U) & k_motor_3phase_sine_mask);
}

/**
 * @brief Application entry. Brings up CGC + GPT triple, runs sweep.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the sweep loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_motor_3phase_init_clocks_and_led();
  internal_motor_3phase_init_console();
  internal_motor_3phase_init_pwm();

  ra8_isr_globals_enable();

  uint32_t print_accum_ms = 0U;

  while (1) {
    uint32_t u_duty = 0U;
    uint32_t v_duty = 0U;
    uint32_t w_duty = 0U;

    internal_motor_3phase_advance(&u_duty, &v_duty, &w_duty);

    if (ra8_gpt_three_phase_set_duty(u_duty, v_duty, w_duty) != k_ra8_ok) {
      break;
    }

    print_accum_ms += k_motor_3phase_update_ms;
    if (print_accum_ms >= k_motor_3phase_print_period) {
      print_accum_ms = 0U;
      internal_motor_3phase_print_duty(u_duty, v_duty, w_duty);
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }

    ra8_delay_ms(k_motor_3phase_update_ms);
  }

  internal_motor_3phase_panic_halt();
}
