/**
 * @file examples/ek_ra8d2/hw_pending/compile_on_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver: stage an EPUB, offload its compile to the M33
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It demonstrates the #149 compiler offload: the M85 stages a book
 * and hands the full EPUB->`.rabook` conversion to the M33 @ 250 MHz, so on a real
 * import the M85 @ 1 GHz stays free to keep the UI live while the slow core grinds.
 *
 * What the M85 does here:
 *   1. Publishes the shared mailbox (see `compile_on_m33.h`), stages the source
 *      `.epub` into shared SRAM, and posts a compile JOB (epub base/len + output
 *      buffer) -- so the same M33 image compiles whatever the M85 dispatches.
 *   2. Releases the Cortex-M33 with `ra_cpu1_release` (HUM Ch 2.9.1), confirms it
 *      booted (signature), then PARKS while the M33 runs the full text/CSS/SVG
 *      compile (`ra_epub_open` -> the pipeline) into the shared output blob.
 *   3. Once the M33 signals `done`, the M85 runs `ra_book_validate` over the blob,
 *      cross-checks the reported CRC + chapter count, AND byte-compares it to the
 *      desktop/M85 golden -- proving the M33 compile is byte-identical. A mismatch
 *      traps (`bkpt`) so board_sim's smoke gate fails the run.
 *
 * board_sim echoes only the primary core's ITM, so the M85 narrates the whole
 * exchange; the blob it validates can only exist because the M33 ran the compile,
 * so a PASS here proves the conversion ran byte-identically on the second core.
 *
 * @note `ra_log_info` is compiled to a no-op unless the build defines INFO-level
 *       logging (a Debug build). `make sim-compile_on_m33` builds Debug so the
 *       `[itm]` lines appear; a release build runs the same logic but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compile_on_m33.h"
#include "parity_fixture.h"
#include "ra_attributes.h"
#include "ra_book.h"
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
 * @details Large enough that a normally-running M33 always finishes the small
 *          emit within budget, yet finite so the M85 never hangs if the M33 does
 *          not boot. board_sim interleaves the cores in instruction chunks, so
 *          the whole emit lands in a few dozen interleaves -- far inside these.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_sig_poll_budget  = 10000000UL, /**< Max iters waiting for M33 signature. */
  k_m85_done_poll_budget = 50000000UL, /**< Max iters waiting for M33 done flag. */
} m85_poll_t;

static_assert((uint32_t)k_m33_parity_epub_len <= (uint32_t)k_com33_epub_cap,
              "staged parity .epub must fit the shared input buffer");

/**
 * @brief Publish the mailbox, stage the source .epub, and post the compile job.
 *
 * @details Zeros the response fields and stamps ::k_com33_magic, then copies the
 * baked parity fixture `.epub` into the shared staged-input buffer, fills the
 * request (epub base/len, output base/cap), and stamps ::k_com33_req_magic LAST
 * behind a `dsb` so the M33 sees the job only once the staged bytes settle. The
 * M33 compiles whatever is staged, so the same image serves any dispatched book.
 *
 * @param[out] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre Called before `ra_cpu1_release` so the M33 sees a live, posted job.
 * @post All response fields read back as 0 and `status` is `running`.
 * @post The staged `.epub` is in shared SRAM and `req_magic` holds
 *       ::k_com33_req_magic behind a `dsb`.
 *
 * @note Single owner (M85) at this point; no concurrency.
 * @since 0.1.0
 */
