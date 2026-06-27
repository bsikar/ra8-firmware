/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the e-reader-on-M33 demo
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It demonstrates the #150 power-saving model: an e-reader spends
 * almost all its time idle on a rendered page, so running that idle / hold /
 * page-turn loop on the M33 @ 250 MHz -- while the M85 @ 1 GHz sleeps -- is the
 * high-leverage battery win. "Power saving = drop to the slow core."
 *
 * What the M85 does here:
 *   1. Publishes the progress mailbox (see `ereader_m33.h`) and stamps its
 *      magic so the M33 trusts it.
 *   2. Releases the Cortex-M33 with `ra_cpu1_release` (HUM Ch 2.9.1) into the
 *      reader loop, confirms it booted (signature), then NARRATES: it polls the
 *      mailbox and logs each page the M33 turns. board_sim echoes only the
 *      primary core's ITM, so the M85 speaks for the M33.
 *   3. Once the M33 reaches the last page (`done`), the M85 logs the verdict --
 *      "ereader_m33 PASS" -- and PARKS in low-power WFI: the M33 owns the page.
 *
 * The heavy one-time work (opening / compiling a book, see #149) is what the
 * M85 would spin up for; the steady-state read runs entirely on the M33.
 *
 * @note `ra_log_info` is compiled to a no-op unless the build defines INFO-level
 *       logging (a Debug build). `make sim-ereader_m33` builds Debug so `[itm]`
 *       lines appear; a release build runs the same logic but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ereader_m33.h"
#include "ra_attributes.h"
#include "ra_dual_core.h"
#include "ra_err.h"
#include "ra_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra_ls_cpu1_stack_top;

/**
 * @enum m85_poll_t
 * @brief Bounded iteration limits for the M85 polling loops.
 * @details Large enough that a normally-running M33 always completes within
 *          budget, yet finite so the M85 never hangs if the M33 does not boot.
 *          board_sim runs cpu0 in 500k-instruction chunks and cpu1 in 100k
 *          chunks between them, so the M33's whole walk lands in a few dozen
 *          interleaves -- far inside these budgets.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_sig_poll_budget  = 10000000UL,  /**< Max iters waiting for M33 signature. */
  k_m85_done_poll_budget = 200000000UL, /**< Max iters waiting for M33 done flag. */
} m85_poll_t;

/**
 * @brief Publish the shared mailbox and stamp its magic before release.
 *
 * @param[out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre Called before `ra_cpu1_release` so the M33 sees a live mailbox.
 * @post All progress fields read back as 0 and `status` is `running`.
 * @post `magic` holds ::k_erm33_magic after a `dsb` published the zeros first.
 *
 * @note Single owner (M85) at this point; no concurrency.
 * @since 0.1.0
 */
