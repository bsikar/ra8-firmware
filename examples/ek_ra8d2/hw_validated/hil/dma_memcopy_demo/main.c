/**
 * @file examples/ek_ra8d2/hw_validated/hil/dma_memcopy_demo/main.c
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
 *      single-block transfer of 256 words via ``ra8_dmac_start``.
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_dmac.h"
#include "ra8_dmac_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dma_demo_baud       = 115200U, /**< DMA demo baud.         */
  k_dma_demo_period_ms  = 1000U,   /**< DMA demo period ms.    */
  k_dma_demo_buf_bytes  = 1024U,   /**< DMA demo buffer bytes. */
  k_dma_demo_buf_words  = 256U,    /**< DMA demo buffer words. */
  k_dma_demo_poll_limit = 100000U, /**< DMA demo poll limit.   */
} dma_demo_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dma_demo_channel = 0U, /**< DMA demo channel. */
  k_dma_demo_byte_sh = 8U, /**< DMA demo byte sh. */
} dma_demo_byte_t;

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

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void dma_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dma_demo_baud) != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dma_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
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
 * @details
 * In **normal** transfer mode each ``DMREQ.SWREQ`` write asks the
 * controller for one unit transfer (per HUM Ch 17.2.15 p 744 -- "When
 * 1 is written to SWREQ bit, a DMA transfer request is generated.
 * After DMA transfer is started [...] this bit is cleared to 0"). To
 * copy the whole 256-word buffer with a single software trigger we
 * run in **block** mode (DMTMD.MD=10b, HUM Ch 17.2.10 p 738) where
 * one trigger moves a full DMCRAH-sized block. DMCRAH carries the
 * block size and DMCRBL counts the number of blocks: one block of
 * ``k_dma_demo_buf_words`` words covers the entire buffer.
 *
 * The poll loop watches DMSTS.ACT (HUM Ch 17.2.16 p 745) -- it
 * de-asserts when the controller finishes the request, which is the
 * correct completion gate for block mode.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_timeout`` if the
 *         channel never drained, or the start error code.
 *
 * @pre ``s_src`` / ``s_dst`` populated.
 * @post On success ``s_dst`` mirrors ``s_src``.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dma_demo_run_copy(void)
{
  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)(uintptr_t)s_src,
    .dst         = (uint32_t)(uintptr_t)s_dst,
    .count       = (uint16_t)k_dma_demo_buf_words,
    .block_count = 1U,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
  };
  const ra8_err_t err = ra8_dmac_start_block((uint8_t)k_dma_demo_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Software trigger: assert DMREQ.SWREQ on the channel.
   * HUM Ch 17.2.15 p 744: write SWREQ=1 to request one block-mode
   * transfer. SWREQ auto-clears when the controller accepts the
   * request (CLRS=0 is the reset default). */
  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_dma_demo_channel);
  if (reg == nullptr) {
    return k_ra8_err_hw_error;
  }
  reg->DMREQ = (uint8_t)k_ra8_dmreq_swreq_mask;

  /* Poll DMSTS.ACT until the controller goes idle.
   * HUM Ch 17.2.16 p 745: "When data transfer in response to one
   * transfer request is completed" the ACT flag clears. */
  for (uint32_t i = 0U; i < (uint32_t)k_dma_demo_poll_limit; ++i) {
    if ((reg->DMSTS & (uint8_t)k_ra8_dmsts_act_mask) == 0U) {
      return ra8_dmac_stop((uint8_t)k_dma_demo_channel);
    }
  }
  (void)ra8_dmac_stop((uint8_t)k_dma_demo_channel);
  return k_ra8_err_hw_timeout;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dma_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    dma_demo_fill_buffers();
    const ra8_err_t err = dma_demo_run_copy();
    const uint8_t   ok  = (err == k_ra8_ok && dma_demo_verify() != 0U) ? 1U : 0U;
    if (ok != 0U) {
      (void)ra8_board_uart_console_write(k_dma_demo_ok_msg,
                                         (size_t)(sizeof(k_dma_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_dma_demo_bad_msg,
                                         (size_t)(sizeof(k_dma_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ra8_delay_ms(k_dma_demo_period_ms);
  }
  dma_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