static void prep_mailbox(volatile com33_mailbox_t* mb)
{
  mb->magic         = 0U;
  mb->req_magic     = 0U;
  mb->m33_sig       = 0U;
  mb->status        = (uint32_t)k_com33_status_running;
  mb->blob_base     = 0U;
  mb->blob_len      = 0U;
  mb->blob_crc      = 0U;
  mb->chapter_count = 0U;
  mb->done          = 0U;
  __asm volatile("dsb" ::: "memory");
  mb->magic = (uint32_t)k_com33_magic;

  /* Stage the source .epub into shared SRAM and POST the job: the M33 compiles
   * the staged bytes (not a baked-in array), so the same M33 image serves any
   * book the M85 dispatches -- this is the seam stage c reuses to offload a real
   * dropped book. req_magic is stamped LAST, behind a dsb, so the M33 sees the
   * job only once the staged bytes + request fields are settled in memory. */
  memcpy(com33_epub(), s_m33_parity_epub, (size_t)k_m33_parity_epub_len);
  mb->epub_base = (uint32_t)(uintptr_t)k_com33_epub_addr;
  mb->epub_len  = (uint32_t)k_m33_parity_epub_len;
  mb->out_base  = (uint32_t)(uintptr_t)k_com33_blob_addr;
  mb->out_cap   = (uint32_t)k_com33_blob_cap;
  __asm volatile("dsb" ::: "memory");
  mb->req_magic = (uint32_t)k_com33_req_magic;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Poll the mailbox until the M33 stamps its boot signature.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether the M33 boot signature appeared within budget.
 * @retval true  `m33_sig == k_com33_m33_sig` within ::k_m85_sig_poll_budget.
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
static bool wait_for_m33_sig(volatile com33_mailbox_t* mb)
{
  RA_BOUNDED_LOOP(k_m85_sig_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_sig_poll_budget; i++) {
    if (mb->m33_sig == (uint32_t)k_com33_m33_sig) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Poll the mailbox until the M33 sets the done flag.
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
 * @note M85 does nothing else during this poll -- this is the yield point.
 * @since 0.1.0
 */
static bool wait_for_done(volatile com33_mailbox_t* mb)
{
  RA_BOUNDED_LOOP(k_m85_done_poll_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m85_done_poll_budget; i++) {
    if (mb->done == 1U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Validate the M33-emitted blob and cross-check the mailbox report.
 *
 * @details Confirms the M33 reported success and a plausible blob location, runs
 * @ref ra_book_validate over the shared blob (magic, version, table extents, and
 * the body CRC-32 -- an independent recompute that catches any transfer
 * corruption), then cross-checks that the blob header's CRC and chapter count
 * match what the M33 published in the mailbox. Each check is an independent guard
 * (no compound decisions) so the failing condition is unambiguous.
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL, `done == 1`).
 *
 * @return Whether the M33 produced a well-formed, intact RABOOK1 blob.
 * @retval true  Status ok, blob validated, and CRC + chapter count agree.
 * @retval false Any check failed; the blob must not be treated as valid.
 *
 * @pre @p mb is the fixed-address mailbox pointer and the M33 set `done`.
 * @pre The blob bytes are settled (the caller issued a `dmb` after `done`).
 * @post No mailbox field nor blob byte is modified.
 *
 * @note Single-threaded; reads only the immutable published blob.
 * @since 0.1.0
 */
static bool verify_blob(volatile com33_mailbox_t* mb)
{
  if (mb == nullptr) {
    return false;
  }
  if (mb->status != (uint32_t)k_com33_status_ok) {
    return false;
  }
  if (mb->blob_base != (uint32_t)k_com33_blob_addr) {
    return false;
  }
  const uint32_t len = mb->blob_len;
  if (len == 0U) {
    return false;
  }
  if (len > (uint32_t)k_com33_blob_cap) {
    return false;
  }
  const void* base = (const void*)(uintptr_t)mb->blob_base;
  if (ra_book_validate(base, (size_t)len) != k_ra_ok) {
    return false;
  }
  const ra_book_header_t* hdr = ra_book_header(base);
  if (hdr->crc32 != mb->blob_crc) {
    return false;
  }
  if (hdr->chapter_count != mb->chapter_count) {
    return false;
  }
  /* #149 a1: byte-identity on the secondary core. The M33-produced blob must
   * equal the desktop/M85 golden (s_m33_parity_golden) byte for byte -- the same
   * acceptance the M85 parity gate proves, now on the slow core. */
  if (len != (uint32_t)k_m33_parity_golden_len) {
    return false;
  }
  if (memcmp(base, s_m33_parity_golden, (size_t)len) != 0) {
    return false;
  }
  return true;
}

/**
 * @brief Park the M85 forever after the verdict (or an unrecoverable failure).
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
 *
 * @pre The verdict (or a fatal error) has already been logged.
 * @post The M85 makes no further forward progress.
 * @post No shared state changes.
 *
 * @note Mirrors the M33 park loop for symmetry.
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
 * @details Publishes the mailbox, releases the Cortex-M33 into the emitter,
 * yields until it signals done, validates the blob the M33 built, then logs the
 * PASS/FAIL verdict and the chapter count read back from the blob. See the file
 * header for the offload narrative.
 *
 * @return Never returns (ends in ::park_forever after logging the result).
 * @retval (none) Control stays in the final spin.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held inactive by hardware until released here.
 * @post The M33 has built a RABOOK1 blob and the M85 has validated it.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
int main(void)
{
  ra_log_init();
  ra_log_info("M85", "==== RA8D2 compile_on_m33 demo (#149b emitter offload) ====");
  ra_log_info("M85", "Cortex-M85 primary core online");
  ra_log_info("M85", "shared mailbox + output blob in SRAM at 0x22100000");

  volatile com33_mailbox_t* mb = com33_mailbox();
  prep_mailbox(mb);

  ra_log_info("M85", "releasing Cortex-M33 to run the RABOOK1 emitter ...");
  const ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  ra_log_info_val("M85", "ra_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra_ok) {
    ra_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_sig(mb)) {
    ra_log_info("M85", "M33 emitter is alive");
  } else {
    ra_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra_log_info("M85", "M85 yielding -- M33 is compiling the book ...");
  if (!wait_for_done(mb)) {
    ra_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }

  /* Consumer-side barrier: order the blob/field loads AFTER observing `done`,
   * pairing with the M33's pre-`done` `dsb` so the M85 reads a settled blob. */
  __asm volatile("dmb" ::: "memory");
  ra_log_info_val("M85", "M33 reported blob length (bytes)", mb->blob_len);

  if (verify_blob(mb)) {
    ra_log_info_val("M85", "ra_book_validate OK; chapters in blob", mb->chapter_count);
    ra_log_info("M85", "compile_on_m33 PASS");
    park_forever();
  }

  /* The M33-produced blob did NOT equal the desktop/M85 golden byte for byte (or
   * failed validation). Trap so the byte-identity is GATED: board_sim's smoke
   * detects the BKPT and fails the run. A silent park here would read as a clean
   * boot and hide a compile regression (e.g. a future board_sim long-shift seam
   * miss on a DEFLATE'd fixture). On real silicon with no debugger the BKPT
   * escalates to a HardFault -- a loud, visible failure for the demo. */
  ra_log_info_val("M85", "compile_on_m33 FAIL -- status", mb->status);
  __asm volatile("bkpt #0");
  park_forever();
}
