/**
 * @file examples/ek_ra8d2/hw_pending/da16600_probe/main.c
 * @brief DA16600 Wi-Fi + BLE bring-up ladder over Pmod1 SCI2 (plain NS app)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Hardware bring-up probe for the US159-DA16600EVZ Pmod in Pmod1 (J26).
 * Climbs the validation ladder rung by rung and reports each result on
 * the J-Link OB VCOM (SCI8):
 *
 *   1. ``AT`` probe          -> ``da16600: alive``
 *   2. ``AT+WFSCAN``         -> ``wifi: scan N=<count>``
 *   3. BLE advertise on/off  -> ``ble: adv ok``
 *
 * Wi-Fi join + TCP echo are exercised by the follow-up app once an AP
 * fixture is on the air; this probe needs no infrastructure.
 *
 * The DA16600 talks AT commands over a 3.3 V UART at 115200 8N1 with no
 * flow control (UM-WI-046 section 2.1). Pmod1 routes the module to SCI
 * channel 2: TXD2 = P801, RXD2 = P802 (EK-RA8D2 v1 UM Table 17 p 26;
 * cross-checked against HUM Table 20.15 PORT8 at PSEL 00100b).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_port_regs.h"
#include "ra8d2_sci_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_da16600.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_i2c.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_pin_validator.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum da16600_probe_const_t
 * @brief Compile-time settings for the bring-up ladder.
 */
typedef enum : uint32_t {
  k_probe_decimal_base    = 10U,     /**< Radix for integer-to-ASCII.          */
  k_probe_u16_max_digits  = 5U,      /**< Max decimal digits in a uint16_t.    */
  k_probe_u16_str_len     = 6U,      /**< Digits + NUL for a uint16_t string.  */
  k_probe_hex_str_len     = 5U,      /**< 4 hex digits + NUL.                  */
  k_probe_hex_nibble      = 4U,      /**< Bits per hex digit.                  */
  k_probe_hex_mask        = 0xFU,    /**< One hex digit.                       */
  k_probe_at_timeout_ms   = 2000U,   /**< Default AT command timeout.          */
  k_probe_baud            = 115200U, /**< Console + DA16600 UART baud.         */
  k_probe_baud_alt1       = 230400U, /**< Sweep: DA16600 debug-console rate.   */
  k_probe_baud_alt2       = 9600U,   /**< Sweep: conservative fallback.        */
  k_probe_baud_alt3       = 57600U,  /**< Sweep: legacy default.               */
  k_probe_baud_alt4       = 921600U, /**< Sweep: high-rate option.             */
  k_probe_da16600_sci_ch  = 2U,      /**< Pmod1 UART = SCI2 (TXD2/RXD2).       */
  k_probe_at_line_buf_len = 256U,    /**< AT-line accumulator length.          */
  k_probe_boot_settle_ms  = 2500U,   /**< DA16600 cold-boot settle window.     */
  k_probe_sweep_window_ms = 500U,    /**< RX listen window per baud sweep.     */
  k_probe_reset_pulse_ms  = 200U,    /**< RESET low-pulse width.               */
  k_probe_reset_boot_ms   = 2000U,   /**< Post-reset boot wait (DA16600 cold). */
  k_probe_banner_wait_ms  = 6000U,   /**< Boot-banner listen window.           */
  k_probe_u15_iic_ch      = 1U,      /**< U15 expander IIC_B channel.          */
  k_probe_u15_addr        = 0x43U,   /**< U15 PI4IOE5V6408 7-bit address.      */
  k_probe_u15_reg_iodir   = 0x03U,   /**< U15 direction (1=output).            */
  k_probe_u15_reg_out     = 0x05U,   /**< U15 output-latch register.           */
  k_probe_u15_reg_hiz     = 0x07U,   /**< U15 output Hi-Z (1=Hi-Z).            */
  k_probe_u15_reg_input   = 0x0FU,   /**< U15 input-level register.            */
  k_probe_u15_default     = 0xF2U,   /**< Project-default latch (BSP value).   */
  k_probe_loop_pattern    = 0xA5U,   /**< Loopback test byte.                  */
  k_probe_tx_test_bytes   = 24U,     /**< 0x00 frames sent during pin sampling.*/
  k_probe_tx_samples      = 400U,    /**< PIDR samples per transmitted frame.  */
  k_probe_txd2_pin_bit    = 1U,      /**< P801 bit within PORT8 PIDR.          */
  k_probe_rxd2_pin_bit    = 2U,      /**< P802 bit within PORT8 PIDR.          */
  k_probe_ard_rx_pin_bit  = 8U,      /**< P808 (Arduino D0/RXD7) PIDR bit.     */
  k_probe_ard_tx_pin_bit  = 9U,      /**< P809 (Arduino D1/TXD7) PIDR bit.     */
  k_probe_sci7_ch         = 7U,      /**< Arduino D0/D1 UART = SCI7.           */
  k_probe_echo_port       = 7U,      /**< RFC 862 TCP echo port.               */
  k_probe_echo_buf_len    = 128U,    /**< Echo payload buffer.                 */
  k_probe_recv_window_ms  = 10000U,  /**< Per-pass TCP recv wait.              */
  k_probe_edge_window_ms  = 8000U,   /**< Edge-sampler listen window.          */
  k_probe_u15_sweep_base  = 0xF8U,   /**< Focused sweep base (bits 3-7 high).  */
  k_probe_u15_sweep_n     = 8U,      /**< All combos of latch bits 0-2.        */
  k_probe_u15_full_n      = 256U,    /**< Exhaustive latch state space.        */
  k_probe_u15_settle_ms   = 150U,    /**< Mux/level settle after a latch write.*/
  k_probe_quick_at_ms     = 400U,    /**< Short AT window inside the sweep.    */
  k_probe_baud_div        = 100U,    /**< Sweep label divisor (fits uint16).   */
  k_probe_ascii_del       = 0x7FU,   /**< First non-printable upper ASCII.     */
  k_probe_pass_period_ms  = 5000U,   /**< Idle heartbeat between ladder runs.  */
  k_probe_p602_pin_bit    = 2U,      /**< Pmod2 RXD0 (P602) bit in PORT6 PIDR.  */
  k_probe_p603_pin_bit    = 3U,      /**< Pmod2 TXD0 (P603) bit in PORT6 PIDR.  */
  k_probe_u15_all_high    = 0xFFU,   /**< Drive-test latch: all outputs HIGH.   */
  k_probe_u15_all_low     = 0x00U,   /**< Drive-test latch: all outputs LOW.    */
  k_probe_wake_pulse_ms   = 20U,     /**< Hold time per wake pulse level.       */
  k_probe_wake_cycles     = 3U,      /**< Wake L->H->L pulses to emit.          */
  k_probe_wake_settle_ms  = 200U,    /**< Settle after wake before AT.          */
} da16600_probe_const_t;

/* Pmod2 (J25) alternate landing for the US159-DA16600EVZ: SCI0 on PORT6
 * (TXD0=P603, RXD0=P602), RESET=P410, RTS0=P604. UM Table 19 p 27. An
 * authentic Renesas/Avnet DA16600 reference plugs the module into Pmod2,
 * so the probe samples this connector too -- if the module is on Pmod2
 * its boot edges land here and not on Pmod1's P802. */
static const ra_port_pin_t k_probe_p2_rxd   = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_2);
static const ra_port_pin_t k_probe_p2_txd   = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_3);
static const ra_port_pin_t k_probe_p2_rts   = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_4);
static const ra_port_pin_t k_probe_p2_reset = (ra_port_pin_t)RA_PIN(k_ra_port_4, k_ra_pin_10);

/** @brief Pmod1 UART pins (J26): TXD2 = P801, RXD2 = P802. */
static const ra_port_pin_t k_probe_pin_txd = (ra_port_pin_t)k_ra_board_pmod1_uart_txd;
static const ra_port_pin_t k_probe_pin_rxd = (ra_port_pin_t)k_ra_board_pmod1_uart_rxd;

