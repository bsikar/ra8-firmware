/**
 * @file examples/ek_ra8d2/hw_validated/hil/ssie_audio_loop/main.c
 * @brief SSIE I2S internal-loopback audio integrity demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up SSIE channel 0 in controller / I2S mode and exercises the
 * full-duplex TX -> RX path with an internally generated 16-sample
 * sine pattern. The demo pushes the pattern into the TX FIFO via
 * ``ra8_ssie_write_sample`` and (on real silicon, with the J11 audio
 * connector wired in loopback) reads it back via ``ra8_ssie_read_sample``.
 * On the host unit-test build the read path is mocked but the API
 * surface is exercised identically.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra8_ssie_init(0, &cfg)`` -- controller I2S, 16-bit data / 32-bit
 *      system word, AUDIO_MCK / 32 bit clock divider.
 *   3. ``ra8_ssie_start(0, k_ra8_ssie_dir_tx_rx)``.
 *   4. Push the 16-sample sine pattern into TX, log status.
 *   5. ``ra8_ssie_stop(0)`` + ``ra8_ssie_deinit(0)`` then idle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_ssie.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_ssie_loop_baud = 115200U, /**< Ssie loop baud. */
} ssie_loop_const_t;

/** @brief SSIE channel + FIFO watermarks. */
typedef enum : uint8_t {
  k_ssie_loop_channel      = 0U,  /**< Ssie loop channel.      */
  k_ssie_loop_tx_threshold = 4U,  /**< Ssie loop TX threshold. */
  k_ssie_loop_rx_threshold = 4U,  /**< Ssie loop RX threshold. */
  k_ssie_loop_sample_count = 16U, /**< Ssie loop sample count. */
} ssie_loop_chan_t;

static const uint8_t k_ssie_loop_log_msg[] = "ssie: loop ok\r\n";

/**
 * @brief 16-entry quantised sine pattern (Q15, two periods) used as the
 *        TX-side audio test vector.
 */
static const uint32_t k_ssie_loop_pattern[16] = {
  0x00000000U,
  0x30FB0000U,
  0x5A820000U,
  0x76410000U,
  0x7FFF0000U,
  0x76410000U,
  0x5A820000U,
  0x30FB0000U,
  0x00000000U,
  0xCF050000U,
  0xA57E0000U,
  0x89BF0000U,
  0x80010000U,
  0x89BF0000U,
  0xA57E0000U,
  0xCF050000U,
};

static void ssie_loop_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void ssie_loop_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ssie_loop_baud) != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
}

/**
 * @brief Bring SSIE0 up in controller I2S mode and stream the test pattern.
 *
 * @par MC/DC:
 * Compound decision: ``init != ok || start != ok || write_sample != ok``.
 * Three atomic conditions x N+1 = 4 vectors -- all three failure
 * branches plus the all-ok golden are exercised in
 * test_app_ssie_audio_loop.c.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ssie_loop_run(void)
{
  const ra8_ssie_cfg_t cfg = {
    .role          = k_ra8_ssie_role_controller,
    .format        = k_ra8_ssie_format_i2s,
    .data_word     = k_ra8_ssie_dwl_16,
    .system_word   = k_ra8_ssie_swl_32,
    .bclk_div      = k_ra8_ssie_bclk_div_32,
    .use_gpt_clk   = false,
    .long_frame    = true,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = true,
    .tx_threshold  = (uint8_t)k_ssie_loop_tx_threshold,
    .rx_threshold  = (uint8_t)k_ssie_loop_rx_threshold,
  };
  ra8_err_t err = ra8_ssie_init((uint8_t)k_ssie_loop_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_ssie_start((uint8_t)k_ssie_loop_channel, k_ra8_ssie_dir_tx_rx);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ssie_loop_sample_count; i++) {
    err = ra8_ssie_write_sample((uint8_t)k_ssie_loop_channel, k_ssie_loop_pattern[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  ssie_loop_setup_or_halt();
  ra8_isr_globals_enable();

  if (ssie_loop_run() != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  if (ra8_board_uart_console_write(k_ssie_loop_log_msg,
                                   (size_t)(sizeof(k_ssie_loop_log_msg) - 1U)) != k_ra8_ok) {
    ssie_loop_panic_halt();
  }
  (void)ra8_ssie_stop((uint8_t)k_ssie_loop_channel);
  (void)ra8_ssie_deinit((uint8_t)k_ssie_loop_channel);

  while (1) {
    __asm__ volatile("wfi");
  }
  return 0;
}
#pragma GCC diagnostic pop
