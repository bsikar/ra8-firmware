/**
 * @file examples/ek_ra8d2/cpu1_pingpong_ipc/main.c
 * @brief CPU0 <-> CPU1 ping-pong via the IPC peripheral (work-in-progress)
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * Companion to ``cpu1_pingpong`` (which uses shared SRAM2). This
 * variant attempts to use the chip's IPC channels for the ping-pong
 * round-trip instead of shared memory. The constraint we need to
 * work around: CPU1 has SECEXT disabled (HUM Ch 2), so it can only
 * run non-secure. The IPC channel security attribution
 * (``IPCSAR.SAIPCIRn`` -- HUM Ch 3.2.1) is one bit per channel that
 * picks Secure (0, the reset default) or Non-secure (1) access; once
 * set to NS the channel is accessible to both NS and S code (because
 * ARMv8-M Secure state can always reach NS-attributed memory).
 *
 * The blocker is writing IPCSAR itself: it lives in the CPSCU window
 * and is only writable from Secure state. The current build's
 * Reset_Handler runs at 0x02000000 -- NS per the chip's IDAU default
 * (bit 28 = 0) -- so CPU0 is already in NS by the time ``main()``
 * begins.
 *
 * This app probes that exact question on bench: it tries the IPCSAR
 * write from main() and stashes the read-back into a JLink-visible
 * global. If the write takes, CPU0 is in S and we can finish the IPC
 * implementation in-place. If the write is dropped (read-back == 0)
 * we need a secure-boot stub at a Secure-attributed reset vector.
 *
 * Lives in ``hw_pending/`` until either bench path is resolved. The
 * working ``cpu1_pingpong`` (SRAM2 variant) is in ``hw_validated/hil/``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_dual_core.h"
#include "ra_err.h"

extern uint32_t g_ra_ls_cpu1_mram_start;
extern uint32_t g_ra_ls_cpu1_stack_top;

/**
 * @var g_cpu1_pingpong_ipc_xpsr
 * @brief XPSR snapshot at the very top of main().
 *
 * @details Read via JLink memprobe. IPSR bits encode whether we're
 * in Thread mode vs an exception; used mostly to confirm the
 * experiment ran rather than to decode security state directly
 * (XPSR does not expose CONTROL.S on M85).
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_xpsr = 0xFFFFFFFFU;

/**
 * @var g_cpu1_pingpong_ipc_ipcsar_pre
 * @brief IPCSAR read BEFORE the experimental write.
 *
 * @details A nonzero pre-value would mean some prior boot code
 * (boot ROM, system_init) wrote IPCSAR for us. Reset default is 0.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_ipcsar_pre = 0xFFFFFFFFU;

/**
 * @var g_cpu1_pingpong_ipc_ipcsar_post
 * @brief IPCSAR read AFTER writing ``SAIPCIR2 | SAIPCIR0 = 1`` to it.
 *
 * @details If this read shows ``0x00050000`` we are in S mode and
 * the write took; the IPC channels are now NS-attributed and CPU1
 * can reach them. If it shows ``0x00000000`` we are in NS and the
 * write was silently dropped; a secure-boot stub is required to
 * enable the IPC variant.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_ipcsar_post = 0xFFFFFFFFU;

/**
 * @var g_cpu1_pingpong_ipc_release_err
 * @brief Captured ra_cpu1_release return code.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_release_err = 0xFFFFFFFFU;

/**
 * @brief CPU0 entry. Snapshot state, attempt IPCSAR write, idle.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss.
 * @pre System clocks configured by system_init.
 * @post Global diag vars stamped with bench-readable observations.
 * @post Loop never exits.
 *
 * @note Single-threaded.
 * @since 0.1.0
 */
int main(void)
{
  uint32_t xpsr = 0U;
  __asm__ volatile("mrs %0, xpsr" : "=r"(xpsr));
  g_cpu1_pingpong_ipc_xpsr = xpsr;

  volatile uint32_t* ipcsar      = (volatile uint32_t*)0x40008610UL;
  g_cpu1_pingpong_ipc_ipcsar_pre = *ipcsar;
  /* SAIPCIR0 (bit 16, IPC0 ch0, CPU1->CPU0 path)
   * SAIPCIR2 (bit 18, IPC1 ch0, CPU0->CPU1 path)
   * Set both to 1 to flip those channels to NS-attributed. */
  *ipcsar = 0x00050000U;
  __asm__ volatile("dsb" ::: "memory");
  g_cpu1_pingpong_ipc_ipcsar_post = *ipcsar;

  ra_err_t err = ra_cpu1_release(&g_ra_ls_cpu1_mram_start, &g_ra_ls_cpu1_stack_top);
  g_cpu1_pingpong_ipc_release_err = (uint32_t)err;

  /* Stop here -- the bench reads the globals via JLink to decide
   * the next implementation step. */
  while (1) {
    __asm__ volatile("wfi");
  }
}