/** @brief AT line buffer (caller-owned per ::ra_da16600_cfg_t contract). */
static uint8_t s_at_line[k_probe_at_line_buf_len];

/** @brief AP fixture credentials (scripts/hil_da16600_setup.md). */
static const char k_probe_ssid[] = "hil_lab";
/** @brief AP fixture passphrase. */
static const char k_probe_pass[] = "test1234";

/** @brief Cached PCLKA rate (Hz) for the late SCI2 bring-up. */
static uint32_t s_pclka_hz;

/** @brief Boot-window RX capture (replayed every ladder pass). */
static uint8_t s_bootlog[k_probe_at_line_buf_len];
/** @brief Bytes captured into ::s_bootlog. */
static uint16_t s_bootlog_len;

/* =============================================================================
 * Console helpers (SCI8 J-Link OB VCOM)
 * =============================================================================
 */

/**
 * @brief Park the CPU in WFI forever after a fatal boot error.
 *
 * @details Used only when the console / clock bring-up itself fails.
 *
 * @pre Called only after a fatal error in boot.
 * @pre Interrupts may be in any state.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post No further code runs.
 * @note Never returns.
 * @since 0.1.0
 */
static void probe_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a NUL-terminated string to the VCOM console.
 *
 * @details Thin wrapper over ::ra_board_uart_console_write.
 *
 * @param[in] s NUL-terminated ASCII string.
 *
 * @pre Console is initialized.
 * @pre @p s is non-NULL and NUL-terminated.
 * @post The bytes have been queued on SCI8.
 * @post No other state changes.
 * @note Blocking polled write.
 * @since 0.1.0
 */
static void probe_log(const char* s)
{
  uint32_t n = 0U;
  while (s[n] != '\0') {
    n++;
  }
  (void)ra_board_uart_console_write((const uint8_t*)s, (size_t)n);
}

/**
 * @brief Log a 16-bit value as 4 uppercase hex digits.
 *
 * @details Used to surface ::ra_err_t codes for remote diagnosis.
 *
 * @param[in] v Value to print.
 *
 * @pre Console is initialized.
 * @pre None beyond console readiness.
 * @post Four hex digits have been written to the console.
 * @post No other state changes.
 * @note Blocking polled write.
 * @since 0.1.0
 */
static void probe_log_hex16(uint16_t v)
{
  static const char hex[] = "0123456789ABCDEF";
  char              out[k_probe_hex_str_len];
  out[0] = hex[(v >> (uint16_t)(3U * k_probe_hex_nibble)) & k_probe_hex_mask];
  out[1] = hex[(v >> (uint16_t)(2U * k_probe_hex_nibble)) & k_probe_hex_mask];
  out[2] = hex[(v >> (uint16_t)k_probe_hex_nibble) & k_probe_hex_mask];
  out[3] = hex[v & k_probe_hex_mask];
  out[4] = '\0';
  probe_log(out);
}

/**
 * @brief Decimal-format @p value into @p dst (libc-free, NUL-terminated).
 *
 * @details Worst case "65535" + NUL = 6 bytes.
 *
 * @param[out] dst   Caller buffer (must hold at least 6 bytes).
 * @param[in]  value Value to format.
 *
 * @pre @p dst points to at least 6 writable bytes.
 * @pre None beyond the buffer contract.
 * @post @p dst holds the NUL-terminated decimal string.
 * @post No other state changes.
 * @note Pure function apart from the output buffer.
 * @since 0.1.0
 */
static void probe_format_u16(char* dst, uint16_t value)
{
  char    tmp[k_probe_u16_str_len];
  uint8_t ti = 0U;
  if (value == 0U) {
    dst[0] = '0';
    dst[1] = '\0';
    return;
  }
  uint16_t v = value;
  while (v > 0U) {
    if (ti >= (uint8_t)k_probe_u16_max_digits) {
      break;
    }
    tmp[ti] = (char)('0' + (uint8_t)(v % k_probe_decimal_base));
    v /= (uint16_t)k_probe_decimal_base;
    ++ti;
  }
  uint8_t di = 0U;
  while (ti > 0U) {
    --ti;
    dst[di] = tmp[ti];
    ++di;
  }
  dst[di] = '\0';
}

/* =============================================================================
 * DA16600 byte transport (SCI2)
 * =============================================================================
 */

/**
 * @brief Polled TX of one byte on the DA16600 SCI channel.
 *
 * @details Bound into ::ra_modem_at_io_t::tx_byte.
 *
 * @param[in] ctx  Unused.
 * @param[in] byte Byte to transmit.
 * @return ::ra_err_t from the SCI driver.
 * @retval k_ra_ok Byte accepted.
 *
 * @pre SCI2 is initialized.
 * @pre None beyond SCI readiness.
 * @post One byte has been shifted out on TXD2.
 * @post No other state changes.
 * @note Blocking polled write.
 * @since 0.1.0
 */
static ra_err_t probe_da16600_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  return ra_sci_putc_polling((uint8_t)k_probe_da16600_sci_ch, byte);
}

/**
 * @brief Bounded-poll RX of one byte on the DA16600 SCI channel.
 *
 * @details Bound into ::ra_modem_at_io_t::rx_byte. A short spin with no
 * data returns non-OK, which the AT engine treats as "no byte yet".
 *
 * @param[in]  ctx Unused.
 * @param[out] out Receive slot.
 * @return ::ra_err_t from the SCI driver.
 * @retval k_ra_ok One byte was popped into @p out.
 *
 * @pre SCI2 is initialized.
 * @pre @p out is non-NULL.
 * @post On k_ra_ok, ``*out`` holds the received byte.
 * @post On any other code, ``*out`` is untouched.
 * @note Bounded spin; never blocks indefinitely.
 * @since 0.1.0
 */
static ra_err_t probe_da16600_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  return ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, out);
}

/**
 * @brief Monotonic millisecond timestamp for AT timeouts.
 *
 * @details Bound into ::ra_modem_at_io_t::now_ms; forwards ra_time's
 * SysTick millisecond counter.
 *
 * @param[in] ctx Unused.
 * @return Milliseconds since boot.
 * @retval 0..UINT32_MAX Monotonic tick.
 *
 * @pre ::ra_time_init has been called.
 * @pre None beyond the timebase.
 * @post No state changes.
 * @post Result is monotonic across calls.
 * @note Wraps after ~49.7 days; irrelevant here.
 * @since 0.1.0
 */
static uint32_t probe_da16600_now_ms(void* ctx)
{
  (void)ctx;
  return ra_time_ms();
}

/* =============================================================================
 * Bring-up
 * =============================================================================
 */

/**
 * @brief Bring CGC + SysTick + VCOM console + SCI2 + pin mux up.
 *
 * @details Panic-halts on any failure (no console to report on yet).
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; SCI2 runs 115200 8N1 on P801/P802.
 * @post SysTick millisecond timebase is live.
 * @note Called exactly once from main.
 * @since 0.1.0
 */
static void probe_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_pclka_hz) != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_board_uart_console_init((uint32_t)k_probe_baud) != k_ra_ok) {
    probe_panic_halt();
  }
  /* Force the SW4 layout this project needs (Pmod1 UART + Octo-SPI
   * inactive) through the U15 expander. UM Sec 4.3.4 p 15: with the
   * expander port in output mode the software value overrides the SW4
   * DIP -- verified valid here (the old "DIP overpowers" reading was a
   * 0x0F-blind-to-outputs artifact; see probe_u15_drive_test). */
  const ra_err_t sw4 = ra_board_io_expander_apply_project_sw4_defaults();
  probe_log((sw4 == k_ra_ok) ? "sw4: override applied\r\n" : "sw4: override FAILED\r\n");
  /* Assert host RTS (P804) LOW *before* any wire/edge measurement. The
   * authentic FSP transport (rm_at_transport_da16xxx_uart) opens the UART
   * with SCI_UART_FLOW_CONTROL_RTS: host RTS drives the DA16600's CTS
   * input, and the module gates its TX (boot log + AT replies) on it.
   * Until this is LOW the module is forbidden to transmit, which reads on
   * the bus as a dead, edge-free wire even though the module is alive.
   * P804 doubles as Pmod1 SPI-CS, so it is ours to drive; it is
   * independent of the P801/P802 pins the edge sampler claims. */
  const ra_port_pin_t pin_rts = (ra_port_pin_t)k_ra_board_pmod1_uart_rts;
  if (ra_gpio_output_init(pin_rts, k_ra_level_low) == k_ra_ok) {
    probe_log("flow: RTS(P804) asserted LOW\r\n");
  } else {
    probe_log("flow: RTS(P804) claim FAILED\r\n");
  }
}

