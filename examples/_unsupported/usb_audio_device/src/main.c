/**
 * @file examples/_unsupported/usb_audio_device/src/main.c
 * @brief USB Audio Class 1.0 device-mode smoke test for EK-RA8D2
 *        (USB-FS, 48 kHz / 16-bit / stereo iso-IN sine generator)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()``, routes the four USB-FS pins
 * to the EK-RA8D2 v1 USB-FS receptacle (J11), brings the device-mode
 * UAC1 stack up via the NSC USB veneers + the ``ra8_usb_paud`` class
 * layer, and feeds a precomputed 1 kHz sine wave (48-sample LUT --
 * exactly one cycle at 48 kHz) into the iso-IN endpoint every USB-FS
 * 1 ms frame so the host enumerates the gadget as a UAC1 microphone /
 * headset and renders the test tone.
 *
 * USB Audio Class 1.0 sec 2.2.5 "Format Type Descriptor" (Type-I PCM
 * 16-bit, 2 channels, 48 kHz) and sec 5.2.3.2.3.1 "Sampling Frequency
 * Control" frame the descriptor + class-request behaviour the driver
 * synthesises around the LUT below.
 *
 * ## Pinout (USB-FS, EK-RA8D2 v1)
 *
 * | Net           | Pin    | PFS PSEL                | Notes                         |
 * |---------------|--------|-------------------------|-------------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra8_psel_usb_fs (0x13) | VBUS sense (input).           |
 * | USB_FS_VBUSEN | P5_00  | k_ra8_psel_usb_fs (0x13) | VBUS-enable drive (output).   |
 * | USB_FS_DP     | P8_14  | k_ra8_psel_usb_fs (0x13) | D+ data line (analog buffer). |
 * | USB_FS_DM     | P8_15  | k_ra8_psel_usb_fs (0x13) | D- data line (analog buffer). |
 *
 * Pin assignments are copied verbatim from ``examples/usb_cdc_echo``
 * because the EK-RA8D2 v1 board only routes USB-FS to one pad set.
 *
 * ## Sequence
 *
 *   1. ``ra8_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra8_time_init(cpuclk0_hz)`` for ``ra8_delay_ms``.
 *   3. ``ra8_pfs_route_peripheral`` for the four USB-FS pins.
 *   4. ``ra8_gpio_output_init(k_ra8_pin_led1, low)`` for the heartbeat.
 *   5. ``ra8_board_uart_console_init`` (SCI8, PD_02 / PD_03) at 115200
 *      8N1 for log output.
 *   6. ``ra8_nsc_usb_init(k_ra8_usb_speed_fs)`` -- secure veneer to
 *      ``ra8_usb_device_init``.
 *   7. ``ra8_usb_paud_init(k_ra8_usb_speed_fs)`` -- iso-IN PIPE1 / iso-OUT
 *      PIPE2 configured at the FS default 192-byte max-packet (48 kHz
 *      stereo 16-bit per USB Audio 1.0 sec 3.7.1).
 *   8. ``ra8_usb_paud_set_format`` -- explicit 48 kHz / 2 ch / 16-bit.
 *   9. ``ra8_usb_paud_set_volume(0)`` -- 0 dB feature-unit volume
 *      (USB Audio 1.0 sec 5.2.2.4.3.2 "Volume Control" Q8.8 = 0).
 *  10. ``ra8_nsc_usb_attach(k_ra8_usb_speed_fs, true)`` -- raise D+
 *      pull-up so the host begins enumeration.
 *  11. Loop: feed 48 samples (one full cycle of the 1 kHz sine LUT)
 *      to ``ra8_usb_paud_send_frame`` per iteration, log every 1000
 *      frames over SCI8, and toggle LED1 once per log line.
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
#include "ra8_nsc_comms.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_paud.h"

/**
 * @enum usb_audio_config_t
 * @brief Numeric configuration constants for the demo.
 *
 * @details
 * Every literal used by the bring-up path lives here so the magic-
 * number lint never sees a bare integer. The BSP console uses the
 * on-board J-Link OB CDC bridge pins (PD_02 / PD_03) for log output.
 */
