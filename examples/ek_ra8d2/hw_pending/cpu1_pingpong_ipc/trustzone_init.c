/**
 * @file examples/ek_ra8d2/hw_pending/cpu1_pingpong_ipc/trustzone_init.c
 * @brief Per-app secure-boot wiring for cpu1_pingpong_ipc
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Thin wrapper around ``libs/ra_tz_secure_boot/`` -- the per-app file
 * exists because the auto-discovery glue scans for ``trustzone_init.c``
 * in each example directory. The actual SAU + CPSCU + BLXNS sequence
 * lives in ``ra_tz_secure_boot_run`` so other apps can share it.
 *
 * When ``RA_TRUSTZONE_ENABLE`` is not defined the function is a no-op
 * so the single-world build is unaffected. When it is defined the
 * secure-boot:
 *   1. Programmes the five-region SAU partition.
 *   2. Unlocks ``PRCR_S.PRC4`` and writes ``IPCSAR = 0x00050000``
 *      (SAIPCIR0 + SAIPCIR2 set) so channels 0 and 2 -- the two CPU1
 *      (always-NS) endpoints -- become NS-accessible.
 *   3. Re-locks PRCR_S.
 *   4. BLXNS-es into the NS image starting at ``NS_VECTOR_TABLE``,
 *      which the linker places at the NS-MRAM base (0x02080000).
 *
 * Bench validation is NOT performed by this commit -- a human operator
 * with ``scripts/hil_recover.sh`` warm is the only safe path to flash
 * the resulting image (see ``project_sau_sgstubs_brick``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_log.h"
#include "ra_tz_secure_boot.h"

/**
 * @enum cpu1_pingpong_ipc_tz_const_t
 * @brief Constants for the cpu1_pingpong_ipc secure boot.
 *
 * @details
 *  - ``k_ipcsar_value`` matches the acceptance criterion of issue #22:
 *    SAIPCIR0 (bit 16) + SAIPCIR2 (bit 18) = 0x00050000.
 *  - ``k_ipcpar_value`` stays at 0 -- channels remain Privileged-only.
 *  - ``k_ns_vector_table_addr`` is the linker-pinned start of NS MRAM
 *    where the NS image's vector table is placed.
 *
 * @invariant ``k_ipcsar_value`` must clear bits 17/19 so SAIPCIR1 and
 *            SAIPCIR3 stay Secure (CPU0 owns those channels).
 *
 * @see ra_tz_secure_boot_run
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ipcsar_value         = 0x00050000UL, /**< SAIPCIR0 + SAIPCIR2 = NS. */
  k_ipcpar_value         = 0x00000000UL, /**< Privileged-only (default).*/
  k_ns_vector_table_addr = 0x02080000UL, /**< NS image entry vectors.   */
} cpu1_pingpong_ipc_tz_const_t;

/**
 * @var g_cpu1_pingpong_ipc_ipcsar_post
 * @brief HIL post-secure-boot IPCSAR read-back diagnostic.
 *
 * @details
 * Bench scripts read this through SWD: it must hold ``0x00050000``
 * after the secure boot has finished. The variable is updated from
 * within ``ra_trustzone_init`` immediately after the IPCSAR write so a
 * J-Link memprobe can pinpoint whether the write landed even if the
 * BLXNS later fails.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_ipcsar_post = 0U;

/**
 * @var g_cpu1_pingpong_ipc_tz_step
 * @brief Mirror of ``ra_tz_secure_boot_get_step`` for SWD readback.
 *
 * @details One-shot snapshot taken inside ``ra_trustzone_init`` for
 *          bench diagnostics. Mirrors the secure-boot library step
 *          enum (``ra_tz_secure_boot_step_t``).
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint8_t g_cpu1_pingpong_ipc_tz_step = 0U;

void ra_trustzone_init(void)
{
#ifdef RA_TRUSTZONE_ENABLE
  /* Pre 1: this function is called from SystemInit, BEFORE the .data
   * copy / .bss zero. We must not read or write any non-volatile
   * globals -- the diagnostic stamps below are volatile. */
  /* Pre 2: the boot ROM left SAU disabled and IDAU in reset state. */

  const uint32_t* ns_vt = (const uint32_t*)(uintptr_t)k_ns_vector_table_addr;

  const ra_err_t err = ra_tz_secure_boot_sau_init();
  if (err != k_ra_ok) {
    ra_log_error_val("CPU1IPC", "sau_init failed", (uint32_t)err);
    const ra_tz_secure_boot_step_t step_fail = ra_tz_secure_boot_get_step();
    g_cpu1_pingpong_ipc_tz_step              = (uint8_t)step_fail;
    return;
  }

  (void)ra_tz_secure_boot_security_init((uint32_t)k_ipcsar_value, (uint32_t)k_ipcpar_value);
  /* HUM Ch 3.2.1 "IPCSAR" p 205 -- read-back so a J-Link memprobe can
   * confirm the write landed even if BLXNS later wedges. */
  g_cpu1_pingpong_ipc_ipcsar_post        = *(volatile uint32_t*)0x40008610UL;
  const ra_tz_secure_boot_step_t step_ok = ra_tz_secure_boot_get_step();
  g_cpu1_pingpong_ipc_tz_step            = (uint8_t)step_ok;

  /* On hardware ra_tz_secure_boot_jump_ns does not return. On host
   * the BLXNS is captured into the secure-boot library's state for
   * the unit tests to inspect. */
  (void)ra_tz_secure_boot_jump_ns(ns_vt);

  /* Post 1: g_cpu1_pingpong_ipc_ipcsar_post == 0x00050000 (bench). */
  /* Post 2: ra_tz_secure_boot_get_step() == ..._branched (host) /
   *         control transferred to NS reset_handler (target). */
#else
  /* Without RA_TRUSTZONE_ENABLE this is a no-op. */
  g_cpu1_pingpong_ipc_tz_step     = 0U;
  g_cpu1_pingpong_ipc_ipcsar_post = 0U;
#endif
}