/**
 * @brief Route P801/P802 to SCI2 and start the DA16600 UART.
 *
 * @details Runs AFTER the wire survey so the GPIO claims do not clash.
 * Panic-halts on failure (boot-time wiring is non-recoverable).
 *
 * @pre ::probe_setup_or_halt and ::probe_rung_wire have run.
 * @pre P801/P802 are unclaimed.
 * @post SCI2 runs 115200 8N1 on P801/P802.
 * @post The AT byte transport is usable.
 * @note Called exactly once from main.
 * @since 0.1.0
 */
static void probe_uart_setup_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_probe_pin_txd, k_ra_psel_sci_async, "da16600.txd2") != k_ra_ok) {
    probe_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_probe_pin_rxd, k_ra_psel_sci_async, "da16600.rxd2") != k_ra_ok) {
    probe_panic_halt();
  }
  const ra_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_probe_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = s_pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_probe_da16600_sci_ch, &cfg) != k_ra_ok) {
    probe_panic_halt();
  }
}

/**
 * @brief Rung 0: physical-layer diagnostics on the DA16600 UART.
 *
 * @details Reads the RXD2 line level as a GPIO (an idle UART driven by
 * a live module reads HIGH; a mux-open or unpowered line reads LOW or
 * floats), then sends a manual ``AT`` and hex-dumps every byte seen on
 * SCI2 for a short window. Run before the AT engine so the wire truth
 * is visible even when the protocol layer fails.
 *
 * @pre Console is initialized; SCI2 + pins are NOT yet routed.
 * @pre ::ra_time_init has been called.
 * @post RXD2 idle level and a raw RX hex dump have been printed.
 * @post P802 is left routed to SCI2 (peripheral mode).
 * @note Diagnostic only; results are human-read over VCOM.
 * @since 0.1.0
 */
/**
 * @brief Survey one pin: read it bare, then with the internal pull-up.
 *
 * @details Claims the pin as a GPIO input twice (releasing in between
 * and afterwards so later peripheral routing succeeds) and prints a
 * one-line verdict: driven HIGH (live idle UART), driven LOW, or
 * FLOATING (bare LOW but pull-up wins -- nothing is connected).
 *
 * @param[in] name Label printed before the verdict.
 * @param[in] pin  Pin to survey.
 *
 * @pre Console is initialized.
 * @pre @p pin is not claimed by any driver.
 * @post One survey line has been printed.
 * @post @p pin is released (unclaimed).
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_wire_test_pin(const char* name, ra_port_pin_t pin)
{
  ra_level_t bare = k_ra_level_low;
  ra_level_t pull = k_ra_level_low;
  ra_err_t   e1   = ra_gpio_input_init(pin, k_ra_pull_none);
  if (e1 == k_ra_ok) {
    (void)ra_gpio_read(pin, &bare);
    (void)ra_pin_validator_release(pin);
  }
  if (ra_gpio_input_init(pin, k_ra_pull_up) == k_ra_ok) {
    (void)ra_gpio_read(pin, &pull);
    (void)ra_pin_validator_release(pin);
  }
  probe_log(name);
  if (e1 != k_ra_ok) {
    probe_log(" claim FAIL err=");
    probe_log_hex16((uint16_t)e1);
    probe_log("\r\n");
    return;
  }
  probe_log((bare == k_ra_level_high) ? " bare=H" : " bare=L");
  probe_log((pull == k_ra_level_high) ? " pull=H" : " pull=L");
  /* bare=L + pull=H -> floating (mux open / nothing driving).
   * bare=H          -> actively driven high (live idle UART).
   * bare=L + pull=L -> actively driven low.                     */
  if (bare == k_ra_level_high) {
    probe_log(" (driven HIGH)\r\n");
    return;
  }
  probe_log((pull == k_ra_level_high) ? " (FLOATING)\r\n" : " (driven LOW)\r\n");
}

/**
 * @brief Rung 0: physical-layer survey of the five Pmod1 P80x lines.
 *
 * @details Must run BEFORE any SCI routing so the GPIO claims succeed.
 * Reads each pin bare and with the internal pull-up: a floating pin
 * (bare LOW, pull HIGH) means the SW4 analog mux has Pmod1 isolated; a
 * driven-HIGH RXD2 means a live module UART is idling on the line.
 *
 * @pre Console is initialized; no P80x pin is claimed yet.
 * @pre ::ra_time_init has been called.
 * @post Five survey lines have been printed.
 * @post The pins remain claimed as GPIO inputs (re-routed later).
 * @note Diagnostic only; results are human-read over VCOM.
 * @since 0.1.0
 */
static void probe_rung_wire(void)
{
  probe_wire_test_pin("wire: P800/cts2",
                      (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_0));
  probe_wire_test_pin("wire: P801/txd2", k_probe_pin_txd);
  probe_wire_test_pin("wire: P802/rxd2", k_probe_pin_rxd);
  probe_wire_test_pin("wire: P803/sck2",
                      (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_3));
  probe_wire_test_pin("wire: P804/rts2",
                      (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_4));
  /* Side-band pins: direct MCU GPIOs to J26 pins 7/9/10, no OSPI pin
   * sharing and no SW4 involvement. A populated, powered US159 presents
   * its IRQ/GPIO pins here; if every one floats, no module is seated. */
  probe_wire_test_pin("wire: P006/irq ", (ra_port_pin_t)k_ra_board_pmod1_irq);
  probe_wire_test_pin("wire: P412/gpa ", (ra_port_pin_t)k_ra_board_pmod1_gpio_a);
  probe_wire_test_pin("wire: P413/gpb ", (ra_port_pin_t)k_ra_board_pmod1_gpio_b);
}