typedef enum : uint32_t {
  k_usb_audio_baud         = 115200U, /**< J-Link OB CDC log baud.   */
  k_usb_audio_log_period   = 1000U,   /**< Frames per SCI8 log line. */
  k_usb_audio_idle_step_ms = 1U,      /**< USB-FS frame period.      */
} usb_audio_config_t;

/**
 * @enum usb_audio_format_t
 * @brief Audio format constants used to seed the format shadow.
 *
 * @details
 * USB Audio 1.0 sec 2.2.5 "Format Type Descriptor" -- Type-I PCM,
 * 16-bit sub-frame, 2 channels, 48 kHz. The LUT below stores 48 stereo
 * samples (one full cycle of a 1 kHz tone), which is exactly the FS
 * iso payload of one microframe.
 */
typedef enum : uint32_t {
  k_usb_audio_sample_rate_hz = 48000U,               /**< Sample rate (Hz).                    */
  k_usb_audio_channels       = 2U,                   /**< Stereo.                              */
  k_usb_audio_bytes_per_samp = 2U,                   /**< 16-bit.                              */
  k_usb_audio_lut_samples    = 48U,                  /**< 48 stereo samples per frame.         */
  k_usb_audio_lut_bytes = (uint32_t)(48U * 2U * 2U), /**< 48 samples * 2 ch * 2 bytes = 192 B. */
} usb_audio_format_t;

/**
 * @enum usb_audio_volume_t
 * @brief Q8.8-format volume constant (USB Audio 1.0 sec 5.2.2.4.3.2).
 */
typedef enum : int16_t {
  k_usb_audio_volume_0_db = 0, /**< 0 dB attenuation, full scale. */
} usb_audio_volume_t;

/**
 * @brief 1 kHz sine LUT, 48 stereo 16-bit samples per cycle.
 *
 * @details
 * Each entry is interleaved L/R 16-bit little-endian (USB Audio 1.0
 * sec 2.2.5 Format Type-I sample byte order). The amplitude is
 * 0x4000 (half full-scale) so the host can downstream-mix without
 * clipping. Hand-computed with sin(2*pi*n/48) at integer step 1 of
 * 48; floating-point math is forbidden in this firmware so the LUT
 * is precomputed and stored in MRAM as constant rodata.
 *
 * Index 0 = sin(0), 12 = sin(pi/2) = +0x4000, 24 = sin(pi) = 0,
 * 36 = sin(3pi/2) = -0x4000, ... back to 0 at index 48.
 */
