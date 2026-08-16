/**
 * @file examples/ek_ra8d2/hw_validated/hil/cache_coherency_hil/main.c
 * @brief CPU0 (Cortex-M85) cache-coherency validator against CPU1 (Cortex-M33)
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This M85 image is built with ``RA8_BOOT_ENABLE_CACHE_MPU`` defined (see
 * the per-app ``CMakeLists.txt``), so the shared boot (``system_init.c``)
 * brings up the MPU + I-cache + D-cache before ``main`` runs. With the
 * D-cache ON, the only way a cross-core hand-off through SRAM stays
 * coherent with the cacheless Cortex-M33 is for the shared bytes to live
 * in a **non-cacheable** region -- which the boot's MPU region 4 maps
 * over ``0x22100000..0x2219FFFF``. This test exercises exactly that path
 * and proves it works without any software cache maintenance.
 *
 * Flow:
 *   1. Zero the shared block (see ``cache_coherency_shared.h``) and
 *      release the Cortex-M33 with ``ra8_cpu1_release`` (HUM Ch 2.9.1
 *      "CPU control registers").
 *   2. Round-trip a data-carrying payload: write ``ping_base + r``, DSB,
 *      bump ``ping_seq``; wait (bounded) for ``pong_seq`` to advance;
 *      verify ``pong_payload == pong_base + r``. No clean/invalidate is
 *      issued -- the non-cacheable region is what keeps it coherent.
 *   3. ::g_cache_coherency_match advances per verified round;
 *      ::g_cache_coherency_mismatch advances on a timeout or wrong echo.
 *      After the first ::k_cache_coherency_rounds verified rounds the
 *      success banner is emitted once over the VCOM console + ITM.
 *
 * Two-phase loop (the design reconciles the two HIL surfaces):
 *   - Self-test phase (rounds 0..N-1): each round toggles LED1, a
 *     peripheral (IOPORT) write. The cross-core hand-off itself is pure
 *     SRAM, which ra8_emulator's MMIO-keyed idle detector cannot see; the
 *     LED heartbeat keeps it awake so the success banner surfaces even
 *     under ``RA8_EMU_IDLE_STOP=1``. On the bench the LED visibly
 *     blinks while the test runs.
 *   - Steady phase (rounds N..): keep round-tripping forever with no
 *     heartbeat so the J-Link memprobe HIL gate sees
 *     ::g_cache_coherency_match advance across its sample window (a
 *     one-shot counter frozen in a terminal WFI would read unchanged at
 *     both probe halts and fail the delta check). In ra8_emulator this
 *     steady phase is pure SRAM, so ``RA8_EMU_IDLE_STOP`` terminates
 *     the run cleanly right after the banner -- the same clean stop a
 *     terminal WFI would give, without breaking the memprobe gate.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "cache_coherency_shared.h"
#include "ra8_boot_entry.h"
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
 * @enum cache_coherency_hil_const_t
 * @brief VCOM-console line rate for the deterministic HIL banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate. The banner is additive to the existing ``ra8_log`` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cache_coherency_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} cache_coherency_hil_const_t;

/**
 * @var g_cache_coherency_match
 * @brief HIL liveness counter -- incremented on every verified
 *        M85 -> M33 -> M85 round-trip (echo == pong_base + round).
 *
 * @details
 * Read externally by ``scripts/hil/jlink_memprobe.sh`` via SWD. The
 * probe asserts this counter advances by >= ::k_cache_coherency_rounds
 * across its sample window, proving the Cortex-M33 was released and is
 * servicing the non-cacheable shared block coherently with the M85
 * D-cache on. If the shared region were cacheable, a stale M85 read
 * would land in ::g_cache_coherency_mismatch instead.
 *
 * @note Read externally by J-Link only; firmware never reads it back.
 * @warning Do not rename; ``hil.conf`` HIL_PROBE_SYMBOL matches it exactly.
 * @since 0.1.0
 */
volatile uint32_t g_cache_coherency_match = 0U;