/**
 * @brief Try one baud: send ``AT`` (CRLF) raw and hex-dump the reply.
 *
 * @details Re-inits SCI2 at @p baud, drains stale RX, transmits
 * ``AT\r\n`` byte by byte, then prints every byte received within a
 * 500 ms window as hex (plus printable ASCII in brackets).
 *
 * @param[in] baud UART bit-rate to test.
 *
 * @pre SCI2 pins are routed; console is initialized.
 * @pre ::ra_time_init has been called.
 * @post One sweep line has been printed.
 * @post SCI2 is left configured at @p baud.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_sweep_baud(uint32_t baud)
{
  const ra_sci_cfg_t cfg = {
    .baud      = baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = s_pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_probe_da16600_sci_ch, &cfg) != k_ra_ok) {
    probe_log("sweep: sci re-init FAIL\r\n");
    return;
  }
  uint8_t drain = 0U;
  while (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &drain) == k_ra_ok) {
  }
  static const char at_cmd[] = "AT\r\n";
  for (uint32_t i = 0U; at_cmd[i] != '\0'; i++) {
    (void)ra_sci_putc_polling((uint8_t)k_probe_da16600_sci_ch, (uint8_t)at_cmd[i]);
  }
  probe_log("sweep ");
  char digits[k_probe_u16_str_len];
  probe_format_u16(digits, (uint16_t)(baud / (uint32_t)k_probe_baud_div));
  probe_log(digits);
  probe_log("00:");
  const uint32_t start    = ra_time_ms();
  uint32_t       seen     = 0U;
  char           ascii[2] = {};
  while ((ra_time_ms() - start) < (uint32_t)k_probe_sweep_window_ms) {
    uint8_t b = 0U;
    if (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &b) != k_ra_ok) {
      continue;
    }
    probe_log(" ");
    probe_log_hex16((uint16_t)b);
    if (b >= (uint8_t)' ') {
      if (b < (uint8_t)k_probe_ascii_del) {
        ascii[0] = (char)b;
        probe_log("[");
        probe_log(ascii);
        probe_log("]");
      }
    }
    seen++;
  }
  probe_log((seen == 0U) ? " (silence)\r\n" : "\r\n");
}

/**
 * @brief Read back U15's output latch and physical input levels.
 *
 * @details The SW4 override programs the U15 latch; this reads back the
 * latch (reg 0x05), the direction (0x03), Hi-Z (0x07) and the input-level
 * register (0x0F). NB: 0x0F reads 0x00 for any pin held in output mode on
 * the PI4IOE5V6408 (see ::probe_u15_drive_test), so a latch!=level result
 * is the register being blind to outputs, NOT the DIP winning.
 *
 * @pre The SW4 override ran (IIC_B1 is initialized, U15 ACKs).
 * @pre Console is up.
 * @post One status line with both register values has been printed.
 * @post No U15 state is modified.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_u15_readback(void)
{
  uint8_t  reg   = (uint8_t)k_probe_u15_reg_out;
  uint8_t  latch = 0U;
  uint8_t  level = 0U;
  ra_err_t e =
    ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &latch, 1U);
  if (e != k_ra_ok) {
    probe_log("u15: latch read FAIL\r\n");
    return;
  }
  reg = (uint8_t)k_probe_u15_reg_input;
  e = ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &level, 1U);
  if (e != k_ra_ok) {
    probe_log("u15: level read FAIL\r\n");
    return;
  }
  /* Read the direction (0x03) and Hi-Z (0x07) registers too. If the BSP
   * truly put the pins in push-pull output mode these read iodir=0xFF /
   * hiz=0x00; if so and pins still differ from latch, the DIP is winning
   * an actual electrical fight. If they read otherwise, the override
   * never armed and it is fixable in software. */
  uint8_t iodir = 0U;
  uint8_t hiz   = 0U;
  reg           = (uint8_t)k_probe_u15_reg_iodir;
  (void)
    ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &iodir, 1U);
  reg = (uint8_t)k_probe_u15_reg_hiz;
  (void)ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &hiz, 1U);
  probe_log("u15: latch=");
  probe_log_hex16((uint16_t)latch);
  probe_log(" pins=");
  probe_log_hex16((uint16_t)level);
  probe_log(" iodir=");
  probe_log_hex16((uint16_t)iodir);
  probe_log(" hiz=");
  probe_log_hex16((uint16_t)hiz);
  probe_log((latch == level) ? " (in==out)\r\n" : " (0x0F blind to outputs)\r\n");
}

/**
 * @brief Empirically decide whether U15's input register tracks its outputs.
 *
 * @details Drives the latch to 0xFF then 0x00 (pins already iodir=output,
 * hiz=0) and reads the input-level register (0x0F) after each. If 0x0F
 * follows (0xFF then 0x00) the expander really drives the SW4 nets and the
 * mux moves with software -- so a static ``pins=0000`` was a readback that
 * simply does not reflect output pins, NOT a DIP overpowering the driver.
 * If 0x0F stays put regardless, the nets are externally pinned. Restores
 * the project latch on exit.
 *
 * @pre ::probe_rung_u15_readback has run; IIC_B1 is up.
 * @pre Console is initialized.
 * @post U15 output latch restored to the project default (0xF2).
 * @post Two probe lines have been printed.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static ra_err_t probe_u15_write_latch(uint8_t value);

static void probe_u15_drive_test(void)
{
  uint8_t reg = (uint8_t)k_probe_u15_reg_input;
  uint8_t hi  = 0U;
  uint8_t lo  = 0U;
  (void)probe_u15_write_latch((uint8_t)k_probe_u15_all_high);
  ra_delay_ms((uint32_t)k_probe_u15_settle_ms);
  (void)ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &hi, 1U);
  (void)probe_u15_write_latch((uint8_t)k_probe_u15_all_low);
  ra_delay_ms((uint32_t)k_probe_u15_settle_ms);
  (void)ra_i2c_transfer((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, &reg, 1U, &lo, 1U);
  (void)probe_u15_write_latch((uint8_t)k_probe_u15_default);
  probe_log("u15 drive: out=FF->pins=");
  probe_log_hex16((uint16_t)hi);
  probe_log(" out=00->pins=");
  probe_log_hex16((uint16_t)lo);
  probe_log((hi != lo) ? " (OUTPUTS LIVE)\r\n" : " (pins pinned)\r\n");
}

/**
 * @brief Hard-reset the DA16600 via Pmod1.8 (P402) and dump its banner.
 *
 * @details Drives RESET low for the pulse width, releases it (input,
 * the daughter card self-pulls), then hex-dumps everything the module
 * prints on SCI2 during the boot window. The DA16600 emits an
 * ``+INIT:DONE`` banner on its AT UART after reset (UM-WI-046), so any
 * bytes here prove module presence AND confirm the baud.
 *
 * @pre SCI2 is initialized at the listen baud; console is up.
 * @pre P402 is unclaimed.
 * @post One reset cycle has been issued; the dump line is printed.
 * @post P402 is held a driven output-high (reset de-asserted).
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_reset_banner(void)
{
  const ra_port_pin_t pin_rst = (ra_port_pin_t)k_ra_board_pmod1_reset;
  if (ra_gpio_output_init(pin_rst, k_ra_level_low) != k_ra_ok) {
    probe_log("reset: claim FAIL\r\n");
    return;
  }
  ra_delay_ms((uint32_t)k_probe_reset_pulse_ms);
  /* Release reset HIGH and HOLD it. The authentic FSP transport drives
   * RESET low ~20 ms then high and keeps it driven; the EK's FSP default
   * leaves this pin LOW, which parks the module in reset. Floating it to
   * an input (as before) let it drift back low and re-arm reset, so the
   * module never finished booting. Keep P402 a driven output-high. */
  (void)ra_gpio_write(pin_rst, k_ra_level_high);
  ra_delay_ms((uint32_t)k_probe_reset_boot_ms);
  uint8_t drain = 0U;
  while (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &drain) == k_ra_ok) {
  }
  probe_log("reset: pulsed, banner dump:");
  const uint32_t start    = ra_time_ms();
  uint32_t       seen     = 0U;
  char           ascii[2] = {};
  while ((ra_time_ms() - start) < (uint32_t)k_probe_banner_wait_ms) {
    uint8_t b = 0U;
    if (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &b) != k_ra_ok) {
      continue;
    }
    if (s_bootlog_len < (uint16_t)k_probe_at_line_buf_len) {
      s_bootlog[s_bootlog_len] = b;
      s_bootlog_len++;
    }
    if (b >= (uint8_t)' ') {
      if (b < (uint8_t)k_probe_ascii_del) {
        ascii[0] = (char)b;
        probe_log(ascii);
        seen++;
        continue;
      }
    }
    probe_log("<");
    probe_log_hex16((uint16_t)b);
    probe_log(">");
    seen++;
  }
  probe_log((seen == 0U) ? " (silence)\r\n" : "\r\n");
}

