/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/dma_memcopy_hal/main.c
 * @brief 1 KB DMAC SRAM-to-SRAM copy demo driven by the ra8_dmac HAL
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The HAL-primitive twin of ``dma_memcopy_demo``. It performs the
 * identical 1 KB block copy but fires and waits entirely through the
 * ``ra8_dmac`` API -- ``ra8_dmac_software_trigger`` and
 * ``ra8_dmac_wait_idle`` -- instead of reaching for the raw channel
 * register window (``DMREQ.SWREQ`` / ``DMSTS.ACT``). It therefore does
 * not include ``ra8_dmac_regs.h`` at all: application code no longer
 * touches DMAC MMIO directly.
 *
 * Brings up CGC + SysTick + SCI8 + LEDs + DMAC0 channel 0. Once a
 * second:
 *
 *   1. Fills a 1 KB source buffer with a deterministic pattern
 *      (``i ^ (i >> 8)``) and zeroes the destination buffer.
 *   2. Programmes DMAC0 channel 0 for a 32-bit-wide, increment-both,
 *      single-block transfer of 256 words via ``ra8_dmac_start_block``.
 *   3. Software-triggers the channel via ``ra8_dmac_software_trigger``
 *      and waits for completion via ``ra8_dmac_wait_idle``.
 *   4. Walks both buffers and reports ``"dmahal: copied 1024B match=Y\r\n"``
 *      on the J-Link OB CDC channel.
 *
 * LED1 toggles on every successful copy; LED2 toggles on a
 * verification mismatch or a DMAC timeout.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers. Kept
 * ALONGSIDE the raw-poke ``dma_memcopy_demo`` so the two paths can be
 * diffed on the same bench.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dma_hal_baud       = 115200U, /**< DMA HAL demo baud.         */
  k_dma_hal_period_ms  = 1000U,   /**< DMA HAL demo period ms.    */
  k_dma_hal_buf_bytes  = 1024U,   /**< DMA HAL demo buffer bytes. */
  k_dma_hal_buf_words  = 256U,    /**< DMA HAL demo buffer words. */
  k_dma_hal_poll_limit = 100000U, /**< DMSTS.ACT poll bound.      */
} dma_hal_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dma_hal_channel = 0U, /**< DMA HAL demo channel. */
  k_dma_hal_byte_sh = 8U, /**< DMA HAL demo byte sh. */
} dma_hal_byte_t;

/** @brief Output line tags. */
static const uint8_t k_dma_hal_ok_msg[]  = "dmahal: copied 1024B match=Y\r\n";
static const uint8_t k_dma_hal_bad_msg[] = "dmahal: copied 1024B match=N\r\n";

/** @brief Source / destination buffers (32-bit aligned by element type). */
static uint32_t s_src[k_dma_hal_buf_words];
static uint32_t s_dst[k_dma_hal_buf_words];

/** @brief Park forever after a fatal init failure. */
static void dma_hal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void dma_hal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dma_hal_baud) != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dma_hal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dma_hal_panic_halt();
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
static void dma_hal_fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_hal_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dma_hal_byte_sh);
    s_dst[i] = 0U;
  }
}

/**
 * @brief Verify that ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @par MC/DC:
 * Compound decision in the loop: ``s_dst[i] != s_src[i]``. One atomic
 * condition x 2 vectors -- match (steady-state) and one mismatch
 * (covered by the host integration test).
 *
 * @return 1 if all elements equal, 0 otherwise.
 *
 * @pre Buffers are filled.
 * @post Return value is 0 or 1.
 *
 * @since 0.1.0
 */
static uint8_t dma_hal_verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_hal_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Programme, software-fire, and wait for the DMAC channel.
 *
 * @details
 * Programmes DMAC0 channel 0 in block mode (DMTMD.MD = 10b), then uses
 * the ``ra8_dmac`` HAL primitives to run the whole transfer without
 * touching channel MMIO from application code:
 *
 *  - ``ra8_dmac_software_trigger`` asserts ``DMREQ.SWREQ`` (one block
 *    per trigger).
 *  - ``ra8_dmac_wait_idle`` spins on ``DMSTS.ACT`` until the controller
 *    completes the block or the poll bound expires.
 *
 * The channel is always stopped before returning so a failure never
 * leaves it armed.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_timeout`` if the
 *         channel never went idle, or a propagated start / stop error.
 *
 * @pre ``s_src`` / ``s_dst`` populated.
 * @post On success ``s_dst`` mirrors ``s_src``.
 * @post The channel is stopped (DMCNT.DTE = 0) on every return path.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dma_hal_run_copy(void)
{
  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)(uintptr_t)s_src,
    .dst         = (uint32_t)(uintptr_t)s_dst,
    .count       = (uint16_t)k_dma_hal_buf_words,
    .block_count = 1U,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
  };
  ra8_err_t err = ra8_dmac_start_block((uint8_t)k_dma_hal_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_dmac_software_trigger((uint8_t)k_dma_hal_channel);
  if (err != k_ra8_ok) {
    (void)ra8_dmac_stop((uint8_t)k_dma_hal_channel);
    return err;
  }

  err = ra8_dmac_wait_idle((uint8_t)k_dma_hal_channel, (uint32_t)k_dma_hal_poll_limit);
  const ra8_err_t stop_err = ra8_dmac_stop((uint8_t)k_dma_hal_channel);
  if (err != k_ra8_ok) {
    return err;
  }
  return stop_err;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dma_hal_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    dma_hal_fill_buffers();
    const ra8_err_t err = dma_hal_run_copy();
    uint8_t         ok  = 0U;
    if (err == k_ra8_ok) {
      if (dma_hal_verify() != 0U) {
        ok = 1U;
      }
    }
    if (ok != 0U) {
      (void)ra8_board_uart_console_write(k_dma_hal_ok_msg, (size_t)(sizeof(k_dma_hal_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_dma_hal_bad_msg,
                                         (size_t)(sizeof(k_dma_hal_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ra8_delay_ms(k_dma_hal_period_ms);
  }
  dma_hal_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
