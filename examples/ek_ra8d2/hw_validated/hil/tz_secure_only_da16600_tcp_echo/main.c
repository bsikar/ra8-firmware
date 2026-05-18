/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_secure_only_da16600_tcp_echo/main.c
 * @brief DA16600 Wi-Fi TCP-echo demo for EK-RA8D2 + US159-DA16600EVZ Pmod
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Bring up the DA16600 module over SCI2 (Pmod1 UART pins per
 * EK-RA8D2 v1 UM Table 17 p 26), associate with an AP using the
 * compile-time SSID / passphrase, accept inbound TCP connections on
 * the AT-side echo port, and bounce any incoming payload back to
 * the originator.
 *
 * The HIL harness uses ``HIL_MODE=alive`` for this app until the
 * Pi-as-AP lab fixture lands -- see ``hil.conf`` for the rationale.
 *
 * SSID and passphrase can be overridden at compile time:
 *
 *   make tz_secure_only_da16600_tcp_echo \
 *     EXTRA_CFLAGS='-DDA16600_SSID="\"mywifi\"" -DDA16600_PASS="\"secret123\""'
 *
 * If unset they default to "hil_lab" / "test1234" so the binary
 * still links cleanly against the HIL fixture we will stand up next.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_da16600.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/* =============================================================================
 * Compile-time tunables
 * =============================================================================
 */

#ifndef DA16600_SSID
#define DA16600_SSID "hil_lab"
#endif

#ifndef DA16600_PASS
#define DA16600_PASS "test1234"
#endif

/**
 * @brief Compile-time tunables for the TCP-echo demo.
 *
 * @details
 *  - @c k_demo_echo_port    : Standard TCP echo port (RFC 862).
 *  - @c k_demo_recv_cap     : Max bytes echoed back per inbound frame.
 *  - @c k_demo_baud         : DA16600 default UART speed (UM-WI-046 2.1).
 *  - @c k_demo_da16600_sci  : SCI channel wired to Pmod1.UART per BSP.
 *  - @c k_demo_at_line_buf  : AT line accumulator bytes.
 */
typedef enum : uint32_t {
  k_demo_baud            = 115200U,
  k_demo_da16600_sci     = 2U,
  k_demo_at_line_buf     = 512U,
  k_demo_recv_cap        = 256U,
  k_demo_echo_port       = 7U,
  k_demo_cpu_hz_at_reset = 8400000U,
  k_demo_recv_timeout_ms = 1000U,
} da16600_tcp_const_t;

/* TXD2/RXD2 live on Pmod1.2 / Pmod1.3 (P801/P802) per EK-RA8D2 v1 UM
 * Table 17 p 26. */
static const ra_port_pin_t k_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_1);
static const ra_port_pin_t k_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_2);

/** @brief AT line buffer (caller-owned per ::ra_da16600_cfg_t contract). */
static uint8_t s_at_line[k_demo_at_line_buf];

static const uint8_t k_demo_banner_boot[]        = "da16600: tcp echo boot\r\n";
static const uint8_t k_demo_banner_assoc_fail[]  = "da16600: wifi connect failed\r\n";
static const uint8_t k_demo_banner_listen_fail[] = "da16600: tcp listen failed\r\n";
static const uint8_t k_demo_banner_ip_prefix[]   = "wifi: ip=";
static const uint8_t k_demo_banner_cr_lf[]       = "\r\n";

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Park the CPU in WFI -- used as a panic stop.
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static ra_err_t demo_da16600_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  return ra_sci_putc_polling((uint8_t)k_demo_da16600_sci, byte);
}

static ra_err_t demo_da16600_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  return ra_sci_getc_polling((uint8_t)k_demo_da16600_sci, out);
}

static uint32_t demo_da16600_now_ms(void* ctx)
{
  (void)ctx;
  return ra_time_ms();
}

[[nodiscard]] static ra_err_t demo_bring_up_pins(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_demo_pin_txd, k_ra_psel_sci_async, "da16600.txd2");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_demo_pin_rxd, k_ra_psel_sci_async, "da16600.rxd2");
}

[[nodiscard]] static ra_err_t demo_bring_up_clocks_and_console(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  ra_err_t err        = ra_cgc_init();
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_time_init(cpuclk0_hz);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_board_uart_console_init((uint32_t)k_demo_baud);
}

[[nodiscard]] static ra_err_t demo_bring_up_da16600_sci(uint32_t pclka_hz)
{
  const ra_sci_cfg_t cfg = {
    .baud      = (uint32_t)k_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  return ra_sci_init((uint8_t)k_demo_da16600_sci, &cfg);
}

[[nodiscard]] static ra_err_t demo_bring_up_da16600(void)
{
  const ra_da16600_cfg_t cfg = {
    .io                 = {.tx_byte = demo_da16600_tx,
                           .rx_byte = demo_da16600_rx,
                           .now_ms  = demo_da16600_now_ms,
                           .ctx     = nullptr},
    .line_buf           = s_at_line,
    .line_buf_len       = (uint16_t)k_demo_at_line_buf,
    .default_timeout_ms = 2000U,
  };
  return ra_da16600_init(&cfg);
}

static void demo_log(const uint8_t* data, uint32_t len)
{
  (void)ra_board_uart_console_write(data, (size_t)len);
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  if (demo_bring_up_clocks_and_console(&pclka_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_bring_up_pins() != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_bring_up_da16600_sci(pclka_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  ra_isr_globals_enable();

  demo_log(k_demo_banner_boot, (uint32_t)(sizeof k_demo_banner_boot - 1U));

  if (demo_bring_up_da16600() != k_ra_ok) {
    demo_panic_halt();
  }

  char ip[k_ra_da16600_ip_str_len];
  if (ra_da16600_wifi_connect(DA16600_SSID, DA16600_PASS, ip, (size_t)k_ra_da16600_ip_str_len) !=
      k_ra_ok) {
    demo_log(k_demo_banner_assoc_fail, (uint32_t)(sizeof k_demo_banner_assoc_fail - 1U));
    demo_panic_halt();
  }

  /* Emit "wifi: ip=<dotted>\r\n" so the HIL Pi-AP fixture can scrape
   * it and learn where to send echo probes. */
  demo_log(k_demo_banner_ip_prefix, (uint32_t)(sizeof k_demo_banner_ip_prefix - 1U));
  uint32_t ip_len = 0U;
  while (ip[ip_len] != '\0') {
    ++ip_len;
  }
  demo_log((const uint8_t*)ip, ip_len);
  demo_log(k_demo_banner_cr_lf, (uint32_t)(sizeof k_demo_banner_cr_lf - 1U));

  ra_da16600_socket_t listener = 0U;
  if (ra_da16600_tcp_open(k_ra_da16600_socket_listen,
                          nullptr,
                          (uint16_t)k_demo_echo_port,
                          &listener) != k_ra_ok) {
    demo_log(k_demo_banner_listen_fail, (uint32_t)(sizeof k_demo_banner_listen_fail - 1U));
    demo_panic_halt();
  }

  /* Echo loop: block on recv, bounce the bytes back via send. */
  uint8_t rx[k_demo_recv_cap];
  while (1) {
    size_t got = 0U;
    if (ra_da16600_tcp_recv(listener,
                            rx,
                            (size_t)k_demo_recv_cap,
                            &got,
                            (uint16_t)k_demo_recv_timeout_ms) == k_ra_ok) {
      if (got > 0U) {
        (void)ra_da16600_tcp_send(listener, rx, got);
      }
    }
  }
}
#pragma GCC diagnostic pop
