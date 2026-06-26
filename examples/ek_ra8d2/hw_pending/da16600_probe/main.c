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

#include "da16600_probe_steps.h"
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
 * Pin map and shared state
 * =============================================================================
 */

/** @brief Pmod1 UART pins (J26): TXD2 = P801, RXD2 = P802. */
static const ra_port_pin_t k_probe_pin_txd = (ra_port_pin_t)k_ra_board_pmod1_uart_txd;
static const ra_port_pin_t k_probe_pin_rxd = (ra_port_pin_t)k_ra_board_pmod1_uart_rxd;

/** @brief AT line buffer (caller-owned per ::ra_da16600_cfg_t contract). */
static uint8_t s_at_line[k_probe_at_line_buf_len];

/** @brief AP fixture credentials (scripts/hil_da16600_setup.md). */
static const char k_probe_ssid[] = "hil_lab";
/** @brief AP fixture passphrase. */
static const char k_probe_pass[] = "test1234";

uint32_t s_da16600_probe_pclka_hz;

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

void probe_log(const char* s)
{
  uint32_t n = 0U;
  while (s[n] != '\0') {
    n++;
  }
  (void)ra_board_uart_console_write((const uint8_t*)s, (size_t)n);
}

void probe_log_hex16(uint16_t v)
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

void probe_format_u16(char* dst, uint16_t value)
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
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_da16600_probe_pclka_hz) != k_ra_ok) {
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
    .pclk_hz   = s_da16600_probe_pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_probe_da16600_sci_ch, &cfg) != k_ra_ok) {
    probe_panic_halt();
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
