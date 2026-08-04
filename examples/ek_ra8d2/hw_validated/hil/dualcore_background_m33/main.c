/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_background_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the autonomous M33 demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's primary core, the Cortex-M85,
 * out of reset. It demonstrates M33 as an autonomous co-processor:
 *
 *   1. The M85 releases the M33 with `ra8_cpu1_release`.
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
 * @note `ra8_log_info` is a no-op unless the build defines a log level of INFO
 *       or finer (a Debug build). `make emu-dualcore_background_m33` builds
 *       Debug, so `[itm]` lines appear; a release build runs the same logic
 *       but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "dualcore_background.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_cgc.h"
#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra8_ls_cpu1_stack_top;

/**
 * @enum dualcore_bg_hil_t
 * @brief VCOM-console line rate for the deterministic HIL success banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate; the Pi HIL rig's `uart_scrape` reads /dev/ttyACM0 to gate the
 *          app. The banner is additive to the existing `ra8_log` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dualcore_bg_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} dualcore_bg_hil_t;

/**
 * @var k_dualcore_bg_pass_banner
 * @brief Deterministic, run-to-run-stable HIL success banner (uart_scrape gate).
 * @details Emitted over the SCI8 / J-Link OB VCOM console only when the M33's
 *          autonomous final counter equalled ::k_bg_target_count -- a value only
 *          the second core executing can produce. The ITM `ra8_log` verdict is
 *          left intact; this is purely additive so the Pi rig (which has no SWO /
 *          ITM capture) can gate the producer/consumer handshake.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; the HIL gate (hil.conf HIL_EXPECT) matches it exactly.
 * @since 0.1.0
 */
static const uint8_t k_dualcore_bg_pass_banner[] = "dualcore_background_m33: count PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL success banner.
 *
 * @details
 * Configures the clock tree (`ra8_cgc_init`, which publishes the PCLKA the SCI8
 * BRR divisor is computed from -- and which the released M33 then shares) then
 * the EK-RA8D2 debug console (`ra8_board_uart_console_init`, SCI8 on PD02/PD03 at
 * ::k_dualcore_bg_hil_baud). Best-effort: a failure only means the additive HIL
 * banner cannot reach the host; the `ra8_log` ITM trace and the dual-core logic
 * are unaffected.
 *
 * @return Whether the VCOM console is ready to carry the banner.
 * @retval true  Clock + SCI8 console are up.
 * @retval false A bring-up step failed (the banner is then silently skipped).
 *
 * @pre Called once during M85 bring-up, before `ra8_cpu1_release`.
 * @pre `ra8_log_init` has run (failures are narrated over ITM).
 * @post On true, SCI8 is enabled (TE/RE) and PD02/PD03 route to it.
 * @post On false, no console state persists; the app continues normally.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool dualcore_bg_hil_console_init(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_dualcore_bg_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console.
 *
 * @details
 * Writes ::k_dualcore_bg_pass_banner to the SCI8 / J-Link OB VCOM console and
 * flushes it so the bytes clock out before the M85 parks. A no-op if the console
 * never came up (the write returns `k_ra8_err_not_initialized`, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only when the M33 counter hit its target (single, non-compound guard).
 * @pre ::dualcore_bg_hil_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No shared-block field is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static void dualcore_bg_hil_emit_pass(void)
{
  (void)ra8_board_uart_console_write(k_dualcore_bg_pass_banner,
                                     (size_t)(sizeof(k_dualcore_bg_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
}

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
 * @pre Called before `ra8_cpu1_release` so the M33 starts from a clean slate.
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
 * @pre The M85 has already called `ra8_cpu1_release`.
 * @post No block field is modified.
 * @post Iteration count bounded by ::k_m85_sig_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_m33_sig(volatile dualcore_bg_t* bg)
{
  RA8_LOOP_BOUND(k_m85_sig_poll_budget);
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
  RA8_LOOP_BOUND(k_m85_done_poll_budget);
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
 * @note The core spins in place.
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
  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 dualcore_background_m33 demo ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");
  ra8_log_info("M85", "shared block in SRAM at 0x22100000");

  /* Bring up the VCOM console so the success path can emit the HIL banner.
   * Best-effort: a failure is narrated over ITM and the demo continues. */
  if (!dualcore_bg_hil_console_init()) {
    ra8_log_info("M85", "VCOM console init failed -- HIL banner unavailable");
  }

  volatile dualcore_bg_t* bg = dualcore_bg();
  zero_bg(bg);

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_sig(bg)) {
    ra8_log_info("M85", "M33 is alive");
  } else {
    ra8_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra8_log_info("M85", "M85 yielding -- waiting for M33 to finish counting ...");
  if (!wait_for_done(bg)) {
    ra8_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }

  /* Consumer-side data memory barrier: order the load of `counter` AFTER the
   * observation of `done == 1`, pairing with the producer's pre-`done` `dsb`
   * in cpu1_main.c so the M85 reads the final, settled count. Mirrors the
   * publish/consume barrier discipline used by dualcore_mailbox (ping_once)
   * and cpu1_pingpong (main). */
  __asm volatile("dmb" ::: "memory");
  const uint32_t final_count = bg->counter;
  ra8_log_info_val("M85", "M33 counted to", final_count);

  if (final_count == (uint32_t)k_bg_target_count) {
    ra8_log_info("M85", "dualcore_background_m33 PASS");
    /* Additive HIL banner: the M33 counted autonomously to its target, so
     * mirror the verdict to VCOM for the Pi rig's uart_scrape to gate. */
    dualcore_bg_hil_emit_pass();
  } else {
    ra8_log_info("M85", "dualcore_background_m33 FAIL -- wrong count");
  }

  park_forever();
}
