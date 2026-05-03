/**
 * @file main.c
 * @brief 1 KB DMAC SRAM-to-SRAM copy + verify demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + SCI8 + LEDs + DMAC0 channel 0. Once a
 * second:
 *
 *   1. Fills a 1 KB source buffer with a deterministic pattern
 *      (``i ^ (i >> 8)``) and zeroes the destination buffer.
 *   2. Programmes DMAC0 channel 0 for a 32-bit-wide, increment-both,
 *      single-block transfer of 256 words via ``ra_dmac_start``.
 *   3. Triggers the channel by software (``DMREQ`` set) and waits for
 *      the in-flight count (``DMCRA``) to drain to zero.
 *   4. Walks both buffers and reports ``"dma: copied 1024B match=Y\r\n"``
 *      on the J-Link OB CDC channel.
 *
 * LED1 toggles on every successful copy; LED2 latches if the
 * destination ever differs from the source.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_dmac_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dma_demo_baud        = 115200U,
  k_dma_demo_period_ms   = 1000U,
  k_dma_demo_sci_channel = 8U,
  k_dma_demo_buf_bytes   = 1024U,
  k_dma_demo_buf_words   = 256U,
  k_dma_demo_poll_limit  = 100000U,
} dma_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dma_demo_channel = 0U,
  k_dma_demo_byte_sh = 8U,
} dma_demo_byte_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel. */
static const ra_port_pin_t k_dma_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_dma_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Output line tags. */
static const uint8_t k_dma_demo_ok_msg[]  = "dma: copied 1024B match=Y\r\n";
static const uint8_t k_dma_demo_bad_msg[] = "dma: copied 1024B match=N\r\n";

/** @brief Source / destination buffers (32-bit aligned by element type). */
static uint32_t s_src[k_dma_demo_buf_words];
static uint32_t s_dst[k_dma_demo_buf_words];

/** @brief Park forever after a fatal init failure. */
static void dma_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
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
[[nodiscard]] static ra_err_t dma_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_dma_demo_pin_txd, k_ra_psel_sci_async, "dma_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_dma_demo_pin_rxd, k_ra_psel_sci_async, "dma_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void dma_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (dma_demo_pins_init() != k_ra_ok) {
    dma_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_dma_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_dma_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    dma_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    dma_demo_panic_halt();
  }
}

/**
 * @brief Fill ``s_src`` with a deterministic pattern and clear ``s_dst``.
 *
 * @par MC/DC:
 * Trivial loop with no compound decision -- only the implicit loop
 * exit condition. No N+1 vectors required.
 *
 * @pre Buffers are statically allocated.
 * @post Every word in ``s_src`` is set; ``s_dst`` is all-zero.
 *
 * @since 0.1.0
 */
static void dma_demo_fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_demo_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dma_demo_byte_sh);
    s_dst[i] = 0U;
  }
}

/**
 * @brief Verify that ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @par MC/DC:
 * Compound decision in the loop: ``s_dst[i] != s_src[i]``. One
 * atomic condition x 2 vectors -- match (steady-state) and one
 * mismatch (covered by the host integration test).
 *
 * @return 1 if all elements equal, 0 otherwise.
 *
 * @pre Buffers are filled.
 * @post Return value is 0 or 1.
 *
 * @since 0.1.0
 */
static uint8_t dma_demo_verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_demo_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Programme + trigger the DMAC channel and wait for completion.
 *
 * @par MC/DC:
 * Compound decision: ``ra_dmac_start != ok || poll exhausted``. Two
 * atomic conditions x N+1 = 3 vectors covered by the host
 * integration test.
 *
 * @return ``k_ra_ok`` on success, ``k_ra_err_hw_timeout`` if the
 *         channel never drained, or the start error code.
 *
 * @pre ``s_src`` / ``s_dst`` populated.
 * @post On success ``s_dst`` mirrors ``s_src``.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dma_demo_run_copy(void)
{
  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)(uintptr_t)s_src,
    .dst     = (uint32_t)(uintptr_t)s_dst,
    .count   = (uint16_t)k_dma_demo_buf_words,
    .width   = k_ra_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  const ra_err_t err = ra_dmac_start((uint8_t)k_dma_demo_channel, &cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Software trigger: assert DMREQ.SWREQ on the channel. */
  volatile r_dmac_channel_regs_t* reg = ra_dmac((uint8_t)k_dma_demo_channel);
  if (reg == nullptr) {
    return k_ra_err_hw_error;
  }
  reg->DMREQ = 0x1U;

  /* Poll DMCRA until it drains. */
  for (uint32_t i = 0U; i < (uint32_t)k_dma_demo_poll_limit; ++i) {
    if (reg->DMCRA == 0U) {
      return ra_dmac_stop((uint8_t)k_dma_demo_channel);
    }
  }
  (void)ra_dmac_stop((uint8_t)k_dma_demo_channel);
  return k_ra_err_hw_timeout;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dma_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    dma_demo_fill_buffers();
    const ra_err_t err = dma_demo_run_copy();
    const uint8_t  ok  = (err == k_ra_ok && dma_demo_verify() != 0U) ? 1U : 0U;
    if (ok != 0U) {
      (void)ra_sci_write_polling((uint8_t)k_dma_demo_sci_channel,
                                 k_dma_demo_ok_msg,
                                 (uint32_t)(sizeof(k_dma_demo_ok_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_sci_write_polling((uint8_t)k_dma_demo_sci_channel,
                                 k_dma_demo_bad_msg,
                                 (uint32_t)(sizeof(k_dma_demo_bad_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ra_delay_ms(k_dma_demo_period_ms);
  }
  dma_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
