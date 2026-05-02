/**
 * @file main.c
 * @brief SSIE I2S audio loopback demo for EK-RA8D2 (mic in -> speaker out)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4), routes the SSIE0 pins
 * (BCLK, LRCLK, AUDIO_MCK, SDI, SDO) per the EK-RA8D2 v1 audio jack
 * pinout, opens SSIE0 in master full-duplex I2S mode at 16-bit data
 * inside a 32-bit system word, and runs a polling loopback that
 * copies samples from the receive FIFO straight to the transmit
 * FIFO. Every 1000 samples processed the firmware prints
 * ``"audio: <N> samples processed\r\n"`` over the J-Link OB CDC
 * port (SCI8 @ 115200) so the host can see the loopback is alive.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init(cpuclk0_hz)`` for the diagnostic-print throttle.
 *   3. ``ra_pfs_route_peripheral()`` for SCI8 + the five SSIE0 pins.
 *   4. ``ra_sci_init(8, 115200 8N1)`` for diagnostic output.
 *   5. ``ra_mstp_init()`` then ``ra_ssie_init(0, &cfg)`` -- master
 *      mode, I2S, 16/32, AUDIO_MCK / 4 bit-clock, FIFO TX/RX
 *      thresholds at half-full.
 *   6. ``ra_ssie_start(0, k_ra_ssie_dir_tx_rx)`` to enable the
 *      full-duplex pump.
 *   7. Loop: poll ``ra_ssie_get_status``; on RX-full pop one sample
 *      and push it back to TX, increment the counter, and print
 *      every 1000 samples.
 *
 * Pin-mux notes: the EK-RA8D2 v1 silk-screen labels the audio
 * codec jacks as AUDIO IN / AUDIO OUT but the SSIE0 fan-out pin
 * assignment was not confirmed against the User's Manual at
 * authoring time. The placeholder pin macros in this file
 * (``k_audio_loopback_pin_*``) need a hardware bring-up pass
 * before live audio will pass through. The application still
 * compiles cleanly and exercises ``ra_ssie_init`` /
 * ``ra_ssie_start`` / ``ra_ssie_read_sample`` /
 * ``ra_ssie_write_sample`` end-to-end.
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_ssie.h"
#include "ra_time.h"

/**
 * @brief Compile-time settings for the audio loopback demo.
 *
 * @details
 * The print throttle is per-1000 samples, which at 48 kHz is one
 * line every ~21 ms. Lower it if the J-Link CDC backend cannot
 * keep up; raise it if the line spam is annoying.
 */
typedef enum : uint32_t {
  k_audio_loopback_baud         = 115200U,
  k_audio_loopback_print_period = 1000U,
} audio_loopback_config_t;

/** @brief uint8_t channel ids consumed by the SCI / SSIE drivers. */
typedef enum : uint8_t {
  k_audio_loopback_sci_channel    = 8U,
  k_audio_loopback_ssie_channel   = 0U,
  k_audio_loopback_tx_fifo_thresh = 4U, /**< SSISCR.TDES half-full.    */
  k_audio_loopback_rx_fifo_thresh = 4U, /**< SSISCR.RDFS half-full.    */
} audio_loopback_chan_t;

/**
 * @brief SSIE PSEL code from HUM Ch 20.4 "Peripheral I/O Table".
 *
 * @details
 * The SSIE BCLK / LRCLK / SDI / SDO / AUDIO_MCK pins all use PFS
 * PSEL = 0x0E. This is not yet exposed in ``ra_gpio_constants.h``;
 * a numeric cast keeps this app standalone until a shared constant
 * is added.
 */
typedef enum : uint8_t {
  k_audio_loopback_psel_ssie = 0x0EU,
} audio_loopback_psel_t;

/** @brief PD_02 / PD_03 -- on-board J-Link CDC SCI8 TXD / RXD. */
static const ra_port_pin_t k_audio_loopback_pin_txd = RA_PIN(k_ra_port_13, k_ra_pin_2);
static const ra_port_pin_t k_audio_loopback_pin_rxd = RA_PIN(k_ra_port_13, k_ra_pin_3);

/**
 * @brief Placeholder pin identifiers for the SSIE0 audio bus.
 *
 * @details
 * TODO: confirm against EK-RA8D2 v1 manual.
 *
 * The five SSIE0 pins (AUDIO_MCK, BCLK, LRCLK, SDI, SDO) need to be
 * verified against the EK-RA8D2 v1 audio-codec routing; until then
 * they point at five adjacent low-port pins so the validator does
 * not collide with the SCI8 / LED pins.
 */