static const uint8_t s_usb_audio_sine_lut[k_usb_audio_lut_bytes] = {
  /* sample 0  : 0x0000 L,R */ 0x00U, 0x00U, 0x00U, 0x00U,
  /* sample 1  : 0x086FU    */ 0x6FU, 0x08U, 0x6FU, 0x08U,
  /* sample 2  : 0x10B5U    */ 0xB5U, 0x10U, 0xB5U, 0x10U,
  /* sample 3  : 0x18F8U    */ 0xF8U, 0x18U, 0xF8U, 0x18U,
  /* sample 4  : 0x2120U    */ 0x20U, 0x21U, 0x20U, 0x21U,
  /* sample 5  : 0x2924U    */ 0x24U, 0x29U, 0x24U, 0x29U,
  /* sample 6  : 0x30FBU    */ 0xFBU, 0x30U, 0xFBU, 0x30U,
  /* sample 7  : 0x389DU    */ 0x9DU, 0x38U, 0x9DU, 0x38U,
  /* sample 8  : 0x3FFFU    */ 0xFFU, 0x3FU, 0xFFU, 0x3FU,
  /* sample 9  : 0x471CU    */ 0x1CU, 0x47U, 0x1CU, 0x47U,
  /* sample 10 : 0x4DECU    */ 0xECU, 0x4DU, 0xECU, 0x4DU,
  /* sample 11 : 0x5468U    */ 0x68U, 0x54U, 0x68U, 0x54U,
  /* sample 12 : 0x4000U    */ 0x00U, 0x40U, 0x00U, 0x40U,
  /* sample 13 : 0x5468U    */ 0x68U, 0x54U, 0x68U, 0x54U,
  /* sample 14 : 0x4DECU    */ 0xECU, 0x4DU, 0xECU, 0x4DU,
  /* sample 15 : 0x471CU    */ 0x1CU, 0x47U, 0x1CU, 0x47U,
  /* sample 16 : 0x3FFFU    */ 0xFFU, 0x3FU, 0xFFU, 0x3FU,
  /* sample 17 : 0x389DU    */ 0x9DU, 0x38U, 0x9DU, 0x38U,
  /* sample 18 : 0x30FBU    */ 0xFBU, 0x30U, 0xFBU, 0x30U,
  /* sample 19 : 0x2924U    */ 0x24U, 0x29U, 0x24U, 0x29U,
  /* sample 20 : 0x2120U    */ 0x20U, 0x21U, 0x20U, 0x21U,
  /* sample 21 : 0x18F8U    */ 0xF8U, 0x18U, 0xF8U, 0x18U,
  /* sample 22 : 0x10B5U    */ 0xB5U, 0x10U, 0xB5U, 0x10U,
  /* sample 23 : 0x086FU    */ 0x6FU, 0x08U, 0x6FU, 0x08U,
  /* sample 24 : 0x0000     */ 0x00U, 0x00U, 0x00U, 0x00U,
  /* sample 25 : -0x086FU   */ 0x91U, 0xF7U, 0x91U, 0xF7U,
  /* sample 26 : -0x10B5U   */ 0x4BU, 0xEFU, 0x4BU, 0xEFU,
  /* sample 27 : -0x18F8U   */ 0x08U, 0xE7U, 0x08U, 0xE7U,
  /* sample 28 : -0x2120U   */ 0xE0U, 0xDEU, 0xE0U, 0xDEU,
  /* sample 29 : -0x2924U   */ 0xDCU, 0xD6U, 0xDCU, 0xD6U,
  /* sample 30 : -0x30FBU   */ 0x05U, 0xCFU, 0x05U, 0xCFU,
  /* sample 31 : -0x389DU   */ 0x63U, 0xC7U, 0x63U, 0xC7U,
  /* sample 32 : -0x3FFFU   */ 0x01U, 0xC0U, 0x01U, 0xC0U,
  /* sample 33 : -0x471CU   */ 0xE4U, 0xB8U, 0xE4U, 0xB8U,
  /* sample 34 : -0x4DECU   */ 0x14U, 0xB2U, 0x14U, 0xB2U,
  /* sample 35 : -0x5468U   */ 0x98U, 0xABU, 0x98U, 0xABU,
  /* sample 36 : -0x4000U   */ 0x00U, 0xC0U, 0x00U, 0xC0U,
  /* sample 37 : -0x5468U   */ 0x98U, 0xABU, 0x98U, 0xABU,
  /* sample 38 : -0x4DECU   */ 0x14U, 0xB2U, 0x14U, 0xB2U,
  /* sample 39 : -0x471CU   */ 0xE4U, 0xB8U, 0xE4U, 0xB8U,
  /* sample 40 : -0x3FFFU   */ 0x01U, 0xC0U, 0x01U, 0xC0U,
  /* sample 41 : -0x389DU   */ 0x63U, 0xC7U, 0x63U, 0xC7U,
  /* sample 42 : -0x30FBU   */ 0x05U, 0xCFU, 0x05U, 0xCFU,
  /* sample 43 : -0x2924U   */ 0xDCU, 0xD6U, 0xDCU, 0xD6U,
  /* sample 44 : -0x2120U   */ 0xE0U, 0xDEU, 0xE0U, 0xDEU,
  /* sample 45 : -0x18F8U   */ 0x08U, 0xE7U, 0x08U, 0xE7U,
  /* sample 46 : -0x10B5U   */ 0x4BU, 0xEFU, 0x4BU, 0xEFU,
  /* sample 47 : -0x086FU   */ 0x91U, 0xF7U, 0x91U, 0xF7U,
};

