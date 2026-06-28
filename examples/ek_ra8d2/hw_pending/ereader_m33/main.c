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
 * almost all its time idle on a rendered page, so handing that held page to the
 * M33 @ 250 MHz -- while the M85 @ 1 GHz sleeps -- is the high-leverage battery
 * win. "Power saving = drop to the slow core."
 *
 * What the M85 does here:
 *   1. Publishes the progress mailbox (see `ereader_m33.h`) and stamps its magic
 *      so the M33 trusts it.
 *   2. Releases the Cortex-M33 with `ra_cpu1_release` (HUM Ch 2.9.1) into the
 *      reader, confirms it booted (signature), then waits for the M33 to render
 *      and publish one held page.
 *   3. Reads back the M33's published framebuffer descriptor -- SDRAM base,
 *      RGB565 geometry, glyph count and the CRC-32 the M33 folded over the
 *      rendered pixels -- validates it, and logs a single deterministic banner
 *      "ereader_m33: rgb565 256x64 sdram crc=<8 hex> PASS". board_sim echoes only
 *      the primary core's ITM, so the M85 speaks for the M33.
 *   4. Parks in low-power WFI: the M33 owns the held page.
 *
 * Why the M85 reports the M33's CRC rather than re-CRCing the framebuffer: on the
 * board_sim emulator the two cores share only the on-chip SRAM mailbox; each
 * core's external-SDRAM window is a separate mapping, so the parked M85 cannot
 * read the bytes the M33 wrote at 0x68000000. The M33 reads its own SDRAM
 * framebuffer back to fold the CRC (the proof the pixels landed) and publishes it
 * through the shared mailbox; the M85 narrates that value. On silicon the single
 * physical SDRAM is shared, so an M85 re-read would match.
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
 *          chunks between them, so the M33's render lands in a few dozen
 *          interleaves -- far inside these budgets.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_sig_poll_budget  = 10000000UL,  /**< Max iters waiting for M33 signature. */
  k_m85_done_poll_budget = 200000000UL, /**< Max iters waiting for M33 done flag. */
} m85_poll_t;

/**
 * @enum m85_banner_t
 * @brief Capacity of the verdict banner the M85 assembles in a stack buffer.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_banner_cap = 96U, /**< Bytes reserved for the assembled verdict line. */
} m85_banner_t;

/**
 * @enum m85_hex_t
 * @brief Constants for formatting a 32-bit value as 8 uppercase hex digits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_hex_nibbles = 8U,    /**< Hex digits in a 32-bit value. */
  k_nibble_bits = 4U,    /**< Bits per hex nibble.          */
  k_nibble_mask = 0x0FU, /**< Low-nibble mask.              */
} m85_hex_t;

/**
 * @brief Append a NUL-terminated string into a bounded banner buffer.
 *
 * @param[in,out] dst Destination buffer (never NULL).
 * @param[in,out] off In/out write offset; advanced by the bytes copied.
 * @param[in]     cap Capacity of @p dst (one byte reserved for the final NUL).
 * @param[in]     src Source string (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p dst, @p off and @p src are non-NULL; `*off < cap`.
 * @pre @p cap is the true size of @p dst.
 * @post `*off <= cap - 1`, leaving room for the terminating NUL.
 * @post Iteration bounded by @p cap (NASA Rule 2).
 *
 * @note Single-threaded; boot context only.
 * @since 0.1.0
 */
static void banner_append(char* dst, uint32_t* off, uint32_t cap, const char* src)
{
  if (dst == nullptr) {
    return;
  }
  if (off == nullptr) {
    return;
  }
  if (src == nullptr) {
    return;
  }
  RA_BOUNDED_LOOP(k_banner_cap);
  for (uint32_t i = 0U; i < cap; i++) {
    const char c = src[i];
    if (c == '\0') {
      break;
    }
    if (*off >= (cap - 1U)) {
      break;
    }
    dst[*off] = c;
    *off += 1U;
  }
}

/**
 * @brief Append @p value as 8 uppercase hex digits into the banner buffer.
 *
 * @param[in,out] dst   Destination buffer (never NULL).
 * @param[in,out] off   In/out write offset; advanced by up to 8.
 * @param[in]     cap   Capacity of @p dst.
 * @param[in]     value 32-bit value to format big-endian (MSB nibble first).
 *
 * @return Nothing.
 *
 * @pre @p dst and @p off are non-NULL; `*off < cap`.
 * @pre @p cap is the true size of @p dst.
 * @post `*off <= cap - 1`.
 * @post Iteration bounded by ::k_hex_nibbles (NASA Rule 2).
 *
 * @note Single-threaded; boot context only.
 * @since 0.1.0
 */
