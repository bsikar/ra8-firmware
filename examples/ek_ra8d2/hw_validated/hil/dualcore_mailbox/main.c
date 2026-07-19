/**
 * @file examples/ek_ra8d2/hw_validated/hil/dualcore_mailbox/main.c
 * @brief CPU0 (Cortex-M85 primary core) driver for the dual-core demo
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *primary* core, the
 * Cortex-M85, out of reset. It demonstrates the two things this example
 * exists to teach:
 *
 *   1. Two separate images, one per core. This M85 ELF carries an embedded
 *      Cortex-M33 image (`.cpu1_image`, built from `cpu1_main.c`). The two
 *      were compiled independently for two different CPU architectures and
 *      stitched into one `.hex` so a single flash brings up both cores.
 *
 *   2. Using both cores. The M85 releases the M33 with `ra8_cpu1_release`
 *      (HUM Ch 2.9.1 "CPU control registers"), then exchanges messages with
 *      it through a shared-SRAM mailbox (see `dualcore_mailbox.h`). Each
 *      round the M85 sends an operand and the M33 sends back `operand*3+1`.
 *
 * Everything the M85 does is logged with `ra8_log`, which the emulator echoes
 * as `[itm]` lines (the analog of the J-Link SWO trace console on hardware).
 * The M33 cannot print directly in the emulator (board_sim echoes only the
 * primary core's ITM), so the M85 logs the M33's mailbox replies on its
 * behalf -- and because a reply value of `operand*3+1` can only have been
 * produced by the M33 executing code, those lines are honest proof that the
 * second core is alive. See `README.md`.
 *
 * @note `ra8_log_info` is compiled to a no-op unless the build defines a
 *       log level of INFO or finer (a Debug build). `make sim-dualcore_mailbox`
 *       builds Debug, so the `[itm]` lines appear; a default release build
 *       still runs the dual-core exchange but stays silent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "dualcore_mailbox.h"
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
 * @enum dualcore_mailbox_hil_t
 * @brief VCOM-console line rate for the deterministic HIL success banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate; the Pi HIL rig's `uart_scrape` reads /dev/ttyACM0 to gate the
 *          app. The banner is additive to the existing `ra8_log` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dualcore_mailbox_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} dualcore_mailbox_hil_t;

/**
 * @var k_dualcore_mailbox_pass_banner
 * @brief Deterministic, run-to-run-stable HIL success banner (uart_scrape gate).
 * @details Emitted over the SCI8 / J-Link OB VCOM console only when all
 *          ::k_m85_demo_rounds mailbox rounds returned the M33's `operand*3+1`
 *          reply -- a value only the second core executing can produce. The ITM
 *          `ra8_log` verdict is left intact; this is purely additive so the Pi
 *          rig (which has no SWO / ITM capture) can gate the dual-core handshake.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; the HIL gate (hil.conf HIL_EXPECT) matches it exactly.
 * @since 0.1.0
 */