/**
 * @var g_cache_coherency_mismatch
 * @brief HIL failure counter -- incremented whenever a round cannot
 *        complete: the bounded pong wait timed out, or the echoed
 *        payload was not ``pong_base + round``.
 *
 * @details
 * The memprobe asserts this stays at 0. On silicon with the D-cache on
 * and a (wrongly) cacheable shared region this would climb as the M85
 * reads stale pong values; the non-cacheable region 4 keeps it at 0.
 *
 * @note Read externally by J-Link only; firmware never reads it back.
 * @warning Do not rename; ``hil.conf`` HIL_PROBE_FAILURE_SYMBOL matches it.
 * @since 0.1.0
 */
volatile uint32_t g_cache_coherency_mismatch = 0U;

/**
 * @var s_cache_coherency_pass_banner
 * @brief Deterministic HIL success banner (uart_scrape / emulator-scrape).
 * @details Emitted once, after ::k_cache_coherency_rounds rounds verify, on the
 *          success path only. Additive to the ``ra8_log`` ITM trace so a rig with
 *          no SWO capture can still gate the boot + cache + dual-core path.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; it must stay >= 12 chars and never be a substring of a
 *          failure string in the binary.
 * @since 0.1.0
 */
static const uint8_t s_cache_coherency_pass_banner[] = "cache_coherency_hil: 8 rounds PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL banner.
 *
 * @details Configures the clock tree (``ra8_cgc_init``) then the EK-RA8D2 debug
 * console (``ra8_board_uart_console_init`` at ::k_cache_coherency_hil_baud).
 * Best-effort: a failure only means the additive banner cannot reach the host;
 * the memprobe counters and the M33 hand-off are unaffected.
 *
 * @return Whether the VCOM console is ready to carry the banner.
 * @retval true  Clock + SCI8 console are up.
 * @retval false A bring-up step failed (the banner is then silently skipped).
 *
 * @pre Called once during M85 bring-up, before ``ra8_cpu1_release``.
 * @pre ``SystemInit`` has enabled the caches + MPU (this build).
 * @post On true, SCI8 is enabled and PD02/PD03 route to it.
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
  if (ra8_board_uart_console_init((uint32_t)k_cache_coherency_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console + ITM.
 *
 * @details Writes ::s_cache_coherency_pass_banner to the SCI8 / J-Link OB VCOM
 * console and flushes it, then mirrors the same verdict over ``ra8_log`` so
 * ra8_emulator surfaces it as an ``[itm]`` line. A no-op on the wire if the console
 * never came up (the write returns ``k_ra8_err_not_initialized``, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only after ::k_cache_coherency_rounds rounds verified.
 * @pre ::internal_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No shared / M33 state is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_emit_pass(void)
{
  (void)ra8_board_uart_console_write(s_cache_coherency_pass_banner,
                                     (size_t)(sizeof(s_cache_coherency_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
  ra8_log_info("M85", "cache_coherency_hil: 8 rounds PASS");
}

/**
 * @brief Park the M85 forever after an unrecoverable startup failure.
 *
 * @details Reached only if ``ra8_cpu1_release`` fails, so the M33 never started.
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
 * @brief Poll until ``pong_seq`` reaches ``target`` or the budget runs out.
 * @details Re-reads the volatile sequence field up to the fixed budget and
 *          returns immediately on the target value; a null pointer is treated
 *          as a bounded failure without dereferencing shared SRAM.
 *
 * @param[in] shared Pointer to the shared message struct (never NULL).
 * @param[in] target Sequence value to wait for (the value just written to
 *                   ``ping_seq``).
 *
 * @return Whether the wait observed the target.
 * @retval true  ``pong_seq == target`` was observed inside the budget.
 * @retval false ::k_cache_coherency_poll_budget iters elapsed first.
 *
 * @pre shared != nullptr.
 * @pre target equals the most recent value the caller wrote to ping_seq.
 * @post No shared state is mutated.
 * @post Iteration count bounded by ::k_cache_coherency_poll_budget.
 *
 * @note Not thread-safe. CPU1 must already have been released or this always
 *       times out.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_wait_for_pong(volatile cache_coherency_shared_t* shared,
                                                uint32_t                           target)
{
  if (shared == nullptr) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_cache_coherency_poll_budget; ++i) {
    if (shared->pong_seq == target) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Run one cross-core round and update the HIL counters.
 *
 * @details Writes ``ping_base + round`` (no cache clean -- region 4 is
 * non-cacheable), bumps ``ping_seq``, waits (bounded) for the M33 echo, and
 * verifies it equals ``pong_base + round``. Bumps ::g_cache_coherency_match on
 * success or ::g_cache_coherency_mismatch on a timeout / wrong echo. The two
 * checks are split (no compound decision) so each is independently visible.
 *
 * @param[in]     shared   Pointer to the shared message struct (never NULL).
 * @param[in]     round    Zero-based round index; selects the payload values.
 * @param[in,out] next_seq Next ``ping_seq`` value to publish; advanced by one.
 *
 * @return Nothing (results land in the global counters).
 *
 * @pre shared != nullptr.
 * @pre next_seq != nullptr.
 * @post Exactly one of the two HIL counters advanced by one.
 * @post ``*next_seq`` advanced by one.
 *
 * @note Not thread-safe; single-threaded M85 context.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_round(volatile cache_coherency_shared_t* shared, uint32_t round, uint32_t* next_seq)
{
  if ((shared == nullptr) || (next_seq == nullptr)) {
    return;
  }
  const uint32_t expect = (uint32_t)k_cache_coherency_pong_base + round;

  shared->ping_payload = (uint32_t)k_cache_coherency_ping_base + round;
  __asm volatile("dsb" ::: "memory");
  shared->ping_seq = *next_seq;

  bool ok = internal_wait_for_pong(shared, *next_seq);
  if (ok) {
    ok = (shared->pong_payload == expect);
  }
  if (ok) {
    g_cache_coherency_match += 1U;
  } else {
    g_cache_coherency_mismatch += 1U;
  }
  *next_seq += 1U;
}

/**
 * @brief CPU0 (Cortex-M85) application entry.
 * @details See the file header for the full behaviour summary.
 * @pre ``SystemInit`` has enabled the caches + MPU (this build defines
 *      ``RA8_BOOT_ENABLE_CACHE_MPU``).
 * @pre The M33 is held in reset by hardware until released here.
 * @post The M33 has been released and the round-trip loop is running.
 * @post This function never returns to its caller.
 * @note Single-threaded; no RTOS on the M85 in this template.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("M85", "cache_coherency_hil: D-cache ON, non-cacheable shared SRAM");

  volatile cache_coherency_shared_t* shared = internal_shared();
  /* CPU0 owns initialisation of the shared block. Zero it before
   * releasing CPU1 so CPU1 starts from a known clean state. */
  shared->ping_seq     = 0U;
  shared->pong_seq     = 0U;
  shared->ping_payload = 0U;
  shared->pong_payload = 0U;
  __asm volatile("dsb" ::: "memory");

  /* Best-effort VCOM console (banner) + LED1 (self-test heartbeat). */
  const bool console_up = internal_console_init();
  if (!console_up) {
    ra8_log_info("M85", "VCOM console init failed -- banner over ITM only");
  }
  const bool led_up = (ra8_board_led_init(k_ra8_board_led1) == k_ra8_ok);

  const ra8_err_t err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  ra8_log_info_val("M85", "ra8_cpu1_release rc (0 = ok)", (uint32_t)err);
  if (err != k_ra8_ok) {
    ra8_log_info("M85", "release FAILED -- halting");
    internal_park_forever();
  }

  uint32_t next_seq    = 1U;
  uint32_t round       = 0U;
  bool     banner_sent = false;

  while (1) {
    /* Self-test heartbeat: one IOPORT write per round keeps ra8_emulator's
     * MMIO-keyed idle detector awake through the pure-SRAM hand-off so the
     * banner surfaces under RA8_EMU_IDLE_STOP=1 (and the LED visibly
     * blinks on the bench). Dropped once the self-test has passed, so the
     * steady phase below is pure SRAM and IDLE_STOP can stop the fake. */
    if (led_up && (round < (uint32_t)k_cache_coherency_rounds)) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }

    internal_round(shared, round, &next_seq);

    if (!banner_sent && (g_cache_coherency_match >= (uint32_t)k_cache_coherency_rounds)) {
      internal_emit_pass();
      banner_sent = true;
    }
    round += 1U;
  }
}