static void banner_append_hex(char* dst, uint32_t* off, uint32_t cap, uint32_t value)
{
  if (dst == nullptr) {
    return;
  }
  if (off == nullptr) {
    return;
  }
  static const char digits[] = "0123456789ABCDEF";
  RA_BOUNDED_LOOP(k_hex_nibbles);
  for (uint32_t i = 0U; i < (uint32_t)k_hex_nibbles; i++) {
    if (*off >= (cap - 1U)) {
      break;
    }
    const uint32_t shift  = ((uint32_t)k_hex_nibbles - 1U - i) * (uint32_t)k_nibble_bits;
    const uint32_t nibble = (value >> shift) & (uint32_t)k_nibble_mask;
    dst[*off]             = digits[nibble];
    *off += 1U;
  }
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
  mb->fb_base     = 0U;
  mb->fb_width    = 0U;
  mb->fb_height   = 0U;
  mb->fb_stride   = 0U;
  mb->fb_format   = 0U;
  mb->fb_crc      = 0U;
  mb->glyph_count = 0U;
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
 * @brief Poll the mailbox until the M33 publishes its held page (`done`).
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
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_done(volatile erm33_mailbox_t* mb)
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
 * @brief Validate the M33's published held-page descriptor.
 *
 * @details Confirms the render succeeded and that every published field is
 * self-consistent: the framebuffer base lies inside the SDRAM window, the
 * geometry equals ::erm33_fb_geom_t, the format is RGB565, and the glyph count
 * and CRC are non-zero (a blank or absent render publishes neither).
 *
 * @param[in] mb Pointer to the shared mailbox (never NULL).
 *
 * @return Whether the M33 published a valid held page.
 * @retval true  status ok and every descriptor field is consistent.
 * @retval false Any check failed.
 *
 * @pre @p mb is the fixed-address mailbox pointer and `done == 1`.
 * @pre A `dsb` has ordered the M33's publish before the M85's read.
 * @post No mailbox field is modified.
 * @post The return value gates the PASS / FAIL banner.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool verify_page(volatile erm33_mailbox_t* mb)
{
  if (mb->status != (uint32_t)k_erm33_status_ok) {
    return false;
  }
  if ((mb->fb_base < (uint32_t)k_erm33_sdram_base) ||
      (mb->fb_base >= (uint32_t)k_erm33_sdram_end)) {
    return false;
  }
  if ((mb->fb_width != (uint32_t)k_erm33_fb_width) ||
      (mb->fb_height != (uint32_t)k_erm33_fb_height)) {
    return false;
  }
  if (mb->fb_stride != (uint32_t)k_erm33_fb_stride) {
    return false;
  }
  if (mb->fb_format != (uint32_t)k_erm33_fb_format_rgb565) {
    return false;
  }
  if ((mb->glyph_count == 0U) || (mb->fb_crc == 0U)) {
    return false;
  }
  return true;
}

/**
 * @brief Assemble and log the single deterministic verdict banner.
 *
 * @param[in] crc  The CRC-32 the M33 published over its rendered pixels.
 * @param[in] pass Whether ::verify_page accepted the held page.
 *
 * @return Nothing.
 *
 * @pre `ra_log_init` has run (the banner reaches ITM in a Debug build).
 * @pre @p crc is the value read from the mailbox.
 * @post Exactly one banner line is emitted.
 * @post No shared state is modified.
 *
 * @note The board_sim gate greps this line for `crc=<hex>` and the verdict word.
 * @since 0.1.0
 */
static void emit_verdict(uint32_t crc, bool pass)
{
  char     buf[k_banner_cap];
  uint32_t off = 0U;
  banner_append(buf, &off, (uint32_t)k_banner_cap, "ereader_m33: rgb565 256x64 sdram crc=");
  banner_append_hex(buf, &off, (uint32_t)k_banner_cap, crc);
  banner_append(buf, &off, (uint32_t)k_banner_cap, pass ? " PASS" : " FAIL");
  buf[off] = '\0';
  ra_log_info("M85", buf);
}

/**
 * @brief Park the M85 in low-power WFI after the M33 owns the page.
 *
 * @return This function never returns.
 * @retval (none) The M85 sleeps until power-off (or a future M33 wake IRQ).
 *
 * @pre The M33 has published the held page and the verdict was logged.
 * @pre The verdict banner has already been emitted.
 * @post The M85 spends its time in WFI, not busy-spinning (the power win).
 * @post No mailbox field is modified.
 *
 * @note On silicon WFI sleeps until an interrupt; with no wake source wired yet
 *       the M85 stays asleep -- exactly the low-power posture.
 * @since 0.1.0
 */
[[noreturn]] static void park_low_power(void)
{
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
 * @details Publishes the mailbox, releases the Cortex-M33 into the reader, waits
 * for it to render and publish one held page, validates the published
 * descriptor, logs the verdict banner, then parks in low-power WFI. See the file
 * header for the power-saving narrative.
 *
 * @return Never returns (ends in ::park_low_power, or ::park_forever on error).
 * @retval (none) Control stays parked.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held inactive by hardware until released here.
 * @post The M33 has rendered the held page and the M85 is parked.
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
  ra_log_info("M85", "M33 renders a held page into external SDRAM (0x68000000)");

  volatile erm33_mailbox_t* mb = erm33_mailbox();
  prep_mailbox(mb);

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

  ra_log_info("M85", "M85 idle; waiting for the M33 to render the held page ...");
  if (!wait_for_done(mb)) {
    ra_log_info("M85", "M33 done flag not seen -- timed out");
    park_forever();
  }
  __asm volatile("dsb" ::: "memory");

  ra_log_info_val("M85", "M33 published fb_base", mb->fb_base);
  ra_log_info_val("M85", "M33 held-page glyphs", mb->glyph_count);
  ra_log_info_val("M85", "M33 held-page pixels CRC32 (decimal)", mb->fb_crc);
  emit_verdict(mb->fb_crc, verify_page(mb));
  park_low_power();
}
