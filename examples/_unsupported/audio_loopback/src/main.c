/**
 * @file examples/_unsupported/audio_loopback/src/main.c
 * @brief BSP-driven I2S audio playback demo for EK-RA8D2 (DA7212 CODEC)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4), then hands the entire
 * SSIE0 + DA7212 CODEC bring-up to the EK-RA8D2 BSP via
 * ``ra8_board_audio_init(48000, 16, 2)``. The BSP routes the CODEC
 * pins (P403/P404/P405/P406/PD06 + I2C control on P511/P512 per UM
 * Table 32 p 38), enables SSIE0 in I2S controller mode, and exposes
 * ``ra8_board_audio_play_sample_block`` for stereo PCM block writes.
 *
 * After init the firmware loops feeding a small repeating stereo
 * buffer to ``ra8_board_audio_play_sample_block``, prints
 * ``"audio: <N> blocks played\r\n"`` over the J-Link OB CDC port
 * (SCI8 @ 115200) every ``k_audio_loopback_print_period`` blocks,
 * and toggles LED1 once per log line.
 *
 * Sequence:
 *   1. ``ra8_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra8_time_init(cpuclk0_hz)`` for the diagnostic-print throttle.
 *   3. ``ra8_board_uart_console_init(115200)`` -- BSP routes PD02 TXD /
 *      PD03 RXD and opens SCI8 @ 115200 8N1 for diagnostic output.
 *   4. ``ra8_board_audio_init(48000, 16, 2)`` -- BSP wires SSIE0 +
 *      CODEC pins and starts the I2S controller.
 *   5. Loop: push the stereo sample block via
 *      ``ra8_board_audio_play_sample_block``, increment counter,
 *      log every ``k_audio_loopback_print_period`` blocks.
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
#include "ra8_isr.h"
#include "ra8_time.h"

/**
 * @brief Compile-time settings for the BSP-driven audio playback demo.
 *
 * @details
 * The print throttle is per-1000 blocks; at 48 kHz with 64 stereo
 * frames per block that is one log line every ~1.3 s.
 */
typedef enum : uint32_t {
  k_audio_loopback_baud         = 115200U, /**< Audio loopback baud.         */
  k_audio_loopback_print_period = 1000U,   /**< Audio loopback print period. */
  k_audio_loopback_sample_rate  = 48000U,  /**< Audio loopback sample rate.  */
} audio_loopback_config_t;

/** @brief uint8_t format ids consumed by the BSP audio path. */
typedef enum : uint8_t {
  k_audio_loopback_bit_depth = 16U, /**< Audio loopback bit depth. */
  k_audio_loopback_channels  = 2U,  /**< Audio loopback channels.  */
} audio_loopback_chan_t;

/** @brief Stereo block size in interleaved L/R 16-bit samples. */
typedef enum : uint32_t {
  k_audio_loopback_block_samples = 128U, /**< 64 stereo frames * 2 ch. */
} audio_loopback_block_t;

/** @brief Diagnostic banner emitted every k_audio_loopback_print_period blocks. */
static const uint8_t s_audio_loopback_msg_prefix[] = "audio: ";
static const uint8_t s_audio_loopback_msg_suffix[] = " blocks played\r\n";

/**
 * @brief Static stereo silence buffer fed to the CODEC each iteration.
 *
 * @details
 * Zero-amplitude PCM keeps the SSIE0 FIFO primed without driving
 * audible output. Replace with a sine LUT (or a pull from an iso-OUT
 * USB endpoint) to produce a tone.
 */
static const int16_t s_audio_loopback_silence[k_audio_loopback_block_samples] = {};

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @details Masks no additional interrupts; repeated WFI instructions keep the
 * core quiescent while preserving register state for a debugger.
 *
 * @pre Called only after a fatal error in boot.
 * @pre No recovery path remains for the current boot attempt.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No peripheral state is changed after entry.
 *
 * @note This helper deliberately does not attempt logging because the console
 * may be the peripheral whose initialization failed.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_loopback_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + LED1 up. Halts on any fail.
 *
 * @details
 * Splits the long boot sequence so each step stays under the
 * NASA Power-of-10 60-line function-size cap.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre Caller is single-threaded init context.
 *
 * @post On success the CGC, SysTick, and LED1 are live.
 * @post Halts in WFI on init failure.
 *
 * @note Initialization is deliberately fail-stop so playback never begins
 * with a partially configured clock or GPIO tree.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_loopback_init_clocks_and_led(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
}

