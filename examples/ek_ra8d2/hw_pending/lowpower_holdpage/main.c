/**
 * @file examples/ek_ra8d2/hw_pending/lowpower_holdpage/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the low-power hold-page demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It demonstrates the #150 power-saving model: for a static
 * e-reader page (text shown, waiting for a tap or a page-turn) the M85 @ 1 GHz
 * is wasted, so the M85 hands the page to the M33 @ 250 MHz and PARKS. The slow
 * core is plenty to hold a rendered page and service the user switch, at a
 * fraction of the power -- "power saving = drop to the slow core".
 *
 * What the M85 does here:
 *   1. "Renders" page 0. A real GLCDC render is M85-heavy and out of scope for
 *      this plumbing demo, so a page marker written into the shared mailbox
 *      (see `lowpower_holdpage.h`) stands in for the rendered framebuffer.
 *   2. Releases the Cortex-M33 with `ra8_cpu1_release` (HUM Ch 2.9.1) into its
 *      hold loop, then PARKS in a WFI sleep -- the low-power posture. From here
 *      the M33 owns the page.
 *
 * The wake path -- the M33 signalling the M85 to spin back up for a heavy
 * re-render (opening or compiling a book, see #149) -- is the remaining piece
 * tracked in #150; this example lands the M85-parks / M33-holds foundation.
 *
 * @note `ra8_log_info` is compiled to a no-op unless the build defines INFO-level
 *       logging (a Debug build). The M33 cannot print in the emulator (ra8_emulator
 *       echoes only the primary core's ITM), so the M85 narrates the M33's hold
 *       heartbeat from the shared mailbox; a climbing heartbeat is honest proof
 *       the M33 is the live core while the M85 sleeps.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "lowpower_holdpage.h"
#include "ra8_boot_entry.h"
#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Base of the embedded M33 image / its vector table (MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_mram_start;
/** @brief Initial stack pointer handed to the M33 at release. */
extern uint32_t g_ra8_ls_cpu1_stack_top;

/**
 * @enum m85_lowpower_const_t
 * @brief Pacing for the parked-M85 narration.
 * @details While parked the M85 wakes from WFI, reads the M33's hold heartbeat,
 *          and re-sleeps. It logs only every Nth wake so the `[itm]` stream
 *          stays readable rather than flooded.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_park_log_every = 240U, /**< Log the M33 heartbeat every Nth WFI wake. */
} m85_lowpower_const_t;

/**
 * @brief "Render" page 0 and publish it to the M33 via the shared mailbox.
 *
 * @param[out] mb Pointer to the shared mailbox.
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre Called before `ra8_cpu1_release` so the M33 sees a published page.
 * @post `page_num`, `active_core`, and `magic` describe a held page owned by
 *       the M33, with the counters zeroed.
 * @post A `dsb` has drained the writes to shared SRAM before the M33 starts.
 *
 * @note A GLCDC render is M85-heavy; the page marker stands in for it.
 * @since 0.1.0
 */
static void render_page0(volatile lowpower_mailbox_t* mb)
{
  mb->magic         = 0U;
  mb->page_num      = 0U;
  mb->active_core   = (uint32_t)k_lowpower_core_m33;
  mb->m33_heartbeat = 0U;
  mb->sw1_events    = 0U;
  __asm volatile("dsb" ::: "memory");
  mb->magic = (uint32_t)k_lowpower_magic;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Park the M85 in low-power WFI sleep, narrating the M33's hold loop.
 *
 * @param[in] mb Pointer to the shared mailbox.
 *
 * @return This function never returns.
 * @note The M85 sleeps until power-off (or, in a future revision, an
 *         M33 wake interrupt for a heavy re-render).
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre The M33 has been released and owns the page.
 * @post The M85 spends its time in WFI, not busy-spinning (the power win).
 * @post Every ::k_m85_park_log_every wakes, one liveness line is logged.
 *
 * @note On silicon WFI sleeps until an interrupt; with no wake source wired yet
 *       the M85 stays asleep, which is exactly the low-power posture. In
 *       ra8_emulator WFI fast-forwards, so the narration still advances.
 * @since 0.1.0
 */
[[noreturn]] static void park_low_power(volatile lowpower_mailbox_t* mb)
{
  (void)mb; /* Read only inside ra8_log_info_val(), which a non-Debug build elides. */
  uint32_t wakes = 0U;
  while (1) {
    __asm volatile("wfi");
    wakes++;
    if ((wakes % (uint32_t)k_m85_park_log_every) == 0U) {
      ra8_log_info_val("M85", "parked (WFI); M33 hold heartbeat", mb->m33_heartbeat);
    }
  }
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A fatal error (e.g. CPU1 release failure) has occurred.
 * @pre The failure has already been logged.
 * @post The M85 makes no further forward progress.
 * @post No mailbox state changes.
 *
 * @note Distinct from ::park_low_power: this is an error halt, not the
 *       intended low-power sleep.
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
 * @details Brings up logging, "renders" page 0 into the shared mailbox,
 * releases the Cortex-M33 into its hold loop, and parks the M85 in low-power
 * WFI sleep. See the file header for the power-saving narrative.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held inactive by hardware until released here.
 * @post The M33 owns the held page and the M85 is parked.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 low-power hold-page demo ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");
  ra8_log_info("M85", "mailbox in shared SRAM at 0x22100000");

  volatile lowpower_mailbox_t* mb = lowpower_mailbox();
  render_page0(mb);
  ra8_log_info("M85", "rendered page 0; handing the held page to the M33");

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  ra8_log_info("M85", "low-power: page handed to M33; M85 parking (clock-gated posture)");
  park_low_power(mb);
}