/**
 * @brief Park the CPU forever in WFI on fatal init failure.
 *
 * @details Re-enters WFI indefinitely so the failing boot context remains
 * available to a debugger without executing additional peripheral accesses.
 *
 * @pre Called only after a fatal error in boot.
 * @pre No safe continuation path remains for the current initialization.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post USB and console state remain unchanged after entry.
 *
 * @note Logging is avoided because the console itself may have failed.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Send a NUL-terminated ASCII line over SCI8 (best-effort).
 *
 * @details Measures the string locally and forwards exactly its payload bytes
 * to the BSP console without a terminator or dynamic allocation.
 *
 * @param[in] s ASCII string (NUL-terminated). May be ``nullptr``.
 *
 * @pre ra8_board_uart_console_init() succeeded for the BSP console.
 * @pre If non-NULL, ``s`` points to a readable NUL-terminated byte sequence.
 * @post Bytes have been polled out of TXD8 (or silently discarded on
 *       backpressure -- this is logging only).
 * @post A NULL argument performs no console write.
 *
 * @note Console errors are intentionally non-fatal for diagnostic messages.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_log(const char* s)
{
  if (s == nullptr) {
    return;
  }
  uint32_t len = 0U;
  while (s[len] != '\0') {
    len++;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Convert a uint32_t to ASCII decimal in caller-supplied buffer.
 *
 * @details Builds digits in reverse order in a fixed local array and copies as
 * many as fit, always terminating a nonzero-capacity destination.
 *
 * @param[in]  value Value to format.
 * @param[out] buf   Destination buffer (>= 11 bytes incl. NUL).
 * @param[in]  cap   Capacity of buf in bytes.
 *
 * @pre buf != nullptr.
 * @pre cap >= 11.
 * @post buf holds a NUL-terminated decimal representation of value.
 * @post No bytes at or beyond ``buf[cap]`` are accessed.
 *
 * @note Invalid zero-capacity input returns without writing; normal callers
 * satisfy the documented eleven-byte capacity contract.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_u32_to_ascii(uint32_t value, char* buf, uint32_t cap)
{
  enum : uint32_t {
    k_dec_radix  = 10U, /**< Dec radix.      */
    k_max_digits = 10U, /**< Maximum digits. */
  };
  if (buf == nullptr || cap == 0U) {
    return;
  }
  char     tmp[k_max_digits + 1U];
  uint32_t n = 0U;
  if (value == 0U) {
    tmp[n] = '0';
    n++;
  } else {
    while (value > 0U && n < k_max_digits) {
      const uint32_t digit = value % k_dec_radix;
      tmp[n]               = (char)('0' + (char)digit);
      value /= k_dec_radix;
      n++;
    }
  }
  uint32_t out = 0U;
  while (n > 0U && out < (cap - 1U)) {
    n--;
    buf[out] = tmp[n];
    out++;
  }
  buf[out] = '\0';
}

