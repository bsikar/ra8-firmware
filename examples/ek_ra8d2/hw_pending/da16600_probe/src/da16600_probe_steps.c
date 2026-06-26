/**
 * @file examples/ek_ra8d2/hw_pending/da16600_probe/src/da16600_probe_steps.c
 * @brief Physical-layer diagnostic rungs for the DA16600 bring-up probe.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of the probe ``main.c`` to keep every translation unit under the
 * file-size gate. These rungs survey the raw wires (pin-level GPIO reads,
 * edge sampling across a module reset), poke the U15 PI4IOE5V6408 expander
 * latch that selects the SW4 board routing, sweep candidate baud rates, and
 * pulse the host-wake lines. They are diagnostic only: results are human-read
 * over the J-Link OB VCOM console. ``main.c`` owns the AT transport and the
 * Wi-Fi / BLE / TCP ladder; the seam between the two is
 * ``da16600_probe_steps.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "da16600_probe_steps.h"

#include <stdint.h>

#include "ra8d2_port_regs.h"
#include "ra8d2_sci_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_i2c.h"
#include "ra_pin_validator.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/* =============================================================================
 * Private state and pin map
 * =============================================================================
 */

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

/** @brief Boot-window RX capture (replayed every ladder pass). */
static uint8_t s_bootlog[k_probe_at_line_buf_len];
/** @brief Bytes captured into ::s_bootlog. */
static uint16_t s_bootlog_len;

/* =============================================================================
 * Rung 0: physical-layer diagnostics on the DA16600 UART.
 * =============================================================================
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

void probe_rung_wire(void)
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
void probe_sweep_baud(uint32_t baud)
{
  const ra_sci_cfg_t cfg = {
    .baud      = baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = s_da16600_probe_pclka_hz,
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
void probe_rung_u15_readback(void)
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

void probe_u15_drive_test(void)
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
void probe_rung_reset_banner(void)
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
void probe_replay_bootlog(void)
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
void probe_rung_u15_sweep(void)
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
void probe_rung_u15_full_sweep(void)
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
void probe_rung_sci2_self(void)
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

/* =============================================================================
 * Edge sampler (GPIO logic-analyzer)
 * =============================================================================
 */

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
void probe_rung_edge_sampler(void)
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
void probe_rung_pmod2_edge(void)
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
void probe_rung_arduino_uart(void)
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
      .pclk_hz   = s_da16600_probe_pclka_hz,
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
void probe_wake_pulse(void)
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