/**
 * @brief Open the BSP J-Link console (SCI8 @ 115200 8N1, PD02/PD03).
 *
 * @details
 * Hands the SCI8 + PD02 TXD / PD03 RXD pin routing and baud setup to
 * the EK-RA8D2 BSP via ``ra8_board_uart_console_init``.
 *
 * @pre Clocks have been initialized.
 * @pre Caller is single-threaded init context.
 *
 * @post SCI8 is enabled for diagnostic output.
 * @post Halts in WFI on init failure.
 *
 * @note Console writes remain best-effort after this routine returns.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_loopback_init_console(void)
{
  if (ra8_board_uart_console_init((uint32_t)k_audio_loopback_baud) != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
}

/**
 * @brief Bring the on-board DA7212 CODEC + SSIE0 up via the BSP.
 *
 * @details
 * ``ra8_board_audio_init`` routes the CODEC pins (P403/P404/P405/P406/
 * PD06 per UM Table 32 p 38) and brings SSIE0 up in I2S controller
 * mode at the requested format. The application no longer touches
 * SSIE registers directly.
 *
 * @pre Pin mux table is unlocked (single-threaded init context).
 * @pre J41 jumpers populated (CODEC selected over camera).
 *
 * @post DA7212 + SSIE0 are live and ready for sample-block writes.
 * @post Halts in WFI on init failure.
 *
 * @note The BSP owns board-specific pin routing and codec control traffic.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_loopback_init_codec(void)
{
  const ra8_err_t err = ra8_board_audio_init(k_audio_loopback_sample_rate,
                                             (uint8_t)k_audio_loopback_bit_depth,
                                             (uint8_t)k_audio_loopback_channels);
  if (err != k_ra8_ok) {
    internal_audio_loopback_panic_halt();
  }
}

/**
 * @brief Convert a 32-bit unsigned integer into an ASCII decimal string.
 *
 * @details Builds digits least-significant first in a bounded local array,
 * then reverses them into the caller's output buffer without heap use.
 *
 * @param[in]  value Integer value to convert.
 * @param[out] buf   Caller buffer, must hold at least 11 bytes.
 * @return Number of bytes written into ``buf`` (does not include a NUL).
 * @retval 1 One byte was written for the value zero.
 * @retval 2..10 The number of decimal digits written for a nonzero value.
 *
 * @pre buf is non-NULL and has room for 11 bytes.
 * @pre The caller does not require a terminating NUL byte.
 *
 * @post Buffer holds the decimal representation, MSD first.
 * @post Bytes beyond the returned length are left unchanged.
 *
 * @note The 11-byte capacity precondition leaves room for all uint32_t values
 * even though this helper writes at most ten digits.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_audio_loopback_u32_to_dec(uint32_t value, uint8_t* buf)
{
  enum : uint8_t {
    k_ascii_zero = 0x30U, /**< '0'.                            */
    k_radix      = 10U,   /**< Radix.                          */
    k_max_digits = 10U,   /**< 2^32 - 1 has at most 10 digits. */
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
 * @brief Print "audio: <count> blocks played\\r\\n" over the BSP console.
 *
 * @details Formats the count into a bounded stack buffer and emits the prefix,
 * digits, and suffix as three allocation-free console writes.
 *
 * @param[in] block_count Cumulative block count.
 *
 * @pre The BSP console is initialized.
 * @pre ``block_count`` is the monotonic playback-loop counter snapshot.
 *
 * @post Three writes have been issued on the BSP console.
 * @post No heap or dynamic allocations.
 *
 * @note Individual write failures are intentionally ignored because this is
 * diagnostic output and must not stop audio playback.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_loopback_print_count(uint32_t block_count)
{
  enum : uint8_t {
    k_dec_buf = 12U, /**< Dec buffer. */
  };
  uint8_t  digits[k_dec_buf];
  uint32_t n = internal_audio_loopback_u32_to_dec(block_count, digits);

  (void)ra8_board_uart_console_write(s_audio_loopback_msg_prefix,
                                     (size_t)(sizeof(s_audio_loopback_msg_prefix) - 1U));
  (void)ra8_board_uart_console_write(digits, (size_t)n);
  (void)ra8_board_uart_console_write(s_audio_loopback_msg_suffix,
                                     (size_t)(sizeof(s_audio_loopback_msg_suffix) - 1U));
}

/**
 * @brief Application entry. Brings up CGC + BSP audio then plays blocks.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the playback loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_audio_loopback_init_clocks_and_led();
  internal_audio_loopback_init_console();
  internal_audio_loopback_init_codec();

  ra8_isr_globals_enable();

  uint32_t block_count = 0U;

  while (1) {
    if (ra8_board_audio_play_sample_block(s_audio_loopback_silence,
                                          k_audio_loopback_block_samples) != k_ra8_ok) {
      break;
    }
    block_count++;
    if ((block_count % k_audio_loopback_print_period) == 0U) {
      internal_audio_loopback_print_count(block_count);
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }
  }

  internal_audio_loopback_panic_halt();
}