/**
 * @brief Replay the boot-window RX capture on the console.
 *
 * @details The VCOM dies with board power, so a cold-boot banner is
 * invisible live; this replays what the firmware heard at T0 on every
 * ladder pass so a late-attaching reader still sees it.
 *
 * @pre Console is initialized.
 * @pre ::probe_rung_reset_banner has run once.
 * @post One replay line has been printed.
 * @post ::s_bootlog is unmodified.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_replay_bootlog(void)
{
  probe_log("bootlog n=");
  char digits[k_probe_u16_str_len];
  probe_format_u16(digits, s_bootlog_len);
  probe_log(digits);
  probe_log(":");
  char ascii[2] = {};
  for (uint16_t i = 0U; i < s_bootlog_len; i++) {
    const uint8_t b = s_bootlog[i];
    if (b >= (uint8_t)' ') {
      if (b < (uint8_t)k_probe_ascii_del) {
        ascii[0] = (char)b;
        probe_log(ascii);
        continue;
      }
    }
    probe_log("<");
    probe_log_hex16((uint16_t)b);
    probe_log(">");
  }
  probe_log("\r\n");
}

/**
 * @brief Write a raw value into U15's output latch (reg 0x05).
 *
 * @details The PI4IOE5V6408 input-status read is not trustworthy for
 * output-mode pins, but issue #44 proved latch bit 2 (OSPI_OE_L) has a
 * real electrical effect. This pokes the latch directly so the sweep
 * can hunt the bit pattern that routes Pmod1 UART.
 *
 * @param[in] value Latch byte to program.
 * @return ::ra_err_t from the I2C write.
 * @retval k_ra_ok Latch accepted.
 *
 * @pre The SW4 override ran once (IIC_B1 up, U15 configured).
 * @pre Console is initialized.
 * @post U15 output latch holds @p value.
 * @post No other U15 registers change.
 * @note Diagnostic helper.
 * @since 0.1.0
 */
static ra_err_t probe_u15_write_latch(uint8_t value)
{
  uint8_t frame[2];
  frame[0] = (uint8_t)k_probe_u15_reg_out;
  frame[1] = value;
  return ra_i2c_write((uint8_t)k_probe_u15_iic_ch, (uint8_t)k_probe_u15_addr, frame, 2U, true);
}

/**
 * @brief Send one ``AT`` and wait briefly for any RX byte.
 *
 * @details Raw single-shot used inside the latch sweep: transmits
 * ``AT\r\n`` and reports whether anything at all came back within the
 * quick window. Any reply identifies the winning latch value.
 *
 * @return 1 when at least one byte arrived, else 0.
 * @retval 1 RX saw traffic.
 * @retval 0 Silence.
 *
 * @pre SCI2 is initialized at 115200.
 * @pre ::ra_time_init has been called.
 * @post The RX FIFO has been drained.
 * @post No persistent state changes.
 * @note Diagnostic helper.
 * @since 0.1.0
 */
static uint8_t probe_quick_at(void)
{
  uint8_t drain = 0U;
  while (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &drain) == k_ra_ok) {
  }
  static const char at_cmd[] = "AT\r\n";
  for (uint32_t i = 0U; at_cmd[i] != '\0'; i++) {
    (void)ra_sci_putc_polling((uint8_t)k_probe_da16600_sci_ch, (uint8_t)at_cmd[i]);
  }
  const uint32_t start = ra_time_ms();
  while ((ra_time_ms() - start) < (uint32_t)k_probe_quick_at_ms) {
    uint8_t b = 0U;
    if (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &b) == k_ra_ok) {
      return 1U;
    }
  }
  return 0U;
}

/**
 * @brief Sweep all 8 combinations of U15 latch bits 0-2, AT-probing each.
 *
 * @details FSP names latch bits 0,1 "OPMOD1/0 mode-selects" and bit 2
 * "OSPI_OE_L"; the project-default value (0xF2) keeps the flash bus
 * enabled and may leave Pmod1 in the wrong mode. For each candidate the
 * sweep programs the latch, settles, GPIO-samples P802, then fires a
 * quick ``AT``. A reply pins down the routing value.
 *
 * @pre SCI2 + console are initialized; U15 is configured.
 * @pre ::ra_time_init has been called.
 * @post Eight result lines have been printed.
 * @post The latch is left at the LAST value that saw RX traffic, or the
 *       project default when none did.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_u15_sweep(void)
{
  uint8_t winner = 0U;
  uint8_t found  = 0U;
  for (uint8_t combo = 0U; combo < (uint8_t)k_probe_u15_sweep_n; combo++) {
    const uint8_t value = (uint8_t)((uint8_t)k_probe_u15_sweep_base | combo);
    if (probe_u15_write_latch(value) != k_ra_ok) {
      probe_log("u15 sweep: write FAIL\r\n");
      return;
    }
    ra_delay_ms((uint32_t)k_probe_u15_settle_ms);
    const uint8_t got = probe_quick_at();
    probe_log("u15 sweep latch=");
    probe_log_hex16((uint16_t)value);
    probe_log((got == 1U) ? " RX TRAFFIC\r\n" : " silent\r\n");
    if (got == 1U) {
      winner = value;
      found  = 1U;
    }
  }
  if (found == 1U) {
    (void)probe_u15_write_latch(winner);
    probe_log("u15 sweep: winner kept\r\n");
    return;
  }
  (void)probe_u15_write_latch((uint8_t)k_probe_u15_default);
}

/**
 * @brief Exhaustive sweep: every U15 latch value 0x00..0xFF, AT each.
 *
 * @details Walks the expander's entire output state space -- every
 * board routing firmware can possibly select -- firing a quick ``AT``
 * at each value. Prints only the values that produce RX traffic, plus
 * a final summary. Restores the project-default latch on exit unless a
 * winner was found (the winner is kept and reported).
 *
 * @pre SCI2 + console are initialized; U15 is configured.
 * @pre ::ra_time_init has been called.
 * @post A summary line has been printed.
 * @post The latch holds the winner, or the project default.
 * @note Runs ~2.5 minutes; diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_u15_full_sweep(void)
{
  uint16_t hits   = 0U;
  uint8_t  winner = 0U;
  probe_log("u15 full sweep: start\r\n");
  for (uint32_t v = 0U; v < (uint32_t)k_probe_u15_full_n; v++) {
    if (probe_u15_write_latch((uint8_t)v) != k_ra_ok) {
      probe_log("u15 full sweep: write FAIL\r\n");
      return;
    }
    ra_delay_ms((uint32_t)k_probe_u15_settle_ms);
    if (probe_quick_at() == 1U) {
      probe_log("u15 full sweep: RX TRAFFIC at latch=");
      probe_log_hex16((uint16_t)v);
      probe_log("\r\n");
      winner = (uint8_t)v;
      hits++;
    }
  }
  if (hits > 0U) {
    (void)probe_u15_write_latch(winner);
    probe_log("u15 full sweep: winner kept\r\n");
    return;
  }
  (void)probe_u15_write_latch((uint8_t)k_probe_u15_default);
  probe_log("u15 full sweep: all 256 silent\r\n");
}

/**
 * @brief Self-test SCI2 with zero external dependencies.
 *
 * @details Two checks that isolate driver bugs from wiring:
 *  1. Internal loopback: sets CCR1.SPLP (TE/RE held 0 across the CCR1
 *     write per the SCI restriction), sends one byte, and expects to
 *     receive it back -- exercising MSTP, clocking, BRR, TX and RX of
 *     channel 2 end to end inside the silicon.
 *  2. TX pin sampling: with loopback off, streams 0x00 frames while
 *     reading PORT8's live PIDR for P801 -- a transmitting UART must
 *     drag the pin low for the start bit + 8 zero bits of every frame.
 *
 * @pre SCI2 is initialized at the working baud; console is up.
 * @pre P801 is routed to TXD2 (peripheral mode).
 * @post Two verdict lines have been printed.
 * @post SCI2 is restored to normal (non-loopback) mode.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_sci2_self(void)
{
  volatile r_sci_regs_t* r = ra_sci((uint8_t)k_probe_da16600_sci_ch);
  if (r == nullptr) {
    probe_log("sci2 self: no reg block\r\n");
    return;
  }
  const uint32_t saved_ccr0 = r->CCR0;
  r->CCR0                   = 0U;
  r->CCR1                   = r->CCR1 | (1U << (uint32_t)k_ra_sci_ccr1_bit_splp);
  r->CCR0                   = saved_ccr0;
  uint8_t drain             = 0U;
  while (ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &drain) == k_ra_ok) {
  }
  (void)ra_sci_putc_polling((uint8_t)k_probe_da16600_sci_ch, (uint8_t)k_probe_loop_pattern);
  uint8_t        echo = 0U;
  const ra_err_t ge   = ra_sci_getc_polling((uint8_t)k_probe_da16600_sci_ch, &echo);
  uint8_t        pass = 0U;
  if (ge == k_ra_ok) {
    if (echo == (uint8_t)k_probe_loop_pattern) {
      pass = 1U;
    }
  }
  r->CCR0 = 0U;
  r->CCR1 = r->CCR1 & ~(1U << (uint32_t)k_ra_sci_ccr1_bit_splp);
  r->CCR0 = saved_ccr0;
  probe_log((pass == 1U) ? "sci2 self: loopback ok\r\n" : "sci2 self: loopback FAIL\r\n");

  volatile r_port_regs_t* p8   = ra_port(k_ra_port_8);
  uint32_t                lows = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_probe_tx_test_bytes; i++) {
    (void)ra_sci_putc_polling((uint8_t)k_probe_da16600_sci_ch, 0U);
    for (uint32_t j = 0U; j < (uint32_t)k_probe_tx_samples; j++) {
      const uint32_t pidr = p8->PCNTR2;
      if ((pidr & (1U << (uint32_t)k_probe_txd2_pin_bit)) == 0U) {
        lows++;
      }
    }
  }
  probe_log((lows > 0U) ? "sci2 self: txd2 pin TOGGLES\r\n" : "sci2 self: txd2 pin STUCK HIGH\r\n");
}

/**
 * @struct edge_track_t
 * @brief Per-pin edge/pulse statistics for the GPIO sampler.
 */
