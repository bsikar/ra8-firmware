/**
 * @file examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/main.c
 * @brief CPU0 (Cortex-M85) ping-pong demo against CPU1 (Cortex-M33)
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * CPU0 side: releases CPU1 via ra8_cpu1_release, then exchanges
 * ping/pong via a fixed shared-SRAM message struct (see
 * shared_pingpong.h). The IPC peripheral is NOT used: this chip
 * variant has SECEXT disabled on CPU1, IPC channel attribution
 * (SAIPCIRn) is mutually exclusive S xor NS, and with CPU0 booting
 * non-secure in this build neither core can flip a channel's
 * attribution. Shared SRAM at 0x22100000 (the start of the upper
 * on-chip SRAM region, below CPU1's 0x22190000 SRAM bank) sidesteps
 * the IPC security problem entirely while still validating that
 * CPU1 was released, booted, and is executing user code.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "shared_pingpong.h"

extern uint32_t g_ra8_ls_cpu1_mram_start;
extern uint32_t g_ra8_ls_cpu1_stack_top;

/**
 * @var g_cpu1_pingpong_match
 * @brief HIL liveness counter -- incremented on every successful
 *        CPU0 -> CPU1 -> CPU0 ping/pong round-trip (got == 0x4321).
 *
 * @details
 * Read externally by scripts/hil/jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving CPU1 (Cortex-M33) was released, booted its
 * vector table, and is servicing the IPC channel pair. If CPU1 never
 * came out of reset, this counter stays at 0 -- alive-mode could not
 * catch that failure because CPU0 keeps iterating its outer loop.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_match = 0U;

/**
 * @var g_cpu1_pingpong_mismatch
 * @brief HIL failure counter -- incremented whenever a round-trip
 *        cannot complete: TX failed, RX timed out after the bounded
 *        poll, or the returned word was not the expected pong magic.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below
 * HIL_PROBE_MAX_FAILURE). Catches "CPU1 hung after release", "IPC
 * FIFO wedged", and "CPU1 echoed the wrong magic" -- previously
 * invisible because the outer ``while (1)`` happily ``continue``s on
 * every failure.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_mismatch = 0U;

/**
 * @var g_cpu1_pingpong_step
 * @brief Boot progress tracker.
 * @details
 * 0 = pre-main; 1 = main() entry; 2 = after ra8_cpu1_release;
 * 3 = after ra8_ipc_init; 4 = first loop iteration.
 * Lets memprobe pinpoint exactly where CPU0 stalls.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_step = 0U;

/**
 * @var g_cpu1_pingpong_release_err
 * @brief Captured ra8_cpu1_release return code.
 * @details Stamped right after the call returns so memprobe can read
 * whichever ra8_err_t variant the HAL surfaced.
 * @since 0.1.0
 */
/** @brief Sentinel for ::g_cpu1_pingpong_release_err ("none yet"). */
typedef enum : uint32_t {
  k_cpu1_pingpong_err_none = 0xFFFFFFFFU, /**< Cpu1 pingpong error none. */
} cpu1_pingpong_err_sentinel_t;

volatile uint32_t g_cpu1_pingpong_release_err = k_cpu1_pingpong_err_none;

/**
 * @brief Poll until ``pong_seq`` reaches ``target`` or the budget runs out.
 * @details Re-reads the volatile response sequence up to the fixed iteration
 *          bound and returns immediately when CPU1 publishes the target.
 *
 * @param[in] shared Pointer to the shared message struct.
 * @param[in] target Sequence value to wait for.
 *
 * @return Whether the wait observed the target.
 * @retval true  ``pong_seq == target`` was observed inside the budget.
 * @retval false ``k_cpu1_pingpong_poll_budget`` iters elapsed first.
 *
 * @pre shared != nullptr.
 * @pre target equals the most recent value the caller wrote to ping_seq.
 * @post No shared state is mutated.
 * @post Iteration count bounded by ``k_cpu1_pingpong_poll_budget``.
 *
 * @note Not thread-safe. CPU1 must already have been released or this
 *       always times out.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_wait_for_pong(volatile cpu1_pingpong_shared_t* shared,
                                                uint32_t                         target)
{
  for (uint32_t i = 0U; i < (uint32_t)k_cpu1_pingpong_poll_budget; ++i) {
    if (shared->pong_seq == target) {
      return true;
    }
  }
  return false;
}

/**
 * @brief CPU0 application entry.
 * @details See file header for behaviour summary.
 * @return Never returns.
 * @retval 0 Never returned.
 * @pre Boot init has completed.
 * @pre CPU1 is held in reset.
 * @post CPU1 released and the ping-pong loop running.
 * @post Function never returns.
 * @note Single-threaded entry.
 * @since 0.1.0
 */
int main(void)
{
  volatile cpu1_pingpong_shared_t* shared = internal_shared();
  /* CPU0 owns initialization of the shared block. Zero everything
   * before releasing CPU1 so CPU1 can rely on starting from a known
   * clean state. */
  shared->ping_seq     = 0U;
  shared->pong_seq     = 0U;
  shared->ping_payload = 0U;
  shared->pong_payload = 0U;
  __asm volatile("dsb" ::: "memory");

  g_cpu1_pingpong_step = 1U;
  ra8_err_t err        = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  g_cpu1_pingpong_release_err = (uint32_t)err;
  g_cpu1_pingpong_step        = 2U;
  if (err != k_ra8_ok) {
    while (1) {
      __asm volatile("nop");
    }
  }

  g_cpu1_pingpong_step = 3U;
  uint32_t next_seq    = 1U;

  while (1) {
    g_cpu1_pingpong_step = 4U;
    /* Write payload before bumping ping_seq so CPU1 always observes
     * the new payload on the same iteration the sequence advances. */
    shared->ping_payload = (uint32_t)k_cpu1_pingpong_magic_ping;
    __asm volatile("dsb" ::: "memory");
    shared->ping_seq = next_seq;

    if (!internal_wait_for_pong(shared, next_seq)) {
      g_cpu1_pingpong_mismatch += 1U;
      next_seq += 1U;
      continue;
    }
    if (shared->pong_payload != (uint32_t)k_cpu1_pingpong_magic_pong) {
      g_cpu1_pingpong_mismatch += 1U;
      next_seq += 1U;
      continue;
    }
    g_cpu1_pingpong_match += 1U;
    next_seq += 1U;
  }
}
