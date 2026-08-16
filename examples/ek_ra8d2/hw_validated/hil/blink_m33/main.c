/**
 * @file examples/ek_ra8d2/hw_validated/hil/blink_m33/main.c
 * @brief CPU0 (Cortex-M85 primary core) launcher for the M33 blink template
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the Cortex-M85,
 * out of reset. It is the canonical "target the M33" template (issue #152): the
 * M85 does the minimum -- release the secondary Cortex-M33 and then sleep -- and
 * the M33 owns the work. Here the work is blinking LED1, but any portable app
 * drops into `cpu1_main.c` the same way.
 *
 *   1. Two images, one per core. This M85 ELF carries an embedded Cortex-M33
 *      image (`.cpu1_image`, built from `cpu1_main.c` by the shared
 *      `ra8_add_cpu1_image()` recipe). One `.hex` flashes both cores.
 *
 *   2. The co-processor / low-power model. The M85 releases the M33 with
 *      `ra8_cpu1_release` (HUM Ch 2.9.1 "CPU control registers"), then drops into
 *      a WFI idle loop. With the heavy M85 asleep and the lean M33 doing the
 *      work, this *is* the low-power posture issue #150 builds on.
 *
 * Everything the M85 does is logged with `ra8_log`, which the emulator echoes as
 * `[itm]` lines (the J-Link SWO trace analog). The M33's proof-of-life is the
 * blinking LED the M85 never drives. See `README.md`.
 *
 * @note `ra8_log_info` is compiled to a no-op unless the build defines a log
 *       level of INFO or finer (a Debug build). `make emu-blink_m33` builds
 *       Debug so the `[itm]` lines appear.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
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
 * @enum blink_m33_hil_t
 * @brief VCOM-console line rate for the deterministic HIL success banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate; the Pi HIL rig's `uart_scrape` reads /dev/ttyACM0 to gate the
 *          app. The banner is additive to the existing `ra8_log` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_blink_m33_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} blink_m33_hil_t;

/**
 * @var s_blink_m33_pass_banner
 * @brief Deterministic HIL banner: the M33 secondary core was released (uart_scrape).
 * @details This template has no shared-SRAM mailbox -- the M33's proof-of-life is
 *          the LED1 blink the M85 never drives. The strongest verdict the M85 can
 *          report over VCOM is that `ra8_cpu1_release` succeeded; the operator (or
 *          the bench rig) confirms the blink visually. Emitted only on the
 *          success path, additive to the existing `ra8_log` ITM trace, so the Pi
 *          rig (which has no SWO / ITM capture) can gate the boot + release path.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; the HIL gate (hil.conf HIL_EXPECT) matches it exactly.
 * @since 0.1.0
 */
static const uint8_t s_blink_m33_pass_banner[] = "blink_m33: m33 released PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL success banner.
 *
 * @details
 * Configures the clock tree (`ra8_cgc_init`, which publishes the PCLKA the SCI8
 * BRR divisor is computed from -- and which the released M33 then shares) then
 * the EK-RA8D2 debug console (`ra8_board_uart_console_init`, SCI8 on PD02/PD03 at
 * ::k_blink_m33_hil_baud). Best-effort: a failure only means the additive HIL
 * banner cannot reach the host; the existing `ra8_log` ITM trace and the M33
 * release are unaffected.
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
RA8_INTERNAL static bool internal_console_init(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_blink_m33_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console.
 *
 * @details
 * Writes ::s_blink_m33_pass_banner to the SCI8 / J-Link OB VCOM console and
 * flushes it so the bytes clock out before the M85 parks in WFI. A no-op if the
 * console never came up (the write returns `k_ra8_err_not_initialized`, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only after `ra8_cpu1_release` returned k_ra8_ok (single guard).
 * @pre ::internal_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No M33 / mailbox state is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_emit_pass(void)
{
  (void)ra8_board_uart_console_write(s_blink_m33_pass_banner,
                                     (size_t)(sizeof(s_blink_m33_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @details Reached only if `ra8_cpu1_release` fails, so the M33 never started.
 * The core spins with the failure already logged.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A fatal error (CPU1 release failure) has occurred.
 * @pre The failure has already been logged.
 * @post The M85 makes no further forward progress.
 * @post The M33 stays held in reset.
 *
 * @note Mirrors the M33's fault handler for symmetry.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_park_forever(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief Idle the M85 forever once the M33 owns the work (low-power posture).
 *
 * @details After releasing the M33 the M85 has nothing to do, so it sleeps in a
 * WFI loop. Modelling the heavy core asleep while the lean M33 runs is the
 * low-power co-processor posture this template exists to demonstrate.
 *
 * @return This function never returns.
 * @note The core stays asleep between interrupts.
 *
 * @pre The M33 has been released successfully.
 * @pre No further M85-side work is pending.
 * @post The M85 consumes minimal power between wake events.
 * @post The M33 continues blinking independently.
 *
 * @note WFI wakes on any pending interrupt; this template enables none, so the
 *       M85 simply stays asleep.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_idle_forever(void)
{
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief CPU0 (Cortex-M85) application entry.
 *
 * @details Brings up logging, releases the Cortex-M33 (which then blinks LED1),
 * and idles. See the file header for the teaching narrative.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held in reset by hardware until released here.
 * @post The M33 has been released and is blinking LED1.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this template.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 dual-core blink template ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");

  /* Bring up the VCOM console so the post-release success path can emit the HIL
   * banner. Best-effort: a failure is narrated over ITM and the demo continues. */
  if (!internal_console_init()) {
    ra8_log_info("M85", "VCOM console init failed -- HIL banner unavailable");
  }

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");

  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    internal_park_forever();
  }

  ra8_log_info("M85", "M33 released -- it now blinks LED1 (BLUE, P600)");
  /* Additive HIL banner: the M33 has been released (its blink is visually
   * confirmed on the bench); mirror the verdict to VCOM for uart_scrape. */
  internal_emit_pass();
  ra8_log_info("M85", "M85 entering WFI idle -- low-power co-processor model");
  internal_idle_forever();
}