typedef struct {
  uint32_t edges;  /**< Level transitions seen.              */
  uint32_t min_lo; /**< Narrowest low pulse (iterations).    */
  uint32_t run;    /**< Current low-run length (iterations). */
  uint32_t prev;   /**< Previous sampled level (0/1).        */
} edge_track_t;

/**
 * @brief Fold one sample into a pin's edge statistics.
 *
 * @details Counts transitions and tracks the narrowest low pulse.
 *
 * @param[in,out] t   Pin statistics.
 * @param[in]     bit Sampled level (0 or 1).
 *
 * @pre @p t was zero-initialized with ``prev`` seeded.
 * @pre @p bit is 0 or 1.
 * @post Statistics reflect the new sample.
 * @post ``t->prev`` holds @p bit.
 * @note Hot path of the sampler loop.
 * @since 0.1.0
 */
static void probe_edge_step(edge_track_t* t, uint32_t bit)
{
  if (bit != t->prev) {
    t->edges++;
    if (bit == 1U) {
      if (t->run < t->min_lo) {
        t->min_lo = t->run;
      }
    }
    t->run = 0U;
  } else if (bit == 0U) {
    t->run++;
  }
  t->prev = bit;
}

/**
 * @brief Print one pin's edge statistics.
 *
 * @details Clamps the 32-bit counters into the uint16 formatter.
 *
 * @param[in] name Label for the pin.
 * @param[in] t    Statistics to print.
 *
 * @pre Console is initialized.
 * @pre @p name and @p t are non-NULL.
 * @post One fragment has been printed (no newline).
 * @post No state changes.
 * @note Diagnostic helper.
 * @since 0.1.0
 */
static void probe_edge_report(const char* name, const edge_track_t* t)
{
  char digits[k_probe_u16_str_len];
  probe_log(name);
  probe_log(" edges=");
  probe_format_u16(digits, (uint16_t)((t->edges > (uint32_t)UINT16_MAX) ? UINT16_MAX : t->edges));
  probe_log(digits);
  probe_log(" minlo=");
  probe_format_u16(digits, (uint16_t)((t->min_lo > (uint32_t)UINT16_MAX) ? UINT16_MAX : t->min_lo));
  probe_log(digits);
}

/**
 * @brief GPIO logic-analyzer: watch P801+P802 raw across a module reset.
 *
 * @details Releases both UART pins to GPIO inputs (pull-up), pulses the
 * Pmod1 RESET line, then tight-samples PORT8's PIDR for the whole boot
 * window. Catches module TX at ANY baud and on EITHER pin -- detecting
 * both a wrong-baud mismatch and a swapped TX/RX Pmod wiring.
 *
 * @pre Console is up; ::ra_time_init has been called.
 * @pre The Pmod1 pins are claimable.
 * @post One report line has been printed.
 * @post P801/P802 are released (caller re-routes them to SCI2).
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_edge_sampler(void)
{
  (void)ra_pin_validator_release(k_probe_pin_txd);
  (void)ra_pin_validator_release(k_probe_pin_rxd);
  if (ra_gpio_input_init(k_probe_pin_txd, k_ra_pull_up) != k_ra_ok) {
    probe_log("edges: txd claim FAIL\r\n");
    return;
  }
  if (ra_gpio_input_init(k_probe_pin_rxd, k_ra_pull_up) != k_ra_ok) {
    probe_log("edges: rxd claim FAIL\r\n");
    return;
  }
  const ra_port_pin_t pin_rst = (ra_port_pin_t)k_ra_board_pmod1_reset;
  (void)ra_pin_validator_release(pin_rst);
  if (ra_gpio_output_init(pin_rst, k_ra_level_low) == k_ra_ok) {
    ra_delay_ms((uint32_t)k_probe_reset_pulse_ms);
    /* Drive RESET HIGH then free the ownership token. The release is
     * bookkeeping only (ra_pin_validator_release does not touch PFS/PDR),
     * so the pin physically stays output-HIGH = reset de-asserted through
     * the sample window, and the later reset rung can re-claim P402. */
    (void)ra_gpio_write(pin_rst, k_ra_level_high);
    (void)ra_pin_validator_release(pin_rst);
  }
  volatile r_port_regs_t* p8    = ra_port(k_ra_port_8);
  edge_track_t            tx    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  edge_track_t            rx    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  edge_track_t            a0    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  edge_track_t            a1    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  uint32_t                iters = 0U;
  const uint32_t          t0    = ra_time_ms();
  while ((ra_time_ms() - t0) < (uint32_t)k_probe_edge_window_ms) {
    const uint32_t now = p8->PCNTR2;
    probe_edge_step(&tx, (now >> (uint32_t)k_probe_txd2_pin_bit) & 1U);
    probe_edge_step(&rx, (now >> (uint32_t)k_probe_rxd2_pin_bit) & 1U);
    probe_edge_step(&a0, (now >> (uint32_t)k_probe_ard_rx_pin_bit) & 1U);
    probe_edge_step(&a1, (now >> (uint32_t)k_probe_ard_tx_pin_bit) & 1U);
    iters++;
  }
  (void)ra_pin_validator_release(k_probe_pin_txd);
  (void)ra_pin_validator_release(k_probe_pin_rxd);
  char digits[k_probe_u16_str_len];
  probe_log("edges: iters/ms=");
  probe_format_u16(digits, (uint16_t)(iters / (uint32_t)k_probe_edge_window_ms));
  probe_log(digits);
  probe_log(" ");
  probe_edge_report("P801", &tx);
  probe_log(" |");
  probe_edge_report(" P802", &rx);
  probe_log(" |");
  probe_edge_report(" P808", &a0);
  probe_log(" |");
  probe_edge_report(" P809", &a1);
  probe_log("\r\n");
}