static const uint8_t k_dualcore_mailbox_pass_banner[] = "dualcore_mailbox: 6 rounds PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL success banner.
 *
 * @details
 * Configures the clock tree (`ra8_cgc_init`, which publishes the PCLKA the SCI8
 * BRR divisor is computed from -- and which the released M33 then shares) then
 * the EK-RA8D2 debug console (`ra8_board_uart_console_init`, SCI8 on PD02/PD03 at
 * ::k_dualcore_mailbox_hil_baud). Best-effort: a failure only means the additive
 * HIL banner cannot reach the host; the `ra8_log` ITM trace and the dual-core
 * exchange are unaffected.
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
static bool dualcore_mailbox_hil_console_init(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_dualcore_mailbox_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console.
 *
 * @details
 * Writes ::k_dualcore_mailbox_pass_banner to the SCI8 / J-Link OB VCOM console
 * and flushes it so the bytes clock out before the M85 drops into the idle
 * heartbeat. A no-op if the console never came up (the write returns
 * `k_ra8_err_not_initialized`, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only when every demo round verified (single, non-compound guard).
 * @pre ::dualcore_mailbox_hil_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No mailbox field is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static void dualcore_mailbox_hil_emit_pass(void)
{
  (void)ra8_board_uart_console_write(k_dualcore_mailbox_pass_banner,
                                     (size_t)(sizeof(k_dualcore_mailbox_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
}

/**
 * @enum m85_demo_const_t
 * @brief Tunables for the M85 driver loop.
 * @details Round count for the narrated demo phase and the busy-wait length
 *          that paces the idle heartbeat so its `[itm]` lines stay readable.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m85_demo_rounds   = 6U,        /**< Narrated request/reply rounds at start. */
  k_m85_heartbeat_nop = 4000000UL, /**< Bounded nop spins between heartbeats.   */
} m85_demo_const_t;

/**
 * @brief Poll the mailbox until the M33 acks @p target or the budget expires.
 *
 * @param[in] mb     Pointer to the shared mailbox.
 * @param[in] target Sequence value the M85 most recently sent.
 *
 * @return Whether the matching ack was observed.
 * @retval true  `reply_seq == target` within the poll budget.
 * @retval false ::k_dualcore_poll_budget iterations elapsed first.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre @p target equals the value just written to `request_seq`.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_dualcore_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; assumes the M33 was released or it always times out.
 * @since 0.1.0
 */
static bool wait_for_reply(volatile dualcore_mailbox_t* mb, uint32_t target)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dualcore_poll_budget; i++) {
    if (mb->reply_seq == target) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Poll the mailbox until the M33 stamps its boot signature.
 *
 * @param[in] mb Pointer to the shared mailbox.
 *
 * @return Whether the M33 signature appeared.
 * @retval true  `m33_signature == k_dualcore_m33_signature` within budget.
 * @retval false ::k_dualcore_poll_budget iterations elapsed first.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre The M85 has already called `ra8_cpu1_release`.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_dualcore_poll_budget (NASA Rule 2).
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool wait_for_m33_boot(volatile dualcore_mailbox_t* mb)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dualcore_poll_budget; i++) {
    if (mb->m33_signature == (uint32_t)k_dualcore_m33_signature) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Send one operand to the M33 and verify the transformed reply.
 *
 * @param[in,out] mb  Pointer to the shared mailbox.
 * @param[in]     seq Operand value, also used as the round sequence number.
 *
 * @return Whether a correct reply came back.
 * @retval true  The M33 replied `seq*3+1` inside the poll budget.
 * @retval false The reply timed out or carried the wrong value.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre @p seq is strictly greater than the previous sequence sent.
 * @post `request` and `request_seq` reflect this round.
 * @post No M33-owned field is written by this core.
 *
 * @note Logs nothing; callers decide whether to narrate the result.
 * @since 0.1.0
 */
static bool ping_once(volatile dualcore_mailbox_t* mb, uint32_t seq)
{
  mb->request = seq;
  __asm volatile("dsb" ::: "memory");
  mb->request_seq = seq;
  if (!wait_for_reply(mb, seq)) {
    return false;
  }
  const uint32_t expected = (seq * (uint32_t)k_dualcore_m33_mul) + (uint32_t)k_dualcore_m33_add;
  return (mb->reply == expected);
}

/**
 * @brief Run one narrated request/reply round, logging both directions.
 *
 * @param[in,out] mb  Pointer to the shared mailbox.
 * @param[in]     seq Operand / sequence number for this round.
 *
 * @return Whether the round succeeded.
 * @retval true  The M33 returned the expected `seq*3+1`.
 * @retval false The reply was missing or wrong.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre The M33 has been released.
 * @post Two or three `[itm]` lines describe the exchange.
 * @post Returns the liveness verdict for the caller to act on.
 *
 * @note Wraps ::ping_once with logging so ::heartbeat_loop can stay quiet.
 * @since 0.1.0
 */
static bool do_round(volatile dualcore_mailbox_t* mb, uint32_t seq)
{
  ra8_log_info_val("M85", "  -> sent operand to M33", seq);
  if (!ping_once(mb, seq)) {
    ra8_log_info("M85", "  !! M33 reply missing or wrong");
    return false;
  }
  ra8_log_info_val("M85", "  <- M33 replied (3n+1)", mb->reply);
  return true;
}

/**
 * @brief Zero every mailbox field before the M33 is released.
 *
 * @param[out] mb Pointer to the shared mailbox.
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre Called before `ra8_cpu1_release` so the M33 starts from a clean slate.
 * @post All six mailbox words read back as 0.
 * @post A `dsb` has drained the writes to shared SRAM.
 *
 * @note Single owner (M85) at this point; no concurrency.
 * @since 0.1.0
 */
static void zero_mailbox(volatile dualcore_mailbox_t* mb)
{
  mb->request_seq   = 0U;
  mb->reply_seq     = 0U;
  mb->request       = 0U;
  mb->reply         = 0U;
  mb->m33_signature = 0U;
  mb->m33_replies   = 0U;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Idle heartbeat: keep pinging the M33 and log its liveness forever.
 *
 * @param[in,out] mb   Pointer to the shared mailbox.
 * @param[in]     seq0 Last sequence number used by the demo phase.
 *
 * @return This function never returns.
 * @note The heartbeat loops until power-off.
 *
 * @pre @p mb is the fixed-address mailbox pointer (never NULL).
 * @pre The demo rounds have completed.
 * @post Each beat advances the sequence and logs one liveness line.
 * @post The loop never exits.
 *
 * @note The nop pacing keeps the `[itm]` stream readable, not flooded.
 * @since 0.1.0
 */
[[noreturn]] static void heartbeat_loop(volatile dualcore_mailbox_t* mb, uint32_t seq0)
{
  uint32_t seq = seq0;
  while (1) {
    for (volatile uint32_t d = 0U; d < (uint32_t)k_m85_heartbeat_nop; d++) {
      __asm volatile("nop");
    }
    seq++;
    if (ping_once(mb, seq)) {
      ra8_log_info_val("M85", "heartbeat: both cores alive, M33 replies", mb->m33_replies);
    } else {
      ra8_log_info("M85", "heartbeat: M33 stopped responding");
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
 * @note Mirrors the M33's fault handler for symmetry.
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
 * @details Brings up logging, releases the Cortex-M33, confirms it booted,
 * runs ::k_m85_demo_rounds narrated mailbox rounds, then drops into the idle
 * heartbeat. See the file header for the teaching narrative.
 *
 * @return Never returns (ends in ::heartbeat_loop).
 * @retval (none) Control stays in the heartbeat loop.
 *
 * @pre `SystemInit` has completed core bring-up.
 * @pre The M33 is held in reset by hardware until released here.
 * @post The M33 has been released and the mailbox exchange is running.
 * @post This function never returns to its caller.
 *
 * @note Single-threaded; no RTOS on the M85 in this example.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_init();
  ra8_log_info("M85", "==== RA8D2 dual-core mailbox demo ====");
  ra8_log_info("M85", "Cortex-M85 primary core online");
  ra8_log_info("M85", "mailbox in shared SRAM at 0x22100000");

  /* Bring up the VCOM console so the success path can emit the HIL banner.
   * Best-effort: a failure is narrated over ITM and the demo continues. */
  if (!dualcore_mailbox_hil_console_init()) {
    ra8_log_info("M85", "VCOM console init failed -- HIL banner unavailable");
  }

  volatile dualcore_mailbox_t* mb = dualcore_mailbox();
  zero_mailbox(mb);

  ra8_log_info("M85", "releasing Cortex-M33 secondary core ...");
  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    park_forever();
  }

  if (wait_for_m33_boot(mb)) {
    ra8_log_info_val("M85", "M33 is up; boot signature", mb->m33_signature);
  } else {
    ra8_log_info("M85", "M33 signature not seen -- did it boot?");
  }

  ra8_log_info("M85", "-- exchanging messages with the M33 --");
  uint32_t seq       = 0U;
  uint32_t ok_rounds = 0U;
  for (uint32_t r = 0U; r < (uint32_t)k_m85_demo_rounds; r++) {
    seq++;
    if (do_round(mb, seq)) {
      ok_rounds++;
    }
  }

  ra8_log_info_val("M85", "demo done; total M33 replies", mb->m33_replies);
  ra8_log_info("M85", "both cores confirmed alive -- idle heartbeat follows");
  /* Additive HIL banner: emit only when EVERY round returned the M33's
   * operand*3+1 reply (a value only the second core executing can produce), so
   * the Pi rig's uart_scrape gates the full dual-core handshake over VCOM. */
  if (ok_rounds == (uint32_t)k_m85_demo_rounds) {
    dualcore_mailbox_hil_emit_pass();
  }
  heartbeat_loop(mb, seq);
}
