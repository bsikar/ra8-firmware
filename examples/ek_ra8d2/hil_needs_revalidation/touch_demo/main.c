/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/touch_demo/main.c
 * @brief Standalone GoodIX GT911 capacitive-touch bring-up demo + HIL (#122).
 *
 * @details
 * `ra8_touch` (GT911 over IIC_B channel 0) was only ever exercised *inside*
 * `ereader_ui` -- there was no standalone example and no CI gate for the touch
 * driver itself. This app is that gate: it brings up the GT911 end-to-end
 * (I2C probe + product-id wake), then polls for a touch frame and reports the
 * first decoded contact on the SCI8 J-Link OB console:
 *
 *   `touch: open=OK pts=1 x=<X> y=<Y>`
 *
 * The bring-up half (`open=OK`) is the deterministic, finger-free part of the
 * gate -- it proves the real `ra8_touch_open` -> IIC_B -> GT911 path reached the
 * product-id check. The point half is exercised under `ra8_emulator`, which models
 * the GT911 on the modelled I2C bus and injects a tap via `--click X Y`; that
 * tap returns through the genuine `ra8_touch_read` decode, so the banner carries
 * a real decoded coordinate. On a bare bench with no finger the poll loop times
 * out and reports `pts=0`, but `open=OK` still holds -- so `hil.conf` asserts
 * only the bring-up substring, which is stable either way.
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_touch.h"

/** @enum td_consts_t @brief Console / GT911 / poll knobs (no magic numbers). */
typedef enum : uint32_t {
  k_td_uart_baud  = 115200U,   /**< Console baud.                      */
  k_td_i2c_chan   = 0U,        /**< IIC_B channel 0 (GT911 bus).       */
  k_td_i2c_bus_hz = 400000U,   /**< Fast-mode I2C clock.               */
  k_td_pclka_hz   = 60000000U, /**< IIC_B clock-source rate.           */
  k_td_gt911_addr = 0x5DU,     /**< GT911 default 7-bit address.       */
  k_td_max_points = 5U,        /**< Read up to the GT911 capacity.     */
  k_td_poll_max   = 20000U,    /**< Bounded poll iterations (NASA R2). */
  k_td_dec_ten    = 10U,       /**< Decimal radix / small-buf cap.     */
} td_consts_t;

/**
 * @var s_touch_bus
 * @brief Bound I2C bus handle the touch driver's injected seam points at.
 *
 * @details
 * File-scope because the seam's `ctx` references it for the whole run.
 *
 * @note Written once during bring-up, then read-only.
 * @warning Do not rebind while the touch driver is open.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_touch_bus;

static const uint8_t k_msg_boot[] = "touch-demo: boot\r\n";
static const uint8_t k_msg_fail[] = "touch-demo: FAIL init\r\n";
static const uint8_t k_msg_open[] = "touch: FAIL open\r\n";
static const uint8_t k_msg_pre[]  = "touch: open=OK pts=";
static const uint8_t k_msg_x[]    = " x=";
static const uint8_t k_msg_y[]    = " y=";
static const uint8_t k_msg_eol[]  = "\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void td_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void td_panic_halt(const uint8_t* msg, uint32_t len)
{
  td_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer in decimal. */
static void td_print_uint(uint32_t value)
{
  uint8_t  buf[k_td_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_td_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_td_dec_ten));
    n++;
    value /= (uint32_t)k_td_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    td_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Poll the GT911 for one touch frame; report the first contact.
 *
 * @details
 * Calls `ra8_touch_read` up to ::k_td_poll_max times (statically bounded).
 * ra8_emulator re-arms an injected `--click` each SysTick chunk until the
 * firmware drains it, so the loop catches the tap within a few chunks; on a
 * bare bench with no finger it exhausts and reports zero points.
 *
 * @param[out] out_x Receives the first contact's X (0 if none).
 * @param[out] out_y Receives the first contact's Y (0 if none).
 * @return Number of points seen in the caught frame (0 or >=1).
 */
static uint8_t td_poll_points(uint16_t* out_x, uint16_t* out_y)
{
  ra8_touch_point_t pts[k_td_max_points] = {};
  uint8_t           seen                 = 0U;
  *out_x                                 = 0U;
  *out_y                                 = 0U;
  for (uint32_t i = 0U; (i < (uint32_t)k_td_poll_max) && (seen == 0U); i++) {
    uint8_t got = 0U;
    if ((ra8_touch_read(pts, (uint8_t)k_td_max_points, &got) == k_ra8_ok) && (got >= 1U)) {
      *out_x = pts[0].x;
      *out_y = pts[0].y;
      seen   = got;
    }
  }
  return seen;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void td_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    td_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    td_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    td_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_td_uart_baud) != k_ra8_ok) {
    td_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the GT911, poll a touch frame, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The open=OK touch banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  td_setup_or_halt();
  ra8_isr_globals_enable();
  td_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  /* App-owned bus bring-up: IIC_B in I2C-compat mode, bound through the
   * ra8_io facade into the driver's injected seam. A future board revision
   * that moves the GT911 onto a RIIC channel only swaps the bind call. */
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_td_i2c_bus_hz,
    .pclka_hz = (uint32_t)k_td_pclka_hz,
  };
  ra8_i2c_bus_ops_t bus_ops = {};
  if (ra8_i3c_init((uint8_t)k_td_i2c_chan, &iic_cfg) != k_ra8_ok) {
    td_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_touch_bus, (uint8_t)k_td_i2c_chan) != k_ra8_ok) {
    td_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  if (ra8_io_i2c_bus_as_ops(&s_touch_bus, &bus_ops) != k_ra8_ok) {
    td_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  const ra8_touch_cfg_t cfg = {.bus        = bus_ops,
                               .target_7b  = (uint8_t)k_td_gt911_addr,
                               .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
                               .max_points = (uint8_t)k_td_max_points};
  if (ra8_touch_open(&cfg) != k_ra8_ok) {
    td_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  uint16_t      px   = 0U;
  uint16_t      py   = 0U;
  const uint8_t seen = td_poll_points(&px, &py);

  td_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  td_print_uint((uint32_t)seen);
  td_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  td_print_uint((uint32_t)px);
  td_print(k_msg_y, (uint32_t)sizeof(k_msg_y) - 1U);
  td_print_uint((uint32_t)py);
  td_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