/**
 * @brief GPIO logic-analyzer for Pmod2 (J25): watch P602/P603 across a reset.
 *
 * @details The mirror of ::probe_rung_edge_sampler for the *other*
 * software-reachable Pmod. Asserts Pmod2 RTS (P604) LOW, releases P602/P603
 * to GPIO inputs, pulses the Pmod2 RESET (P410), and tight-samples PORT6's
 * PIDR across the boot window. If the DA16600 is seated on Pmod2 instead of
 * Pmod1, its boot-log edges appear here -- proving a wrong-connector config
 * rather than a dead module, with no board change.
 *
 * @pre Console is up; ::ra_time_init has been called.
 * @pre PORT6 P602/P603/P604 and P410 are claimable.
 * @post One report line has been printed.
 * @post P602/P603 are released; P604 stays driven LOW; P410 driven HIGH.
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_pmod2_edge(void)
{
  (void)ra_gpio_output_init(k_probe_p2_rts, k_ra_level_low);
  if (ra_gpio_input_init(k_probe_p2_rxd, k_ra_pull_up) != k_ra_ok) {
    probe_log("pmod2: rxd claim FAIL\r\n");
    return;
  }
  (void)ra_gpio_input_init(k_probe_p2_txd, k_ra_pull_up);
  if (ra_gpio_output_init(k_probe_p2_reset, k_ra_level_low) == k_ra_ok) {
    ra_delay_ms((uint32_t)k_probe_reset_pulse_ms);
    (void)ra_gpio_write(k_probe_p2_reset, k_ra_level_high);
  }
  volatile r_port_regs_t* p6    = ra_port(k_ra_port_6);
  edge_track_t            rx    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  edge_track_t            tx    = {.edges = 0U, .min_lo = UINT32_MAX, .run = 0U, .prev = 1U};
  uint32_t                iters = 0U;
  const uint32_t          t0    = ra_time_ms();
  while ((ra_time_ms() - t0) < (uint32_t)k_probe_edge_window_ms) {
    const uint32_t now = p6->PCNTR2;
    probe_edge_step(&rx, (now >> (uint32_t)k_probe_p602_pin_bit) & 1U);
    probe_edge_step(&tx, (now >> (uint32_t)k_probe_p603_pin_bit) & 1U);
    iters++;
  }
  (void)ra_pin_validator_release(k_probe_p2_rxd);
  (void)ra_pin_validator_release(k_probe_p2_txd);
  char digits[k_probe_u16_str_len];
  probe_log("pmod2: iters/ms=");
  probe_format_u16(digits, (uint16_t)(iters / (uint32_t)k_probe_edge_window_ms));
  probe_log(digits);
  probe_log(" ");
  probe_edge_report("P602", &rx);
  probe_log(" |");
  probe_edge_report(" P603", &tx);
  probe_log("\r\n");
}

/**
 * @brief Alt-UART hunt: probe SCI7 on the Arduino D0/D1 header pins.
 *
 * @details The module may be jumper-wired to the Arduino header rather
 * than seated in Pmod1. D0 = P808 = RXD7_B, D1 = P809 = TXD7_B (PSEL
 * 00101b, EK UM Table 20). Routes the pair, fires ``AT`` at 115200 and
 * 230400, and reports whether anything answers.
 *
 * @pre Console is up; P808/P809 are claimable.
 * @pre ::ra_time_init has been called.
 * @post Two verdict lines have been printed.
 * @post SCI7 stays configured (harmless).
 * @note Diagnostic only.
 * @since 0.1.0
 */
static void probe_rung_arduino_uart(void)
{
  const ra_port_pin_t pin_tx = (ra_port_pin_t)k_ra_board_arduino_d1;
  const ra_port_pin_t pin_rx = (ra_port_pin_t)k_ra_board_arduino_d0;
  if (ra_pfs_route_peripheral(pin_tx, k_ra_psel_sci_sync, "ard.txd7") != k_ra_ok) {
    probe_log("ard: txd7 route FAIL\r\n");
    return;
  }
  if (ra_pfs_route_peripheral(pin_rx, k_ra_psel_sci_sync, "ard.rxd7") != k_ra_ok) {
    probe_log("ard: rxd7 route FAIL\r\n");
    return;
  }
  for (uint32_t b = 0U; b < 2U; b++) {
    const ra_sci_cfg_t cfg = {
      .baud      = (b == 0U) ? (uint32_t)k_probe_baud : (uint32_t)k_probe_baud_alt1,
      .data_bits = k_ra_sci_data_8,
      .parity    = k_ra_sci_parity_none,
      .stop_bits = k_ra_sci_stop_1,
      .pclk_hz   = s_pclka_hz,
    };
    if (ra_sci_init((uint8_t)k_probe_sci7_ch, &cfg) != k_ra_ok) {
      probe_log("ard: sci7 init FAIL\r\n");
      return;
    }
    uint8_t drain = 0U;
    while (ra_sci_getc_polling((uint8_t)k_probe_sci7_ch, &drain) == k_ra_ok) {
    }
    static const char at_cmd[] = "AT\r\n";
    for (uint32_t i = 0U; at_cmd[i] != '\0'; i++) {
      (void)ra_sci_putc_polling((uint8_t)k_probe_sci7_ch, (uint8_t)at_cmd[i]);
    }
    const uint32_t start = ra_time_ms();
    uint8_t        got   = 0U;
    while ((ra_time_ms() - start) < (uint32_t)k_probe_quick_at_ms) {
      uint8_t byte = 0U;
      if (ra_sci_getc_polling((uint8_t)k_probe_sci7_ch, &byte) == k_ra_ok) {
        got = 1U;
        break;
      }
    }
    probe_log("ard: sci7 ");
    probe_log((b == 0U) ? "115200" : "230400");
    probe_log((got == 1U) ? " RX TRAFFIC\r\n" : " silent\r\n");
  }
}

/**
 * @brief Rung 1: probe the module with a bare ``AT``.
 *
 * @details Wires the byte transport into ::ra_da16600_init, which sends
 * ``AT`` and expects ``OK`` (UM-WI-046 section 2.1).
 *
 * @return ::ra_err_t from ::ra_da16600_init.
 * @retval k_ra_ok Module answered OK.
 *
 * @pre ::probe_setup_or_halt has run.
 * @pre The DA16600 has had its boot settle window.
 * @post On k_ra_ok the AT facade is ready for Wi-Fi / BLE calls.
 * @post On failure the module state is undefined.
 * @note Blocking up to the probe timeout.
 * @since 0.1.0
 */
/**
 * @brief Pulse the DA16600 host-wake lines (Pmod1 GPIO_A/GPIO_B) Low->High->Low.
 *
 * @details The DA16200/DA16600 RTC_WAKE_UP input wakes the module from DPM
 * (Dynamic Power Management) sleep on a Low->High->Low transition; UART RX
 * does NOT wake it. A module that booted into DPM idles its TX high and
 * ignores AT entirely -- exactly the "powered, mute, zero edges" symptom.
 * We do not know which of GPIO_A (P412) / GPIO_B (P413) is wired to
 * RTC_WAKE_UP on this Pmod, so pulse both before every AT attempt.
 *
 * @pre Console + ::ra_time_init are up; P412/P413 are claimable.
 * @post Both lines have seen ::k_probe_wake_cycles L->H->L pulses, left LOW.
 * @post Both pins are released for re-claim on the next pass.
 * @note Diagnostic / bring-up; harmless if the pins are not the wake line.
 * @since 0.1.0
 */
static void probe_wake_pulse(void)
{
  const ra_port_pin_t pins[2] = {(ra_port_pin_t)k_ra_board_pmod1_gpio_a,
                                 (ra_port_pin_t)k_ra_board_pmod1_gpio_b};
  for (uint8_t p = 0U; p < 2U; p++) {
    (void)ra_pin_validator_release(pins[p]);
    if (ra_gpio_output_init(pins[p], k_ra_level_low) != k_ra_ok) {
      continue;
    }
    for (uint8_t c = 0U; c < (uint8_t)k_probe_wake_cycles; c++) {
      (void)ra_gpio_write(pins[p], k_ra_level_low);
      ra_delay_ms((uint32_t)k_probe_wake_pulse_ms);
      (void)ra_gpio_write(pins[p], k_ra_level_high);
      ra_delay_ms((uint32_t)k_probe_wake_pulse_ms);
      (void)ra_gpio_write(pins[p], k_ra_level_low);
      ra_delay_ms((uint32_t)k_probe_wake_pulse_ms);
    }
    (void)ra_pin_validator_release(pins[p]);
  }
  probe_log("wake: GPIO_A/B pulsed L-H-L\r\n");
  ra_delay_ms((uint32_t)k_probe_wake_settle_ms);
}