static void prep_mailbox(volatile erm33_mailbox_t* mb)
{
  mb->magic       = 0U;
  mb->m33_sig     = 0U;
  mb->status      = (uint32_t)k_erm33_status_running;
  mb->chapter_idx = 0U;
  mb->page_idx    = 0U;
  mb->total_pages = 0U;
  mb->heartbeat   = 0U;
  mb->done        = 0U;
  __asm volatile("dsb" ::: "memory");
  mb->magic = (uint32_t)k_erm33_magic;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Poll the mailbox until the M33 stamps its boot signature.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether the M33 boot signature appeared within budget.
 * @retval true  `m33_sig == k_erm33_m33_sig` within ::k_m85_sig_poll_budget.
 * @retval false Poll budget exhausted before the signature appeared.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M85 has already called `ra_cpu1_release`.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_sig_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_m33_sig(volatile erm33_mailbox_t* mb)
{
  RA_BOUNDED_LOOP(k_m85_sig_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_sig_poll_budget; i++) {
    if (mb->m33_sig == (uint32_t)k_erm33_m33_sig) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Poll until the M33 finishes, logging every page it turns.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether `done` reached 1 within budget.
 * @retval true  `done == 1` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before done was set.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M33 has been confirmed alive via ::wait_for_m33_sig.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note Each distinct `page_idx` is narrated once; board_sim interleaves cpu1
 *       between the M85's poll chunks so the page count climbs as it is read.
 * @since 0.1.0
 */
static bool wait_done_narrate(volatile erm33_mailbox_t* mb)
{
  uint32_t last = 0xFFFFFFFFUL;
  RA_BOUNDED_LOOP(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    const uint32_t page = mb->page_idx;
    if (page != last) {
      ra_log_info_val("M85", "M33 turned to page", page);
      last = page;
    }
    if (mb->done == 1U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Log the PASS / FAIL verdict for the M33's read-through.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer and `done == 1`.
 * @pre The M33 has published `status`, `page_idx` and `total_pages`.
 * @post Exactly one verdict line is logged.
 * @post No mailbox field is modified.
 *
 * @note PASS requires the walk to have succeeded, turned at least one page, and
 *       reached the last page (`page_idx == total_pages`).
 * @since 0.1.0
 */
static void report_verdict(volatile erm33_mailbox_t* mb)
{
  bool pass = true;
  if (mb->status != (uint32_t)k_erm33_status_ok) {
    pass = false;
  }
  if (mb->total_pages == 0U) {
    pass = false;
  }
  if (mb->page_idx != mb->total_pages) {
    pass = false;
  }
  if (pass) {
    ra_log_info("M85", "ereader_m33 PASS");
  } else {
    ra_log_info("M85", "ereader_m33 FAIL -- M33 reader did not complete");
  }
}

/**
 * @brief Park the M85 in low-power WFI after the M33 owns the page.
 *
 * @param[in] mb Pointer to the shared mailbox (unused after the verdict).
 *
 * @return This function never returns.
 * @retval (none) The M85 sleeps until power-off (or a future M33 wake IRQ).
 *
 * @pre The M33 has finished the read-through and owns the held page.
 * @pre The verdict has already been logged.
 * @post The M85 spends its time in WFI, not busy-spinning (the power win).
 * @post No mailbox field is modified.
 *
 * @note On silicon WFI sleeps until an interrupt; with no wake source wired yet
 *       the M85 stays asleep -- exactly the low-power posture.
 * @since 0.1.0
 */
[[noreturn]] static void park_low_power(volatile erm33_mailbox_t* mb)
{
  (void)mb;
  ra_log_info("M85", "M85 parked in low-power WFI; M33 holds the page");
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
 *
 * @pre A fatal error (e.g. CPU1 release failure) has occurred and been logged.
 * @post The M85 makes no further forward progress.
 * @post No mailbox state changes.
 *
 * @note Distinct from ::park_low_power: this is an error halt, not low power.
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
 * @details Publishes the mailbox, releases the Cortex-M33 into the reader loop,
 * narrates the pages it turns, logs the verdict, then parks in low-power WFI.
 * See the file header for the power-saving narrative.
 *
 * @return Never returns (ends in ::park_low_power, or ::park_forever on error).
 * @retval (none) Control stays parked.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held inactive by hardware until released here.
 * @post The M33 has read through the book and the M85 is parked.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
int main(void)
{
  ra_log_init();
  ra_log_info("M85", "==== RA8D2 ereader_m33 demo ====");
  ra_log_info("M85", "Cortex-M85 primary core online");
  ra_log_info("M85", "progress mailbox in shared SRAM at 0x22100000");

  volatile erm33_mailbox_t* mb = erm33_mailbox();
  prep_mailbox(mb);
  ra_log_info("M85", "handing the e-reader to the Cortex-M33 ...");

  ra_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  ra_log_info_val("M85", "ra_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra_ok) {
    ra_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_sig(mb)) {
    ra_log_info("M85", "M33 reader is alive");
  } else {
    ra_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra_log_info("M85", "M85 idle; narrating M33 page turns ...");
  if (!wait_done_narrate(mb)) {
    ra_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }

  ra_log_info_val("M85", "M33 finished; chapters read", mb->chapter_idx + 1U);
  ra_log_info_val("M85", "M33 walked total pages", mb->total_pages);
  report_verdict(mb);
  park_low_power(mb);
}
