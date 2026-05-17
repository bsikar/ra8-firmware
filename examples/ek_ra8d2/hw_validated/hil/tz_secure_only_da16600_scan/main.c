/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_secure_only_da16600_scan/main.c
 * @brief DA16600 Wi-Fi scan smoke test for EK-RA8D2 + US159-DA16600EVZ Pmod
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Smallest possible end-to-end Wi-Fi-side test for the
 * US159-DA16600EVZ Pmod daughter card on Pmod1 (J26) of the
 * EK-RA8D2:
 *
 *   1. Bring CGC + SysTick + SCI8 (J-Link OB VCOM, log channel) up.
 *   2. Configure SCI2 in 8N1 mode at 115200 baud and route
 *      P801/P802 to TXD2/RXD2 -- the Pmod1 UART pin pair per
 *      EK-RA8D2 v1 UM Table 17 p 26.
 *   3. Bring ::ra_da16600 up against SCI2.
 *   4. Call ::ra_da16600_wifi_scan and print the count of networks
 *      found to the J-Link OB VCOM channel ("wifi: scan complete,
 *      N=<n>\r\n").
 *
 * The HIL harness scrapes the J-Link VCOM serial output looking
 * for the string "wifi: scan complete" -- see hil.conf.
 *
 * The full PMOD wiring + EVZ jumper setup is described in
 * scripts/hil_da16600_setup.md. This app assumes that document has
 * been followed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

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
 * Tunables (every integer constant is a typed enum -- no magic numbers).
 * =============================================================================
 */

/**
 * @brief Compile-time tunables for the scan demo.
 *
 * @details
 *  - @c k_demo_baud           : DA16600 default UART speed (UM-WI-046 2.1).
 *  - @c k_demo_da16600_sci_ch : SCI channel wired to Pmod1.UART per board
 *                               BSP (TXD2/RXD2 alternate functions).
 *  - @c k_demo_at_line_buf_len: AT-line accumulator buffer length.
 */
typedef enum : uint32_t {
  k_demo_baud           = 115200U,
  k_demo_da16600_sci_ch = 2U,
  k_demo_at_line_buf_len = 256U,
  k_demo_cpu_hz_at_reset = 8400000U,
} da16600_scan_const_t;

/* TXD2/RXD2 live on Pmod1.2 / Pmod1.3 (P801/P802) per EK-RA8D2 v1 UM
 * Table 17 p 26. */
static const ra_port_pin_t k_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_1);
static const ra_port_pin_t k_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_2);

/** @brief AT line buffer (caller-owned per ::ra_da16600_cfg_t contract). */
static uint8_t s_at_line[k_demo_at_line_buf_len];

/** @brief Banner emitted once the demo has booted. */
static const uint8_t k_demo_banner_boot[] = "da16600: scan demo boot\r\n";
/** @brief Banner emitted after a successful ::ra_da16600_wifi_scan. */
static const uint8_t k_demo_banner_done_prefix[] = "wifi: scan complete, N=";
/** @brief Trailing CR/LF appended after the count digits. */
static const uint8_t k_demo_banner_done_suffix[] = "\r\n";
/** @brief Banner emitted when the module probe fails. */
static const uint8_t k_demo_banner_init_fail[] = "da16600: init failed\r\n";
/** @brief Banner emitted when the scan itself fails. */
static const uint8_t k_demo_banner_scan_fail[] = "da16600: scan failed\r\n";

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

/**
 * @brief Decimal-format @p value into @p dst (libc-free, NUL-terminated).
 *
 * @details Worst case "65535" + NUL = 6 bytes.
 *
 * @param[out] dst   Caller buffer (must hold at least 6 bytes).
 * @param[in]  value Value to format.
 */
static void demo_format_u16(char* dst, uint16_t value)
{
  char    tmp[6];
  uint8_t ti = 0U;
  if (value == 0U) {
    dst[0] = '0';
    dst[1] = '\0';
    return;
  }
  uint16_t v = value;
  while ((v > 0U) && (ti < 5U)) {
    tmp[ti] = (char)('0' + (uint8_t)(v % 10U));
    v /= 10U;
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

/**
 * @brief Polled TX of a single byte on the DA16600 SCI channel.
 */
static ra_err_t demo_da16600_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  return ra_sci_putc_polling((uint8_t)k_demo_da16600_sci_ch, byte);
}

/**
 * @brief Polled RX of a single byte on the DA16600 SCI channel.
 */
static ra_err_t demo_da16600_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  return ra_sci_getc_polling((uint8_t)k_demo_da16600_sci_ch, out);
}

/**
 * @brief Monotonic millisecond timestamp source.
 *
 * @details Backed by ra_time's SysTick implementation; the ra_time
 * tick is in ms, so we forward directly.
 */
static uint32_t demo_da16600_now_ms(void* ctx)
{
  (void)ctx;
  return ra_time_ms();
}

/**
 * @brief Initialize CGC, SysTick, J-Link VCOM, and the DA16600 SCI.
 */
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
  return ra_sci_init((uint8_t)k_demo_da16600_sci_ch, &cfg);
}

[[nodiscard]] static ra_err_t demo_bring_up_da16600(void)
{
  const ra_da16600_cfg_t cfg = {
    .io                 = {.tx_byte = demo_da16600_tx,
                           .rx_byte = demo_da16600_rx,
                           .now_ms  = demo_da16600_now_ms,
                           .ctx     = nullptr},
    .line_buf           = s_at_line,
    .line_buf_len       = (uint16_t)k_demo_at_line_buf_len,
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
    demo_log(k_demo_banner_init_fail, (uint32_t)(sizeof k_demo_banner_init_fail - 1U));
    demo_panic_halt();
  }

  uint16_t count = 0U;
  if (ra_da16600_wifi_scan(&count) != k_ra_ok) {
    demo_log(k_demo_banner_scan_fail, (uint32_t)(sizeof k_demo_banner_scan_fail - 1U));
    demo_panic_halt();
  }

  demo_log(k_demo_banner_done_prefix, (uint32_t)(sizeof k_demo_banner_done_prefix - 1U));
  char count_str[6];
  demo_format_u16(count_str, count);
  uint16_t cs_len = 0U;
  while (count_str[cs_len] != '\0') {
    ++cs_len;
  }
  demo_log((const uint8_t*)count_str, (uint32_t)cs_len);
  demo_log(k_demo_banner_done_suffix, (uint32_t)(sizeof k_demo_banner_done_suffix - 1U));

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