static ra_err_t probe_rung_alive(void)
{
  const ra_da16600_cfg_t cfg = {
    .io                 = {.tx_byte = probe_da16600_tx,
                           .rx_byte = probe_da16600_rx,
                           .now_ms  = probe_da16600_now_ms,
                           .ctx     = nullptr},
    .line_buf           = s_at_line,
    .line_buf_len       = (uint16_t)k_probe_at_line_buf_len,
    .default_timeout_ms = (uint16_t)k_probe_at_timeout_ms,
  };
  return ra_da16600_init(&cfg);
}

/**
 * @brief Rung 2: run a Wi-Fi scan and print the BSS count.
 *
 * @details Wraps ::ra_da16600_wifi_scan and prints the result line.
 *
 * @return ::ra_err_t from the scan.
 * @retval k_ra_ok Scan finished (count may be 0).
 *
 * @pre Rung 1 passed.
 * @pre Console is initialized.
 * @post The scan result line has been printed.
 * @post No persistent state changes.
 * @note Blocking; a full sweep takes seconds.
 * @since 0.1.0
 */
static ra_err_t probe_rung_scan(void)
{
  uint16_t count = 0U;
  ra_err_t err   = ra_da16600_wifi_scan(&count);
  if (err != k_ra_ok) {
    return err;
  }
  char digits[k_probe_u16_str_len];
  probe_format_u16(digits, count);
  probe_log("wifi: scan N=");
  probe_log(digits);
  probe_log("\r\n");
  return k_ra_ok;
}

/**
 * @brief Rung 3: start + stop BLE advertising.
 *
 * @details Wraps ::ra_da16600_ble_advertise_start / _stop.
 *
 * @return ::ra_err_t from the first failing call.
 * @retval k_ra_ok Both calls answered OK.
 *
 * @pre Rung 1 passed.
 * @pre Console is initialized.
 * @post Advertising has been started and stopped once.
 * @post No persistent state changes.
 * @note Blocking up to the BLE timeout per call.
 * @since 0.1.0
 */
static ra_err_t probe_rung_ble(void)
{
  ra_err_t err = ra_da16600_ble_advertise_start();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_da16600_ble_advertise_stop();
}

/**
 * @brief Rung 4: join the AP fixture and report the DHCP lease.
 *
 * @details Wraps ::ra_da16600_wifi_connect against the hil_lab fixture
 * (Pi hostapd, scripts/hil_da16600_setup.md) and prints the IPv4
 * address on success.
 *
 * @return ::ra_err_t from the join.
 * @retval k_ra_ok Associated with a lease.
 *
 * @pre Rung 1 (alive) passed.
 * @pre The AP fixture is on the air.
 * @post On success the lease line has been printed.
 * @post On failure the module state is unchanged for retry.
 * @note Blocking up to the driver's connect timeout.
 * @since 0.1.0
 */
static ra_err_t probe_rung_wifi_join(void)
{
  char           ip[k_ra_da16600_ip_str_len] = {};
  const ra_err_t err = ra_da16600_wifi_connect(k_probe_ssid, k_probe_pass, ip, sizeof ip);
  if (err != k_ra_ok) {
    return err;
  }
  probe_log("wifi: joined ");
  probe_log(k_probe_ssid);
  probe_log(" ip=");
  probe_log(ip);
  probe_log("\r\n");
  return k_ra_ok;
}

/**
 * @brief Rung 5: accept one TCP echo round on port 7.
 *
 * @details Opens a listening socket (AT+TRTS), waits one recv window
 * for an inbound payload, echoes it back verbatim, and closes. The HIL
 * client on the fixture network drives the round trip.
 *
 * @return ::ra_err_t from the first failing step.
 * @retval k_ra_ok One payload was echoed.
 *
 * @pre Rung 4 (join) passed.
 * @pre A client can reach the module's lease address.
 * @post The socket has been closed.
 * @post On success "tcp: echoed N" was printed.
 * @note Blocking for up to the recv window.
 * @since 0.1.0
 */
static ra_err_t probe_rung_tcp_echo(void)
{
  ra_da16600_socket_t sock = 0U;
  ra_err_t            err =
    ra_da16600_tcp_open(k_ra_da16600_socket_listen, "", (uint16_t)k_probe_echo_port, &sock);
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t payload[k_probe_echo_buf_len];
  size_t  got = 0U;
  err = ra_da16600_tcp_recv(sock, payload, sizeof payload, &got, (uint16_t)k_probe_recv_window_ms);
  if (err == k_ra_ok) {
    if (got > 0U) {
      err = ra_da16600_tcp_send(sock, payload, got);
      if (err == k_ra_ok) {
        char digits[k_probe_u16_str_len];
        probe_format_u16(digits, (uint16_t)got);
        probe_log("tcp: echoed ");
        probe_log(digits);
        probe_log(" bytes\r\n");
      }
    }
  }
  (void)ra_da16600_tcp_close(sock);
  return err;
}

/**
 * @brief Report one rung's outcome on the console.
 *
 * @details Prints ``<name> ok`` or ``<name> FAIL err=<hex>``.
 *
 * @param[in] name Rung label.
 * @param[in] err  Outcome code.
 * @return 1 when the rung passed, else 0.
 * @retval 1 Rung passed.
 * @retval 0 Rung failed.
 *
 * @pre Console is initialized.
 * @pre @p name is non-NULL.
 * @post One status line has been printed.
 * @post No other state changes.
 * @note Helper keeps main under the size gate.
 * @since 0.1.0
 */
static uint8_t probe_report(const char* name, ra_err_t err)
{
  probe_log(name);
  if (err == k_ra_ok) {
    probe_log(" ok\r\n");
    return 1U;
  }
  probe_log(" FAIL err=");
  probe_log_hex16((uint16_t)err);
  probe_log("\r\n");
  return 0U;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: climb the DA16600 bring-up ladder forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR / FPU / priority grouping.
 * @post Ladder status lines stream on the VCOM console.
 * @post CPU parks in WFI between passes.
 * @note Each pass re-probes so a hot-plugged module recovers.
 * @since 0.1.0
 */
int32_t main(void)
{
  probe_setup_or_halt();
  ra_isr_globals_enable();
  probe_log("\r\nda16600: probe boot\r\n");
  probe_rung_u15_readback();
  probe_u15_drive_test();
  probe_rung_wire();
  probe_rung_edge_sampler();
  probe_rung_pmod2_edge();
  probe_uart_setup_or_halt();
  probe_rung_sci2_self();
  probe_rung_arduino_uart();
  probe_rung_reset_banner();
  probe_rung_u15_sweep();
  ra_delay_ms((uint32_t)k_probe_boot_settle_ms);
  uint8_t swept = 0U;

  uint8_t deep = 0U;
  while (1) {
    probe_replay_bootlog();
    if (deep == 0U) {
      deep = 1U;
      probe_rung_u15_full_sweep();
    }
    probe_wake_pulse();
    const ra_err_t alive = probe_rung_alive();
    if (alive != k_ra_ok) {
      if (swept == 0U) {
        swept = 1U;
        probe_sweep_baud((uint32_t)k_probe_baud_alt1);
        probe_sweep_baud((uint32_t)k_probe_baud_alt2);
        probe_sweep_baud((uint32_t)k_probe_baud_alt3);
        probe_sweep_baud((uint32_t)k_probe_baud_alt4);
        probe_sweep_baud((uint32_t)k_probe_baud);
      }
    }
    if (probe_report("da16600: alive", alive) == 1U) {
      const ra_err_t scan = probe_rung_scan();
      (void)probe_report("wifi: scan", scan);
      const ra_err_t ble = probe_rung_ble();
      (void)probe_report("ble: adv", ble);
      const ra_err_t join = probe_rung_wifi_join();
      (void)probe_report("wifi: join", join);
      if (join == k_ra_ok) {
        const ra_err_t echo = probe_rung_tcp_echo();
        (void)probe_report("tcp: echo", echo);
      }
    }
    ra_delay_ms((uint32_t)k_probe_pass_period_ms);
  }

  return 0;
}
#pragma GCC diagnostic pop