static const ra_port_pin_t k_audio_loopback_pin_aud_mck = RA_PIN(k_ra_port_5, k_ra_pin_0);
static const ra_port_pin_t k_audio_loopback_pin_bclk    = RA_PIN(k_ra_port_5, k_ra_pin_1);
static const ra_port_pin_t k_audio_loopback_pin_lrclk   = RA_PIN(k_ra_port_5, k_ra_pin_2);
static const ra_port_pin_t k_audio_loopback_pin_sdi     = RA_PIN(k_ra_port_5, k_ra_pin_3);
static const ra_port_pin_t k_audio_loopback_pin_sdo     = RA_PIN(k_ra_port_5, k_ra_pin_4);

/** @brief Diagnostic banner emitted every k_audio_loopback_print_period samples. */
static const uint8_t k_audio_loopback_msg_prefix[] = "audio: ";
static const uint8_t k_audio_loopback_msg_suffix[] = " samples processed\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void audio_loopback_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route SCI8 + the five SSIE0 pins.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                       All pins routed.
 * @retval k_ra_err_gpio_invalid_port    Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin     Pin index out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success SCI8 + five SSIE0 pins are in their PSEL modes.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t audio_loopback_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_audio_loopback_pin_txd, k_ra_psel_sci_async, "audio_loopback.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  err =
    ra_pfs_route_peripheral(k_audio_loopback_pin_rxd, k_ra_psel_sci_async, "audio_loopback.rxd8");
  if (err != k_ra_ok) {
    return err;
  }
  /* 0x0E is a valid PFS PSEL code per HUM Ch 20.4 (SSIE), not yet exposed in
   * ra_psel_t. The NOLINT below silences the EnumCastOutOfRange diagnostic. */
  const ra_psel_t ssie_psel =
    (ra_psel_t)k_audio_loopback_psel_ssie; // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  err = ra_pfs_route_peripheral(k_audio_loopback_pin_aud_mck, ssie_psel, "audio_loopback.mck");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_audio_loopback_pin_bclk, ssie_psel, "audio_loopback.bclk");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_audio_loopback_pin_lrclk, ssie_psel, "audio_loopback.lrclk");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_audio_loopback_pin_sdi, ssie_psel, "audio_loopback.sdi");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_audio_loopback_pin_sdo, ssie_psel, "audio_loopback.sdo");
}

/**
 * @brief Bring CGC + SysTick + LED1 up. Halts on any fail.
 *
 * @details
 * Splits the long boot sequence so each step stays under the
 * NASA Power-of-10 60-line function-size cap. Returns the live
 * PCLKA Hz so the caller can hand it to ``ra_sci_init``.
 *
 * @param[out] out_pclka_hz Receives the live PCLKA rate.
 *
 * @pre out_pclka_hz is non-NULL.
 *
 * @post On success the CGC, SysTick, pin mux, and LED1 are live.
 * @post Halts in WFI on init failure.
 *
 * @since 0.1.0
 */
