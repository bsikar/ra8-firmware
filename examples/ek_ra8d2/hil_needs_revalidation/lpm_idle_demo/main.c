/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/lpm_idle_demo/main.c
 * @brief Sleep-mode wake-count demo for the bare EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LED1 + the LPM block, then
 * loops:
 *
 *   1. ``ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep)`` -- the CPU
 *      idles in WFI; SysTick keeps running and wakes the core
 *      every 1 ms.
 *   2. ``ra8_delay_ms(100)`` lets the SysTick handler bring us back
 *      ~100 wakes later.
 *   3. Increment an in-RAM ``s_wake_count``, toggle LED1, emit
 *      ``"lpm: wake_count=NNNNNNNN\r\n"`` on the J-Link OB CDC
 *      channel (115200 8N1).
 *
 * Sleep mode is the safest LPM mode for a HIL test demo because
 * SysTick keeps running, no external wake source is required, and
 * the JLink debug probe stays attached. Software-Standby /
 * Deep-Standby would require an IRQ pin or RTC alarm wired up,
 * which the bare EVM does not provide without a shield.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_lpm_demo_baud      = 115200U, /**< Lpm demo baud.      */
  k_lpm_demo_period_ms = 100U,    /**< Lpm demo period ms. */
} lpm_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_lpm_demo_nibble_mask  = 0x0FU, /**< Lpm demo nibble mask.  */
  k_lpm_demo_nibble_shift = 4U,    /**< Lpm demo nibble shift. */
  k_lpm_demo_alpha_thresh = 10U,   /**< Lpm demo alpha thresh. */
  k_lpm_demo_hex_per_word = 8U,    /**< Lpm demo hex per word. */
} lpm_demo_byte_t;

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

/** @brief Bring CGC + SysTick + SCI8 + LED1 + LPM up. */
static void lpm_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    lpm_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lpm_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lpm_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_lpm_demo_baud) != k_ra8_ok) {
    lpm_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    lpm_demo_panic_halt();
  }

  /* LPM block uses the cold-reset defaults: OPE=1, IOKEEP=0, no
   * fast-return / SS2LP options. ra8_lpm_init takes care of the
   * PRC1 unlock + SBYCR / DPSBYCR / SSCR1 programming for us. */
  const ra8_lpm_config_t lpm_cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  if (ra8_lpm_init(&lpm_cfg) != k_ra8_ok) {
    lpm_demo_panic_halt();
  }
}

/**
 * @brief Sleep + wake + emit one wake-count line.
 *
 * @par MC/DC:
 * Single decision: ``ra8_lpm_enter_sleep != ok``. One atomic
 * condition x 2 vectors -- the both-ok runtime path plus the
 * sleep-failure branch covered by the host integration test. No
 * compound (N+1) vectors required.
 *
 * @return Error code from the first failing primitive.
 *
 * @pre lpm_demo_setup_or_halt() returned ok.
 * @post On success the wake-count line was transmitted and
 *       ``s_wake_count`` was incremented.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t lpm_demo_one_wake(void)
{
  /* Sleep mode -- WFI returns on the next SysTick IRQ ~1 ms later. */
  if (ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  /* Continue idling until ~100 ms have accumulated. */
  ra8_delay_ms((uint32_t)k_lpm_demo_period_ms);
  s_wake_count++;

  uint8_t hex[k_lpm_demo_hex_per_word] = {};
  lpm_demo_word_to_hex(s_wake_count, hex);

  (void)ra8_board_uart_console_write(k_lpm_demo_prefix, (size_t)(sizeof(k_lpm_demo_prefix) - 1U));
  (void)ra8_board_uart_console_write(hex, (size_t)k_lpm_demo_hex_per_word);
  (void)ra8_board_uart_console_write(k_lpm_demo_eol, (size_t)(sizeof(k_lpm_demo_eol) - 1U));
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lpm_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    if (lpm_demo_one_wake() != k_ra8_ok) {
      break;
    }
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  }
  lpm_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
