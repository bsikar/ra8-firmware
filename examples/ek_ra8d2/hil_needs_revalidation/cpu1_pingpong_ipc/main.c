/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/main.c
 * @brief CPU0 (M85) Secure fallback entry for the TZ ping-pong demo.
 *
 * @par Tag
 * [Ring 1 / app] {World: S}
 *
 * @details
 * On the happy path this function is never reached. ``SystemInit`` calls
 * ``ra8_trustzone_init``, which programmes the SAU, writes
 * IPCSAR=0x00050000, releases CPU1 via ``ra8_cpu1_release`` (still in S),
 * then BLXNS-es into the NS image at 0x02080000 (``ns_reset_handler``
 * in ``ns_main.c``). BLXNS does not return on hardware, so the
 * ``Reset_Handler`` step that calls ``main()`` is unreachable.
 *
 * If the BLXNS path fails -- the NS vector table is blank, the SAU
 * could not be programmed, or the IPCSAR write was rejected -- the
 * secure-boot library returns and ``main`` becomes the fallback. We
 * stamp a diagnostic counter so the bench memprobe can distinguish
 * "BLXNS succeeded -> main never ran" (counter stays 0) from "BLXNS
 * failed -> main ran" (counter advances). The fallback then parks
 * the CPU in a NOP loop so it cannot accidentally touch the
 * NS-attributed IPC channels (which would BusFault in S state).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

/**
 * @var g_cpu1_pingpong_s_fallback_count
 * @brief Bench diagnostic: bumps when the S-side fallback main runs.
 *
 * @details
 * Stays 0 on the happy path because BLXNS in ``ra8_trustzone_init``
 * never returns. Any non-zero value means the secure-boot library
 * bailed out before transferring control to the NS image -- bench
 * scripts should treat that as a regression in the TZ scaffolding,
 * not in the NS ping-pong logic.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 *
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_s_fallback_count = 0U;

/**
 * @brief CPU0 S-side fallback entry point.
 *
 * @details See file header.
 *
 * @return Never returns.
 * @retval 0 Never returned.
 *
 * @pre Boot init has completed.
 * @pre The secure-boot library's BLXNS into NS image either failed or
 *      was skipped (the call site in ``ra8_trustzone_init`` is a no-op
 *      on host builds).
 * @post Diagnostic counter latched, CPU parked in a halt loop.
 * @post Function never returns.
 *
 * @note Single-threaded entry.
 *
 * @since 0.1.0
 */
int main(void)
{
  g_cpu1_pingpong_s_fallback_count = 1U;

  for (;;) {
    __asm__ volatile("nop");
  }
}
