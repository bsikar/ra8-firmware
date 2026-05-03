/**
 * @file examples/ek_ra8d2/lpm_idle_demo/main.c
 * @brief Sleep-mode wake-count demo for the bare EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + the LPM block, then
 * loops:
 *
 *   1. ``ra_lpm_enter_sleep(k_ra_sleep_mode_sleep)`` -- the CPU
 *      idles in WFI; SysTick keeps running and wakes the core
 *      every 1 ms.
 *   2. ``ra_delay_ms(100)`` lets the SysTick handler bring us back
 *      ~100 wakes later.
 *   3. Increment an in-RAM ``s_wake_count``, toggle LED1, emit
 *      ``"lpm: wake_count=NNNNNNNN\r\n"`` on the J-Link OB CDC
 *      channel (115200 8N1).
 *
 * Sleep mode is the safest LPM mode for a smoke-test demo because
 * SysTick keeps running, no external wake source is required, and
 * the JLink debug probe stays attached. Software-Standby /
 * Deep-Standby would require an IRQ pin or RTC alarm wired up,
 * which the bare EVM does not provide without a shield.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_lpm_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_lpm.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_lpm_demo_baud        = 115200U,
  k_lpm_demo_period_ms   = 100U,
  k_lpm_demo_sci_channel = 8U,
} lpm_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_lpm_demo_nibble_mask  = 0x0FU,
  k_lpm_demo_nibble_shift = 4U,
  k_lpm_demo_alpha_thresh = 10U,
  k_lpm_demo_hex_per_word = 8U,
} lpm_demo_byte_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel. */
static const ra_port_pin_t k_lpm_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_lpm_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Output line tags. */
static const uint8_t k_lpm_demo_prefix[] = "lpm: wake_count=";
static const uint8_t k_lpm_demo_eol[]    = "\r\n";

/** @brief Cumulative wake count incremented after every sleep+wake cycle. */
static uint32_t s_wake_count;

/** @brief Park forever after a fatal init failure. */
static void lpm_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Convert the low nibble of ``n`` to its ASCII hex char.
 *
 * @param[in] nibble Lower 4 bits used.
 * @return Printable ASCII byte.
 *
 * @pre None.
 * @post Return value is always printable ASCII.
 *
 * @since 0.1.0
 */
static uint8_t lpm_demo_nibble_to_hex(uint8_t nibble)
{
  uint8_t n = (uint8_t)(nibble & (uint8_t)k_lpm_demo_nibble_mask);
  if (n < (uint8_t)k_lpm_demo_alpha_thresh) {
    return (uint8_t)('0' + n);
  }
  return (uint8_t)('a' + (n - (uint8_t)k_lpm_demo_alpha_thresh));
}

/** @brief Render a 32-bit value as 8 ASCII hex chars (big-endian).
 *
 * @param[in]  v   Value to render.
 * @param[out] dst 8-byte buffer that receives the hex characters.
 *
 * @pre ``dst`` is non-NULL and has at least 8 bytes capacity.
 * @post ``dst[0..7]`` contains printable hex.
 *
 * @since 0.1.0
 */
static void lpm_demo_word_to_hex(uint32_t v, uint8_t* dst)
{
  for (uint8_t i = 0U; i < (uint8_t)k_lpm_demo_hex_per_word; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_lpm_demo_hex_per_word - 1U - i) * (uint8_t)k_lpm_demo_nibble_shift);
    const uint8_t nibble = (uint8_t)((v >> shift) & (uint32_t)k_lpm_demo_nibble_mask);
    dst[i]               = lpm_demo_nibble_to_hex(nibble);
  }
}

/** @brief Route PD_02 / PD_03 to SCI8 TXD/RXD via PFS.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 * @retval k_ra_ok                       Both pins routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port out of range.
 *
 * @pre IOPORT module reachable.
 * @post On success PD_02 + PD_03 in SCI-async mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lpm_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_lpm_demo_pin_txd, k_ra_psel_sci_async, "lpm_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_lpm_demo_pin_rxd, k_ra_psel_sci_async, "lpm_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LED1 + LPM up. */
static void lpm_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  if (lpm_demo_pins_init() != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_lpm_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_lpm_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    lpm_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    lpm_demo_panic_halt();
  }

  /* LPM block uses the cold-reset defaults: OPE=1, IOKEEP=0, no
   * fast-return / SS2LP options. ra_lpm_init takes care of the
   * PRC1 unlock + SBYCR / DPSBYCR / SSCR1 programming for us. */
  const ra_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra_lpm_ss2lp_default,
  };
  if (ra_lpm_init(&lpm_cfg) != k_ra_ok) {
    lpm_demo_panic_halt();
  }
}

/**
 * @brief Sleep + wake + emit one wake-count line.
 *
 * @par MC/DC:
 * Compound decision: ``enter_sleep != ok || sci_writes != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- both-ok runtime path
 * + each error branch covered by the host integration test.
 *
 * @return Error code from the first failing primitive.
 *
 * @pre lpm_demo_setup_or_halt() returned ok.
 * @post On success the wake-count line was transmitted and
 *       ``s_wake_count`` was incremented.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lpm_demo_one_wake(void)
{
  /* Sleep mode -- WFI returns on the next SysTick IRQ ~1 ms later. */
  if (ra_lpm_enter_sleep(k_ra_sleep_mode_sleep) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  /* Continue idling until ~100 ms have accumulated. */
  ra_delay_ms((uint32_t)k_lpm_demo_period_ms);
  s_wake_count++;

  uint8_t hex[k_lpm_demo_hex_per_word] = {};
  lpm_demo_word_to_hex(s_wake_count, hex);

  if (ra_sci_write_polling((uint8_t)k_lpm_demo_sci_channel,
                           k_lpm_demo_prefix,
                           (uint32_t)(sizeof(k_lpm_demo_prefix) - 1U)) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  if (ra_sci_write_polling((uint8_t)k_lpm_demo_sci_channel,
                           hex,
                           (uint32_t)k_lpm_demo_hex_per_word) != k_ra_ok) {
    return k_ra_err_hw_error;
  }
  return ra_sci_write_polling((uint8_t)k_lpm_demo_sci_channel,
                              k_lpm_demo_eol,
                              (uint32_t)(sizeof(k_lpm_demo_eol) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lpm_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    if (lpm_demo_one_wake() != k_ra_ok) {
      break;
    }
    (void)ra_board_led_toggle(k_ra_board_led1);
  }
  lpm_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
