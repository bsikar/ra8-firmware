/**
 * @file examples/ek_ra8d2/hw_validated/hil/agt_cascade_demo/main.c
 * @brief AGT0 + AGT1 cascade (32-bit virtual counter) demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + UART (SCI8 on PD_02 / PD_03) and arms the
 * AGT0 + AGT1 pair as a 32-bit cascade via the new
 * `ra_agt_start_cascade` HAL surface. AGT0 in plain timer mode counts
 * PCLKB; its underflow signal feeds AGT1 (TCK[2:0] = 101b per HUM
 * Ch 24.2.5 "AGTMR1" p 1168 note 6). The effective virtual period is
 * ``(reload32 + 1)`` PCLKB cycles.
 *
 * The demo polls AGT1's AGTCR.TUNDF in the foreground loop. Each
 * AGT1 underflow:
 *   1. Stops + re-arms the cascade to clear AGT1.AGTCR.TUNDF.
 *   2. Toggles board LED1 so a scope / human can see the cadence.
 *   3. Emits ``agt_cas: tick=XXXXXXXX\r\n`` over SCI8.
 *
 * The HIL gate scrapes ``agt_cas: tick=`` to confirm bring-up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_agt_regs.h"
#include "ra_agt.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_agt_cas_baud        = 115200U,
  k_agt_cas_sci_channel = 8U,
  k_agt_cas_poll_ms     = 10U,
  /* 32-bit virtual reload. Low 16 bits go to AGT0, high 16 bits to
   * AGT1. Keep it small enough that AGT1 underflows within the HIL
   * timeout but large enough to demonstrate the cascade chain. */
  k_agt_cas_reload32 = 0x00017FFFU,
} agt_cas_const_t;

/** @brief Hex-digit width + mask for the per-tick banner formatter. */
typedef enum : uint32_t {
  k_agt_cas_hex_digits = 8U,
  k_agt_cas_hex_shift  = 4U,
  k_agt_cas_hex_mask   = 0xFU,
} agt_cas_fmt_t;

/** @brief AGT1 = "high half" channel; its TUNDF flags a full cascade tick. */
typedef enum : uint8_t {
  k_agt_cas_hi_channel = 1U,
} agt_cas_chan_t;

/** @brief SCI8 pin map -- mirrors agt_periodic and uart_hello. */
static const ra_port_pin_t k_agt_cas_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_agt_cas_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Hex print map for the tick counter. */
static const char k_agt_cas_hex_chars[] = "0123456789ABCDEF";

/** @brief Tick-line prefix and CR/LF tail. */
static const uint8_t k_agt_cas_msg_head[] = "agt_cas: tick=";
static const uint8_t k_agt_cas_msg_tail[] = "\r\n";

/**
 * @brief Spin-halt the core if any init step fails.
 *
 * @details
 * Boot helper. Uses `wfi` so an attached debugger can break in and
 * inspect the failed init.
 *
 * @pre Called only from `main()` / the boot path.
 * @pre No caller relies on the function returning.
 *
 * @post Core is halted in a tight `wfi` loop.
 * @post No registers other than `pc` change after the first `wfi`.
 *
 * @note Not thread-safe (boot path).
 * @since 0.1.0
 */
static void agt_cas_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route SCI8 TXD / RXD onto P13_02 / P13_03.
 *
 * @details
 * Wraps `ra_pfs_route_peripheral` for the two SCI pins.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok              Both pins routed.
 * @retval k_ra_err_invalid_arg PFS rejected the pin map.
 *
 * @pre IOPORT power gate is open (CGC has run).
 * @pre PFS PWPR has been unlocked by the caller.
 *
 * @post P13_02 / P13_03 are owned by SCI8.
 * @post PFS PWPR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t agt_cas_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_agt_cas_pin_txd, k_ra_psel_sci_async, "agt_cas.txd");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_agt_cas_pin_rxd, k_ra_psel_sci_async, "agt_cas.rxd");
}

/**
 * @brief Bring up CGC, SysTick, SCI8 and LED1.
 *
 * @details
 * Sequence: ra_cgc_init -> ra_time_init -> ra_pfs_route -> ra_sci_init
 * -> ra_board_led_init. Any failure goes straight to panic-halt so the
 * HIL gate sees no `agt_cas:` banner.
 *
 * @pre Called once from `main()` before `ra_isr_globals_enable`.
 * @pre Stack is sized for the SCI bring-up call chain.
 *
 * @post All peripherals named above are powered, configured, and ready.
 * @post No mutable global beyond CGC / SCI driver state is touched.
 *
 * @note Not thread-safe (boot path).
 * @since 0.1.0
 */
static void agt_cas_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    agt_cas_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    agt_cas_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    agt_cas_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    agt_cas_panic_halt();
  }
  if (agt_cas_pins_init() != k_ra_ok) {
    agt_cas_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_agt_cas_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_agt_cas_sci_channel, &sci_cfg) != k_ra_ok) {
    agt_cas_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    agt_cas_panic_halt();
  }
}

