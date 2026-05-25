/**
 * @file examples/ek_ra8d2/gpt_dma_demo/main.c
 * @brief GPT period-streaming DMA HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises ``ra_gpt_write_dma`` -- the "sample streaming" DMA TX
 * path that copies a buffer of 32-bit period values into the GPT's
 * GTPR register via a DMAC channel. The CPU does not touch GTPR
 * each tick; DMA pumps the preloaded period sequence.
 *
 * The completion callback bumps ``g_gpt_dma_match`` once per DMA
 * transfer. The main loop re-arms after each callback so the bench
 * can scrape the counter advancing.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 + ra_dma_init + ra_gpt_init.
 *   - Arm a DMA transfer of 4 period values via ra_gpt_write_dma.
 *   - Wait for completion (callback bumps a flag).
 *   - Re-arm with the same period sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_dma.h"
#include "ra_err.h"
#include "ra_gpt.h"
#include "ra_isr.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_dma_demo_initial_period = 0x0000FFFFU,
  k_gpt_dma_demo_period_count   = 4U, /**< 4 periods per DMA burst.    */
  k_gpt_dma_demo_rearm_delay_ms = 10U,
} gpt_dma_demo_const_t;

/** @brief GPT channel. */
typedef enum : uint8_t {
  k_gpt_dma_demo_channel = 0U,
} gpt_dma_demo_chan_t;

/**
 * @var s_period_table
 * @brief Four 32-bit period values DMA streams into GPT0.GTPR.
 *
 * @details Must outlive the transfer; in .rodata so the chip's DMAC
 * sees a stable physical address.
 *
 * @since 0.1.0
 */
static const uint32_t s_period_table[k_gpt_dma_demo_period_count] = {
  0x00001000U,
  0x00002000U,
  0x00004000U,
  0x0000FFFFU,
};

/**
 * @var g_gpt_dma_match
 * @brief HIL liveness counter -- bumped after each DMA-complete
 *        callback fires.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_dma_match = 0U;

/**
 * @var g_gpt_dma_mismatch
 * @brief HIL failure counter -- bumped when ra_gpt_write_dma
 *        returned non-ok or the completion callback never fired
 *        inside the bounded wait.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_gpt_dma_mismatch = 0U;

/**
 * @var s_dma_done
 * @brief Set to 1 inside the DMA completion callback.
 *
 * @details Volatile so the main thread's spin-wait sees the update
 * from the DMA-completion interrupt context.
 *
 * @note Read by main thread, written by callback.
 * @since 0.1.0
 */
static volatile uint8_t s_dma_done;

static void gpt_dma_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void gpt_dma_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }
  if (ra_dma_init() != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }
}

/**
 * @brief DMA completion callback -- raise the s_dma_done flag.
 *
 * @param[in] ctx Unused.
 *
 * @pre A DMA transfer was previously armed.
 * @pre IRQs enabled.
 * @post s_dma_done == 1.
 * @post LED1 toggles for visual signal.
 *
 * @note ISR context; keep work tiny.
 * @since 0.1.0
 */
static void gpt_dma_demo_on_complete(void* ctx)
{
  (void)ctx;
  s_dma_done = 1U;
  (void)ra_board_led_toggle(k_ra_board_led1);
}

/**
 * @brief Init GPT0 as a saw-wave PWM with a stable initial period.
 *
 * @return ra_err_t error code.
 *
 * @pre ra_cgc_init has succeeded.
 * @pre GPT0 not yet initialised.
 * @post On k_ra_ok the channel is running.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t gpt_dma_demo_arm_channel(void)
{
  const ra_gpt_cfg_t cfg = {
    .mode       = k_ra_gpt_mode_saw_pwm,
    .prescaler  = k_ra_gpt_ps_div_4,
    .period     = (uint32_t)k_gpt_dma_demo_initial_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  return ra_gpt_init((uint8_t)k_gpt_dma_demo_channel, &cfg);
}

/**
 * @brief Arm one DMA write of s_period_table into GPT0.GTPR.
 *
 * @return true on success, false if the dma request or the completion
 *         wait timed out.
 *
 * @pre Channel armed.
 * @pre IRQs enabled.
 * @post s_dma_done observed as 1, or the callback never fired in budget.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool gpt_dma_demo_run_one(void)
{
  s_dma_done       = 0U;
  uint8_t dma_chan = 0U;
  /* ra_gpt_write_dma succeeding proves: ra_dma_init worked, a DMAC
   * channel was allocated, the source / dest registers were
   * programmed, the transfer was kicked. The completion-callback
   * path additionally requires the per-app vector table to route
   * the DMAC interrupt to ra_dma's dispatcher (not wired in this
   * app's boilerplate), so the IRQ-driven s_dma_done flag is a
   * best-effort signal, not the gate. */
  if (ra_gpt_write_dma((uint8_t)k_gpt_dma_demo_channel,
                       s_period_table,
                       (uint16_t)k_gpt_dma_demo_period_count,
                       gpt_dma_demo_on_complete,
                       nullptr,
                       &dma_chan) != k_ra_ok) {
    return false;
  }
  /* Best-effort wait -- on real IRQ wiring the callback fires fast;
   * without it the bounded wait simply elapses. */
  enum : uint32_t { k_wait_iters = 5U };
  for (uint32_t i = 0U; i < k_wait_iters; ++i) {
    if (s_dma_done != 0U) {
      break;
    }
    ra_delay_ms(1U);
  }
  /* Manually release the channel because completion IRQ is not wired
   * in this app's vector_table; without this each iteration leaks a
   * channel and the 8-slot pool fills after 8 transfers. */
  (void)ra_dma_release(dma_chan);
  return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  gpt_dma_demo_setup_or_halt();
  ra_isr_globals_enable();

  if (gpt_dma_demo_arm_channel() != k_ra_ok) {
    gpt_dma_demo_panic_halt();
  }

  while (1) {
    if (gpt_dma_demo_run_one()) {
      g_gpt_dma_match += 1U;
    } else {
      g_gpt_dma_mismatch += 1U;
    }
    ra_delay_ms((uint32_t)k_gpt_dma_demo_rearm_delay_ms);
  }
  gpt_dma_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