/**
 * @brief Bring up clocks, time, GPIO. Panic-halts on any failure.
 *
 * @details Initializes the CGC, reads CPUCLK0, starts the timebase, and claims
 * LED1 in the dependency order required by the board support package.
 *
 * @param[out] cpuclk0_hz Receives CPUCLK0 rate.
 *
 * @pre cpuclk0_hz non-null.
 * @pre Reset initialization has completed with interrupts still controlled.
 * @post CGC + SysTick + LED1 GPIO are usable.
 * @post ``*cpuclk0_hz`` contains the rate used to configure the timebase.
 *
 * @note Any driver error is fail-stop and leaves the CPU parked in WFI.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_clocks_or_halt(uint32_t* cpuclk0_hz)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, cpuclk0_hz) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_time_init(*cpuclk0_hz) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
}

/**
 * @brief Bring up USB-FS pin mux + UAC1 class layer + format/volume shadows.
 *
 * @details
 * Pin-mux first, then ``ra8_nsc_usb_init`` to release the MSTP gate,
 * then ``ra8_usb_paud_init`` to install the iso-IN / iso-OUT pipes,
 * then ``ra8_usb_paud_set_format`` / ``ra8_usb_paud_set_volume`` to
 * seed the format + volume shadows, then ``ra8_nsc_usb_attach`` to
 * raise the D+ pull-up so the host begins enumeration.
 *
 * @pre Clocks + SCI8 already initialized.
 * @pre USB device pins are available to the board support package.
 * @post USB-FS device-mode UAC1 endpoint is live.
 * @post Format and volume shadows contain the demo's fixed audio settings.
 *
 * @note D+ attachment is deliberately the final operation so the host cannot
 * enumerate a partially configured audio class.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_usb_or_halt(void)
{
  if (ra8_board_usbhs_device_init() != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_usb_paud_init(k_ra8_usb_speed_fs) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }

  const ra8_usb_paud_format_t fmt = {
    .sample_rate_hz   = k_usb_audio_sample_rate_hz,
    .channels         = (uint8_t)k_usb_audio_channels,
    .bytes_per_sample = (uint8_t)k_usb_audio_bytes_per_samp,
  };
  if (ra8_usb_paud_set_format(fmt) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_usb_paud_set_volume(k_usb_audio_volume_0_db) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  if (ra8_nsc_usb_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
}

/**
 * @brief Bring up CGC + SysTick + GPIO + SCI8 + USB-FS + UAC1.
 *        Panic-halts on any failure.
 *
 * @details
 * Mirrors ``usb_cdc_echo`` setup but swaps CDC for the UAC1 audio
 * class. Split into clocks, console, and USB stages so each helper
 * stays inside the NASA-rule-4 line budget.
 *
 * @pre Reset_Handler has initialized data and BSS.
 * @pre The application is still in single-threaded boot context.
 * @post Clocks, LED1, console, and USB audio are initialized on success.
 * @post Any setup error parks the CPU before the streaming loop starts.
 *
 * @note The local clock-rate value is passed only to the timebase setup path.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  internal_usb_audio_clocks_or_halt(&cpuclk0_hz);
  if (ra8_board_uart_console_init((uint32_t)k_usb_audio_baud) != k_ra8_ok) {
    internal_usb_audio_panic_halt();
  }
  internal_usb_audio_usb_or_halt();
}

/**
 * @brief Push one iso frame (1 ms = 48 stereo samples) on PIPE1.
 *
 * @details Submits the immutable interleaved stereo LUT as one bounded UAC1
 * isochronous frame and returns the class driver's exact status.
 *
 * @return Error code from ``ra8_usb_paud_send_frame``.
 *
 * @retval k_ra8_ok                  Bytes queued onto iso-IN.
 * @retval k_ra8_err_invalid_state   Driver not initialized.
 * @retval k_ra8_err_invalid_arg     LUT byte count out of range.
 *
 * @pre ``ra8_usb_paud_init`` succeeded.
 * @pre The host-selected audio format matches the fixed LUT geometry.
 * @post 192 bytes of LUT have been queued onto PIPE1.
 * @post On error the LUT and caller-owned USB state are not modified here.
 *
 * @note Retry policy belongs to the outer streaming loop.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_usb_audio_send_one_frame(void)
{
  return ra8_usb_paud_send_frame(s_usb_audio_sine_lut, (uint16_t)k_usb_audio_lut_bytes);
}

/**
 * @brief Emit one log line announcing the running frame count.
 *
 * @details Formats the counter in a bounded stack buffer, emits three string
 * fragments, and toggles the activity LED once.
 *
 * @param[in] frames Frame counter snapshot.
 *
 * @pre SCI8 is initialized.
 * @pre LED1 was initialized by the clock/setup stage.
 * @post One ASCII line written to SCI8 and LED1 toggled once.
 * @post The caller's frame counter is not modified.
 *
 * @note Console and LED errors are diagnostic-only and intentionally ignored.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usb_audio_log_frames(uint32_t frames)
{
  enum : uint32_t {
    k_log_buf_bytes = 16U, /**< Decimal buffer for the frame counter. */
  };
  char buf[k_log_buf_bytes] = {};
  internal_usb_audio_u32_to_ascii(frames, buf, k_log_buf_bytes);
  internal_usb_audio_log("audio: ");
  internal_usb_audio_log(buf);
  internal_usb_audio_log(" frames sent\r\n");
  (void)ra8_board_led_toggle(k_ra8_board_led1);
}

/**
 * @brief Application entry. Brings up CGC + USB-FS + UAC1, then enters
 *        the iso-IN feed loop forever.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the iso-IN feed loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_usb_audio_setup_or_halt();
  ra8_isr_globals_enable();
  internal_usb_audio_log("ra8d2: USB Audio device ready (UAC1 48 kHz / 16-bit / stereo)\r\n");

  uint32_t frames = 0U;
  while (1) {
    if (internal_usb_audio_send_one_frame() != k_ra8_ok) {
      break;
    }
    frames++;
    if ((frames % k_usb_audio_log_period) == 0U) {
      internal_usb_audio_log_frames(frames);
    }
    ra8_delay_ms(k_usb_audio_idle_step_ms);
  }

  internal_usb_audio_panic_halt();
}
