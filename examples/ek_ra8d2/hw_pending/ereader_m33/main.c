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
 *      reader loop, confirms it booted (signature), then NARRATES + CONSUMES:
 *      for every new `frame_seq` the M33 publishes it reads back the shared
 *      framebuffer, logs the page number and a CRC-32 of the rendered pixels
 *      (proving the M33 produced real, page-varying pixels -- not a blank
 *      plane), and writes `frame_ack` so the M33 may render the next page.
 *      board_sim echoes only the primary core's ITM, so the M85 speaks for the
 *      M33.
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
 * @enum m85_crc_t
 * @brief Constants for the standard reflected CRC-32 over a rendered page.
 * @details The M85 folds every byte of the M33's framebuffer through the IEEE
 *          802.3 / zlib CRC-32 (reflected polynomial, pre/post-inverted) and
 *          logs the result, so a non-zero and page-varying value is proof the
 *          M33 wrote real, page-specific pixels into shared memory.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_crc32_init = 0xFFFFFFFFUL, /**< Pre/post-inversion seed.     */
  k_crc32_poly = 0xEDB88320UL, /**< Reflected CRC-32 polynomial. */
} m85_crc_t;

/**
 * @enum m85_crc_bits_t
 * @brief Bit-fold count for the bitwise CRC-32 inner loop.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_crc32_bits = 8U, /**< Bits folded per input byte. */
} m85_crc_bits_t;

/**
 * @brief CRC-32 of the shared framebuffer the M33 just rendered.
 *
 * @details Standard table-less reflected CRC-32: seed all-ones, fold each byte
 * LSB-first through ::k_crc32_poly, post-invert. Reading every byte back across
 * the shared-SRAM boundary is what proves the M33's pixels actually landed; a
 * blank plane hashes to one fixed value, so a different (and per-page-varying)
 * CRC demonstrates genuine rendering.
 *
 * @param[in] fb  Framebuffer base (never NULL).
 * @param[in] len Bytes to hash; expected to equal ::k_erm33_fb_bytes.
 *
 * @return CRC-32 of the @p len bytes at @p fb.
 * @retval 0 @p fb is NULL or @p len is 0 (no pixels to hash).
 *
 * @pre @p fb addresses the shared framebuffer (::erm33_framebuffer).
 * @pre The M33 published this page and is holding it (`frame_seq`) so the
 *      bytes are stable for the duration of the read.
 * @post No framebuffer byte is modified.
 * @post Both loops are bounded (::k_erm33_fb_bytes, ::k_crc32_bits; NASA Rule 2).
 *
 * @note Not thread-safe; relies on the frame_seq / frame_ack hold handshake.
 * @since 0.1.0
 */
static uint32_t fb_crc32(volatile const uint8_t* fb, uint32_t len)
{
  if (fb == nullptr) {
    return 0U;
  }
  if (len == 0U) {
    return 0U;
  }
  uint32_t crc = (uint32_t)k_crc32_init;
  RA_BOUNDED_LOOP(k_erm33_fb_bytes);
  for (uint32_t i = 0U; i < len; i++) {
    if (i >= (uint32_t)k_erm33_fb_bytes) {
      break;
    }
    crc ^= (uint32_t)fb[i];
    RA_BOUNDED_LOOP(k_crc32_bits);
    for (uint8_t b = 0U; b < (uint8_t)k_crc32_bits; b++) {
      const uint32_t mask = (uint32_t)(0U - (crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_crc32_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_crc32_init;
}

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
  mb->frame_seq   = 0U;
  mb->frame_ack   = 0U;
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
 * @brief Poll until the M33 finishes, CRC-logging every page it renders.
 *
 * @details Watches `frame_seq`. On each new value the M33 has rendered a fresh
 * page and is holding it: the M85 reads the shared framebuffer back, logs the
 * page number and a CRC-32 of its pixels, then writes `frame_ack` so the M33 may
 * render the next page. The render hold-handshake makes the read race-free.
 *
 * @param[in,out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether `done` reached 1 within budget.
 * @retval true  `done == 1` within ::k_m85_done_poll_budget iterations.
 * @retval false Poll budget exhausted before done was set.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The M33 has been confirmed alive via ::wait_for_m33_sig.
 * @post Only `frame_ack` is written; every other mailbox field is read-only.
 * @post Iteration count bounded by ::k_m85_done_poll_budget (NASA Rule 2).
 *
 * @note Each distinct `frame_seq` is consumed once; the producer/consumer hold
 *       handshake guarantees the M85 CRCs every page in order before `done`.
 * @since 0.1.0
 */
static bool wait_done_narrate(volatile erm33_mailbox_t* mb)
{
  uint32_t last = 0U;
  RA_BOUNDED_LOOP(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    const uint32_t seq = mb->frame_seq;
    if (seq != last) {
      const uint32_t crc = fb_crc32(erm33_framebuffer(), (uint32_t)k_erm33_fb_bytes);
      ra_log_info_val("M85", "M33 turned to page", seq);
      ra_log_info_val("M85", "M33 page pixels CRC32", crc);
      (void)crc; /* the read above is the proof; the value is logged only in Debug */
      last          = seq;
      mb->frame_ack = seq;
      __asm volatile("dsb" ::: "memory");
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
