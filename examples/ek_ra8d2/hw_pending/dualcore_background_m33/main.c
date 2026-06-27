/**
 * @file examples/ek_ra8d2/hw_pending/dualcore_background_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the autonomous M33 demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's primary core, the Cortex-M85,
 * out of reset. It demonstrates M33 as an autonomous co-processor:
 *
 *   1. The M85 releases the M33 with `ra_cpu1_release`.
 *   2. The M85 waits for the M33 boot signature -- proof that the second core
 *      left reset and is executing code.
 *   3. The M85 then simply polls `done` in shared SRAM while the M33 runs
 *      independently. The M85 does NO other work (no heartbeat, no pings)
 *      -- the teaching point is that M85 can yield while M33 runs.
 *   4. Once the M33 sets `done = 1`, the M85 reads the final counter value
 *      and logs whether it matches the expected ::k_bg_target_count.
 *
 * This pattern teaches producer/consumer with shared SRAM: the M33 is the
 * producer (it counts) and the M85 is the consumer (it reads the result).
 * Contrast with `dualcore_mailbox`, where the M85 drives each request/reply
 * round. Here the M85 yields and lets the M33 run at its own pace.
 *
 * @note `ra_log_info` is a no-op unless the build defines a log level of INFO
 *       or finer (a Debug build). `make sim-dualcore_background_m33` builds
 *       Debug, so `[itm]` lines appear; a release build runs the same logic
 *       but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "dualcore_background.h"
#include "ra_attributes.h"
#include "ra_dual_core.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra_ls_cpu1_stack_top;

/**
 * @enum m85_bg_poll_t
 * @brief Bounded iteration limits for the M85 polling loops.
 * @details Large enough that a normally-running M33 always completes within
 *          budget, yet finite so the M85 never hangs if the M33 does not boot.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_sig_poll_budget  = 10000000UL, /**< Max iters waiting for M33 signature. */
  k_m85_done_poll_budget = 50000000UL, /**< Max iters waiting for M33 done flag. */
} m85_bg_poll_t;

/**
 * @brief Zero the shared control block before the M33 is released.
 *
 * @param[out] bg Pointer to the shared control block (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p bg points to the fixed shared-SRAM address.
 * @pre Called before `ra_cpu1_release` so the M33 starts from a clean slate.
 * @post All three words of the block read back as 0.
 * @post A `dsb` has drained the writes to shared SRAM.
 *
 * @note Single owner (M85) at this point; no concurrency.
 * @since 0.1.0
 */
static void zero_bg(volatile dualcore_bg_t* bg)
{
  bg->counter = 0U;
  bg->done    = 0U;
  bg->m33_sig = 0U;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Poll the shared block until the M33 stamps its boot signature.
 *
 * @param[in] bg Pointer to the shared control block (never NULL).
 *
 * @return Whether the M33 boot signature appeared within budget.
 * @retval true  `m33_sig == k_bg_m33_signature` within ::k_m85_sig_poll_budget.
 * @retval false Poll budget exhausted before the signature appeared.
 *
 * @pre @p bg points to the fixed shared-SRAM address.
 * @pre The M85 has already called `ra_cpu1_release`.
 * @post No block field is modified.
 * @post Iteration count bounded by ::k_m85_sig_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_m33_sig(volatile dualcore_bg_t* bg)
{
  RA_BOUNDED_LOOP(k_m85_sig_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_sig_poll_budget; i++) {
    if (bg->m33_sig == (uint32_t)k_bg_m33_signature) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Poll the shared block until the M33 sets the done flag.
 *
 * @param[in] bg Pointer to the shared control block (never NULL).
 *
 * @return Whether `done` reached 1 within budget.
 * @retval true  `done == 1` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before done was set.
 *
 * @pre @p bg points to the fixed shared-SRAM address.
 * @pre The M33 has been confirmed alive via ::wait_for_m33_sig.
 * @post No block field is modified.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note M85 does nothing else during this poll -- this is the yield point.
 * @since 0.1.0
 */
static bool wait_for_done(volatile dualcore_bg_t* bg)
{
  RA_BOUNDED_LOOP(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    if (bg->done == 1U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
 *
 * @pre A fatal error has occurred and has already been logged.
 * @post The M85 makes no further forward progress.
 * @post No shared block state changes.
 *
 * @note Mirrors the M33 fault handler for symmetry.
 * @since 0.1.0
 */
[[noreturn]] static void park_forever(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief CPU0 (Cortex-M85) application entry.
 *
 * @details Brings up logging, zeros the shared control block, releases the
 * Cortex-M33, waits for its boot signature, then yields while the M33 counts
 * autonomously to ::k_bg_target_count. After the M33 sets `done`, the M85
 * reads the counter and logs the PASS/FAIL verdict.
 *
 * @return Never returns (ends in ::park_forever after logging the result).
 * @retval (none) Control stays in the final spin.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held in reset by hardware until released here.
 * @post The M33 has autonomously incremented the counter and set done.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
int main(void)
{
  ra_log_init();
  ra_log_info("M85", "==== RA8D2 dualcore_background_m33 demo ====");
  ra_log_info("M85", "Cortex-M85 primary core online");
  ra_log_info("M85", "shared block in SRAM at 0x22100000");

  volatile dualcore_bg_t* bg = dualcore_bg();
  zero_bg(bg);

  ra_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  ra_log_info_val("M85", "ra_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra_ok) {
    ra_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_sig(bg)) {
    ra_log_info("M85", "M33 is alive");
  } else {
    ra_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra_log_info("M85", "M85 yielding -- waiting for M33 to finish counting ...");
  if (!wait_for_done(bg)) {
    ra_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }

  /* Consumer-side data memory barrier: order the load of `counter` AFTER the
   * observation of `done == 1`, pairing with the producer's pre-`done` `dsb`
   * in cpu1_main.c so the M85 reads the final, settled count. Mirrors the
   * publish/consume barrier discipline used by dualcore_mailbox (ping_once)
   * and cpu1_pingpong (main). */
  __asm volatile("dmb" ::: "memory");
  const uint32_t final_count = bg->counter;
  ra_log_info_val("M85", "M33 counted to", final_count);

  if (final_count == (uint32_t)k_bg_target_count) {
    ra_log_info("M85", "dualcore_background_m33 PASS");
  } else {
    ra_log_info("M85", "dualcore_background_m33 FAIL -- wrong count");
  }

  park_forever();
}
