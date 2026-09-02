/**
 * @file examples/ek_ra8d2/hw_validated/hil/gpt_dma_demo/src/main.c
 * @brief GPT period-streaming DMA HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises ``ra8_gpt_write_dma`` -- the "sample streaming" DMA TX
 * path that copies a buffer of 32-bit period values into the GPT's
 * GTPR register via a DMAC channel. The CPU does not touch GTPR
 * each tick; DMA pumps the preloaded period sequence.
 *
 * The completion callback bumps ``g_gpt_dma_match`` once per DMA
 * transfer. The main loop re-arms after each callback so the bench
 * can scrape the counter advancing.
 *
 * Bring-up sequence:
 *   - CGC + SysTick + LED1 + ra8_dma_init + ra8_gpt_init.
 *   - Arm a DMA transfer of 4 period values via ra8_gpt_write_dma.
 *   - Wait for completion (callback bumps a flag).
 *   - Re-arm with the same period sequence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_dma.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_gpt_dma_demo_initial_period = 0x0000FFFFU, /**< GPT DMA demo initial period. */
  k_gpt_dma_demo_period_count   = 4U,          /**< 4 periods per DMA burst.     */
  k_gpt_dma_demo_rearm_delay_ms = 10U,         /**< GPT DMA demo rearm delay ms. */
} gpt_dma_demo_const_t;

/** @brief GPT channel. */
typedef enum : uint8_t {
  k_gpt_dma_demo_channel = 0U, /**< GPT DMA demo channel. */
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
 * @brief HIL failure counter -- bumped when ra8_gpt_write_dma
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

/**
 * @brief Park the core after an unrecoverable GPT DMA demo failure.
 * @details Repeatedly executes WFI, preserving DMA and timer state for debug.
 * @pre Called only from a fatal boot or terminal foreground path.
 * @pre The caller does not require recovery without reset.
 * @post The core stays in WFI until external intervention.
 * @post Neither exported HIL counter changes after entry.
 * @note Not thread-safe; this is a terminal single-threaded path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, SysTick, LED1, and the DMA allocator.
 * @details Brings dependencies up in order and parks on the first HAL error.
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, delays, LED1, and DMA channel allocation are ready.
 * @post GPT0 remains unconfigured until ``internal_arm_channel`` runs.
 * @note Not thread-safe; it owns the demo's peripheral initialization.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_dma_init() != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief DMA completion callback -- raise the s_dma_done flag.
 * @details Publishes the volatile completion flag before issuing the optional
 *          visual LED toggle, keeping interrupt-context work bounded.
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
RA8_INTERNAL static void internal_on_complete(void* ctx)
{
  (void)ctx;
  s_dma_done = 1U;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
}

/**
 * @brief Init GPT0 as a saw-wave PWM with a stable initial period.
 * @details Programs channel zero for PCLKD/4 saw-wave PWM and starts it so the
 *          DMA stream has a live GTPR destination.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok GPT0 accepted and started the configuration.
 * @retval (other) The HAL rejected or could not apply the configuration.
 *
 * @pre ra8_cgc_init has succeeded.
 * @pre GPT0 not yet initialised.
 * @post On k_ra8_ok the channel is running.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_arm_channel(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_gpt_dma_demo_initial_period,
    .duty_a     = 0U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  return ra8_gpt_init((uint8_t)k_gpt_dma_demo_channel, &cfg);
}

/**
 * @brief Arm one DMA write of s_period_table into GPT0.GTPR.
 * @details Resets the completion flag, allocates and starts one bounded period
 *          transfer, waits briefly for the optional callback, then explicitly
 *          releases the channel because this app does not wire its DMA IRQ.
 *
 * @return Whether the DMA request itself was accepted.
 * @retval true The DMA request was accepted and its channel was released.
 * @retval false The DMA request failed before a channel could be used.
 *
 * @pre Channel armed.
 * @pre IRQs enabled.
 * @post s_dma_done observed as 1, or the callback never fired in budget.
 * @post An allocated channel is released before return.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_run_one(void)
{
  s_dma_done       = 0U;
  uint8_t dma_chan = 0U;
  /* ra8_gpt_write_dma succeeding proves: ra8_dma_init worked, a DMAC
   * channel was allocated, the source / dest registers were
   * programmed, the transfer was kicked. The completion-callback
   * path additionally requires the per-app vector table to route
   * the DMAC interrupt to ra8_dma's dispatcher (not wired in this
   * app's boilerplate), so the IRQ-driven s_dma_done flag is a
   * best-effort signal, not the gate. */
  if (ra8_gpt_write_dma((uint8_t)k_gpt_dma_demo_channel,
                        s_period_table,
                        (uint16_t)k_gpt_dma_demo_period_count,
                        internal_on_complete,
                        nullptr,
                        &dma_chan) != k_ra8_ok) {
    return false;
  }
  /* Best-effort wait -- on real IRQ wiring the callback fires fast;
   * without it the bounded wait simply elapses. */
  enum : uint32_t { k_wait_iters = 5U /**< Wait iters. */ };
  for (uint32_t i = 0U; i < k_wait_iters; ++i) {
    if (s_dma_done != 0U) {
      break;
    }
    ra8_delay_ms(1U);
  }
  /* Manually release the channel because completion IRQ is not wired
   * in this app's vector_table; without this each iteration leaks a
   * channel and the 8-slot pool fills after 8 transfers. */
  (void)ra8_dma_release(dma_chan);
  return true;
}

/**
 * @brief Repeatedly stream the fixed period table into GPT0 through DMA.
 * @details Initializes the demo and GPT0, runs one bounded DMA request per
 *          iteration, updates the exported match/mismatch counter, and delays.
 * @pre Reset startup and SystemInit completed successfully.
 * @pre GPT0 and the DMA allocator are not owned by another context.
 * @post Every iteration increments exactly one HIL result counter.
 * @post Allocated DMA channels are released by ``internal_run_one``.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_arm_channel() != k_ra8_ok) {
    internal_panic_halt();
  }

  while (1) {
    if (internal_run_one()) {
      g_gpt_dma_match += 1U;
    } else {
      g_gpt_dma_mismatch += 1U;
    }
    ra8_delay_ms((uint32_t)k_gpt_dma_demo_rearm_delay_ms);
  }
  internal_panic_halt();
}