/**
 * @brief Arm AGT0 + AGT1 as a 32-bit cascade.
 *
 * @par MC/DC:
 * Single atomic decision: ``ra_agt_start_cascade != ok``. Two vectors
 * -- happy path (HIL bench) and the bad-clock reject path covered in
 * `tests/test_ra_agt.c`.
 *
 * @return ``ra_err_t`` from the HAL call.
 *
 * @pre CGC + SCI8 + LED bring-up has finished.
 * @pre IRQs are masked or the demo is single-threaded.
 *
 * @post AGT0 and AGT1 are both running, AGT0 underflow -> AGT1
 *       count source per HUM Ch 24.2.5 p 1168 note 6.
 * @post AGT1 AGTCR.TSTART reads 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t agt_cas_arm(void)
{
  const ra_agt_cascade_cfg_t cfg = {
    .reload32     = (uint32_t)k_agt_cas_reload32,
    .clock        = k_ra_agt_cascade_clk_pclkb,
    .on_underflow = nullptr,
    .ctx          = nullptr,
  };
  return ra_agt_start_cascade(&cfg);
}

/**
 * @brief Render the 32-bit tick counter into an 8-byte ASCII-hex buf.
 *
 * @details
 * Big-endian hex render: digits[0] = most-significant nibble. Caller
 * passes a buffer >= 8 bytes long.
 *
 * @param[in]  tick    Tick counter value to render.
 * @param[out] digits  Destination buffer (>= 8 bytes).
 *
 * @pre ``digits`` is non-NULL and >= 8 bytes.
 * @pre The caller will not interpret ``digits`` as NUL-terminated.
 *
 * @post ``digits`` contains the 8 ASCII hex digits of ``tick``.
 * @post No global state is touched.
 *
 * @note Not thread-safe (caller owns the buffer).
 * @since 0.1.0
 */
static void agt_cas_format_tick(uint32_t tick, uint8_t* digits)
{
  for (uint32_t i = 0U; i < (uint32_t)k_agt_cas_hex_digits; ++i) {
    const uint32_t shift =
      ((uint32_t)k_agt_cas_hex_digits - 1U - i) * (uint32_t)k_agt_cas_hex_shift;
    const uint32_t nib = (tick >> shift) & (uint32_t)k_agt_cas_hex_mask;
    digits[i]          = (uint8_t)k_agt_cas_hex_chars[nib];
  }
}

/**
 * @brief Emit one cascade-tick banner over SCI8.
 *
 * @details
 * Format: ``agt_cas: tick=XXXXXXXX\r\n``. The HIL probe matches on
 * the ``agt_cas: tick=`` prefix so any non-zero tick passes.
 *
 * @param[in] tick Current tick counter value.
 *
 * @return ``ra_err_t`` from the SCI driver (first failure wins).
 *
 * @pre SCI8 has been initialised.
 * @pre ``tick`` fits in 32 bits.
 *
 * @post Up to 24 bytes have been pushed into the SCI8 TX path.
 * @post No mutable global is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t agt_cas_emit_tick(uint32_t tick)
{
  uint8_t digits[k_agt_cas_hex_digits] = {};
  agt_cas_format_tick(tick, digits);
  ra_err_t err = ra_sci_write_polling((uint8_t)k_agt_cas_sci_channel,
                                      k_agt_cas_msg_head,
                                      (uint32_t)(sizeof(k_agt_cas_msg_head) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_sci_write_polling((uint8_t)k_agt_cas_sci_channel, digits, sizeof(digits));
  if (err != k_ra_ok) {
    return err;
  }
  return ra_sci_write_polling((uint8_t)k_agt_cas_sci_channel,
                              k_agt_cas_msg_tail,
                              (uint32_t)(sizeof(k_agt_cas_msg_tail) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  agt_cas_setup_or_halt();
  ra_isr_globals_enable();

  if (agt_cas_arm() != k_ra_ok) {
    agt_cas_panic_halt();
  }

  uint32_t tick = 0U;
  while (1) {
    uint8_t status = 0U;
    if (ra_agt_get_status((uint8_t)k_agt_cas_hi_channel, &status) != k_ra_ok) {
      break;
    }
    if ((status & (uint8_t)k_ra_agt_agtcr_tundf_msk) != 0U) {
      if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
        break;
      }
      ++tick;
      if (agt_cas_emit_tick(tick) != k_ra_ok) {
        break;
      }
      /* Stop both halves + re-arm to clear AGT1.AGTCR.TUNDF. */
      if (ra_agt_stop(0U) != k_ra_ok) {
        break;
      }
      if (ra_agt_stop((uint8_t)k_agt_cas_hi_channel) != k_ra_ok) {
        break;
      }
      if (agt_cas_arm() != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms((uint32_t)k_agt_cas_poll_ms);
  }
  agt_cas_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