static void audio_loopback_init_clocks_and_led(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (audio_loopback_pins_init() != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
}

/**
 * @brief Open SCI8 at 115200 8N1 for diagnostic output.
 *
 * @param[in] pclka_hz Live PCLKA rate from ``ra_cgc_get_clock_hz``.
 *
 * @pre Clocks have been initialised.
 * @pre pclka_hz is non-zero.
 *
 * @post SCI8 is enabled.
 * @post Halts in WFI on init failure.
 *
 * @since 0.1.0
 */
static void audio_loopback_init_sci(uint32_t pclka_hz)
{
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_audio_loopback_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init(k_audio_loopback_sci_channel, &sci_cfg) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
}

/**
 * @brief Open SSIE0 in master full-duplex I2S mode and start.
 *
 * @details
 * SSIE0 is opened as a master full-duplex I2S device. AUDIO_MCK is
 * sourced from the AUCKE-driven internal divider (``enable_aucke
 * = true``). The bit-clock divider produces a per-bit clock from
 * AUDIO_MCK / 4. ``data_word = 16`` and ``system_word = 32`` is
 * the standard codec-friendly framing.
 *
 * @pre ``ra_mstp_init`` succeeded.
 * @pre Pin mux for SSIE0 has been established.
 *
 * @post SSIE0 has TEN | REN asserted and IRQ enables set.
 * @post Halts in WFI on init failure.
 *
 * @since 0.1.0
 */
static void audio_loopback_init_ssie(void)
{
  if (ra_mstp_init() != k_ra_ok) {
    audio_loopback_panic_halt();
  }

  const ra_ssie_cfg_t ssie_cfg = {
    .role          = k_ra_ssie_role_master,
    .format        = k_ra_ssie_format_i2s,
    .data_word     = k_ra_ssie_dwl_16,
    .system_word   = k_ra_ssie_swl_32,
    .bclk_div      = k_ra_ssie_bclk_div_4,
    .use_gpt_clk   = false,
    .long_frame    = false,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = true,
    .bck_idle_stop = false,
    .enable_aucke  = true,
    .tx_threshold  = k_audio_loopback_tx_fifo_thresh,
    .rx_threshold  = k_audio_loopback_rx_fifo_thresh,
  };
  if (ra_ssie_init(k_audio_loopback_ssie_channel, &ssie_cfg) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
  if (ra_ssie_start(k_audio_loopback_ssie_channel, k_ra_ssie_dir_tx_rx) != k_ra_ok) {
    audio_loopback_panic_halt();
  }
}

/**
 * @brief Convert a 32-bit unsigned integer into an ASCII decimal string.
 *
 * @param[in]  value Integer value to convert.
 * @param[out] buf   Caller buffer, must hold at least 11 bytes.
 * @return Number of bytes written into ``buf`` (does not include a NUL).
 *
 * @pre buf is non-NULL and has room for 11 bytes.
 *
 * @post Buffer holds the decimal representation, MSD first.
 *
 * @since 0.1.0
 */
static uint32_t audio_loopback_u32_to_dec(uint32_t value, uint8_t* buf)
{
  enum : uint8_t {
    k_ascii_zero = 0x30U, /**< '0'.                          */
    k_radix      = 10U,
    k_max_digits = 10U, /**< 2^32 - 1 has at most 10 digits. */
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
 * @brief Print "audio: <count> samples processed\\r\\n" over SCI8.
 *
 * @param[in] sample_count Cumulative sample count.
 *
 * @pre SCI8 is initialised.
 *
 * @post Three writes have been issued on SCI8.
 * @post No heap or dynamic allocations.
 *
 * @since 0.1.0
 */
static void audio_loopback_print_count(uint32_t sample_count)
{
  enum : uint8_t {
    k_dec_buf = 12U,
  };
  uint8_t  digits[k_dec_buf];
  uint32_t n = audio_loopback_u32_to_dec(sample_count, digits);

  (void)ra_sci_write_polling(k_audio_loopback_sci_channel,
                             k_audio_loopback_msg_prefix,
                             (uint32_t)(sizeof(k_audio_loopback_msg_prefix) - 1U));
  (void)ra_sci_write_polling(k_audio_loopback_sci_channel, digits, n);
  (void)ra_sci_write_polling(k_audio_loopback_sci_channel,
                             k_audio_loopback_msg_suffix,
                             (uint32_t)(sizeof(k_audio_loopback_msg_suffix) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + SSIE0 then runs loopback.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the loopback loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  audio_loopback_init_clocks_and_led(&pclka_hz);
  audio_loopback_init_sci(pclka_hz);
  audio_loopback_init_ssie();

  ra_isr_globals_enable();

  uint32_t sample_count = 0U;

  while (1) {
    ra_ssie_status_t status = {};
    if (ra_ssie_get_status(k_audio_loopback_ssie_channel, &status) != k_ra_ok) {
      break;
    }
    if (status.rx_count > 0U) {
      uint32_t sample = 0U;
      if (ra_ssie_read_sample(k_audio_loopback_ssie_channel, &sample) != k_ra_ok) {
        break;
      }
      if (ra_ssie_write_sample(k_audio_loopback_ssie_channel, sample) != k_ra_ok) {
        break;
      }
      sample_count++;
      if ((sample_count % k_audio_loopback_print_period) == 0U) {
        audio_loopback_print_count(sample_count);
        (void)ra_board_led_toggle(k_ra_board_led1);
      }
    }
    if (status.error) {
      (void)ra_ssie_start_recovery(k_audio_loopback_ssie_channel);
    }
  }

  audio_loopback_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
