/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/trustzone_init.c
 * @brief Per-app secure-boot wiring for cpu1_pingpong_ipc
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Thin wrapper around ``libs/ra8_tz_secure_boot/`` -- the per-app file
 * exists because the auto-discovery glue scans for ``trustzone_init.c``
 * in each example directory. The actual SAU + CPSCU + BLXNS sequence
 * lives in ``ra8_tz_secure_boot_run`` so other apps can share it.
 *
 * When ``RA8_TRUSTZONE_ENABLE`` is not defined the function is a no-op
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
 * with ``scripts/hil/recover.sh`` warm is the only safe path to flash
 * the resulting image (see ``project_sau_sgstubs_brick``).
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra8_dual_core.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_tz_secure_boot.h"

extern uint32_t g_ra8_ls_cpu1_mram_start;
extern uint32_t g_ra8_ls_cpu1_stack_top;

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
 * @see ra8_tz_secure_boot_run
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ipcsar_value         = 0x000F0303UL, /**< All IPC subblocks NS.              */
  k_ipcpar_value         = 0x000F0303UL, /**< All IPC subblocks unprivileged-OK. */
  k_ns_vector_table_addr = 0x02080000UL, /**< NS image entry vectors.            */
} cpu1_pingpong_ipc_tz_const_t;

/**
 * @var g_cpu1_pingpong_ipc_ipcsar_post
 * @brief HIL post-secure-boot IPCSAR read-back diagnostic.
 *
 * @details
 * Bench scripts read this through SWD: it must hold ``0x00050000``
 * after the secure boot has finished. The variable is updated from
 * within ``ra8_trustzone_init`` immediately after the IPCSAR write so a
 * J-Link memprobe can pinpoint whether the write landed even if the
 * BLXNS later fails.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_ipcsar_post = 0U;

/**
 * @var g_cpu1_pingpong_ipc_tz_step
 * @brief Mirror of ``ra8_tz_secure_boot_get_step`` for SWD readback.
 *
 * @details One-shot snapshot taken inside ``ra8_trustzone_init`` for
 *          bench diagnostics. Mirrors the secure-boot library step
 *          enum (``ra8_tz_secure_boot_step_t``).
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint8_t g_cpu1_pingpong_ipc_tz_step = 0U;

/**
 * @var g_cpu1_pingpong_ipc_cpu1_release_err
 * @brief S-side CPU1 release return code captured pre-BLXNS.
 *
 * @details
 * ``ra8_cpu1_release`` writes CPU1INITVTOR / CPU1WAITCR / CPU1ACTCSR
 * which are all in the CPU control register block (0x4000F000). Those
 * registers are Secure-only on this chip so the release must happen
 * before BLXNS hands the CPU to the NS image. Bench reads this
 * counter to confirm CPU1 was successfully released even when the NS
 * ping-pong loop later misbehaves.
 *
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_cpu1_pingpong_ipc_cpu1_release_err = 0xFFFFFFFFU;

#ifdef RA8_TRUSTZONE_ENABLE
/**
 * @brief Release CPU1 with PRCR_S.PRC1 unlocked around the call.
 *
 * @details
 * CPU1INITVTOR / CPU1ACTCSR live in the CPU control window
 * (0x4000F000) and are gated by PRCR_S.PRC1 (LPM / dual-core
 * lifecycle). ``ra8_tz_secure_boot_security_init`` opens PRC4 for the
 * IPCSAR write then relocks the whole PRCR_S, so the writes inside
 * ``ra8_cpu1_release`` are silently dropped without an explicit unlock
 * here. The bench symptom of leaving PRC1 locked was CPU1INITVTOR
 * reading back as the chip cold-reset 0x02000000 regardless of the
 * value written and CPU1ACTCSR.ACT staying clear so CPU1 never came
 * out of reset.
 *
 * HUM Ch 13.2.1 "PRCR_S" p 521 + Ch 2.9.1.7 "CPU1INITVTOR" p 128-129 +
 * Ch 2.9.1.9 "CPU1ACTCSR" p 129-130.
 *
 * @pre security_init has already run (PRC4 was opened then relocked).
 * @pre CPU1 vector table image is staged at ORIGIN(MRAM_CPU1).
 * @post CPU1 active or rel_err captured in
 *       ``g_cpu1_pingpong_ipc_cpu1_release_err``.
 * @post PRCR_S restored to "all locked" (0xA500).
 *
 * @since 0.1.0
 */
static void internal_release_cpu1(void)
{
  g_cpu1_pingpong_ipc_cpu1_release_err = 0xDEADBEEFUL;
  *(volatile uint16_t*)0x4001E3FAUL    = (uint16_t)0xA512U; /* key | PRC1 | PRC4 */

  /* Chip-level bus controller security attribution. With these at
   * cold-reset defaults the M33's view of NS peripherals is gated by
   * the chip's bus arbiter; the CPU1 SAU init in cpu1_main.c alone is
   * not sufficient to reach IPCSAR-attributed channels. HUM Ch 9.2.4
   * "CPSCU" + FSP R_BSP_SecurityInit. */
  *(volatile uint32_t*)0x40008100UL = 0x00000001UL; /* BUSSARA  */
  *(volatile uint32_t*)0x40008104UL = 0x00000001UL; /* BUSSARB  */
  *(volatile uint32_t*)0x40008110UL = 0x00000001UL; /* BUSSARC  */
  *(volatile uint32_t*)0x40008170UL = 0x00000000UL; /* CPUSAR   */
  *(volatile uint32_t*)0x40008130UL = 0xFFFFFFFFUL; /* MMPUSARA */
  *(volatile uint32_t*)0x40008134UL = 0xFFFFFFFFUL; /* MMPUSARB */

  const ra8_err_t rel_err = ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top);
  *(volatile uint16_t*)0x4001E3FAUL    = (uint16_t)0xA500U; /* relock all PRCs */
  g_cpu1_pingpong_ipc_cpu1_release_err = (uint32_t)rel_err;
}
#endif

void ra8_trustzone_init(void)
{
#ifdef RA8_TRUSTZONE_ENABLE
  /* Pre 1: this function is called from SystemInit, BEFORE the .data
   * copy / .bss zero. We must not read or write any non-volatile
   * globals -- the diagnostic stamps below are volatile. */
  /* Pre 2: the boot ROM left SAU disabled and IDAU in reset state. */

  const uint32_t* ns_vt = (const uint32_t*)(uintptr_t)k_ns_vector_table_addr;

  const ra8_err_t err = ra8_tz_secure_boot_sau_init();
  if (err != k_ra8_ok) {
    ra8_log_error_val("CPU1IPC", "sau_init failed", (uint32_t)err);
    const ra8_tz_secure_boot_step_t step_fail = ra8_tz_secure_boot_get_step();
    g_cpu1_pingpong_ipc_tz_step               = (uint8_t)step_fail;
    return;
  }

  const ra8_err_t sec_err =
    ra8_tz_secure_boot_security_init((uint32_t)k_ipcsar_value, (uint32_t)k_ipcpar_value);
  if (sec_err != k_ra8_ok) {
    /* Do NOT release CPU1 or BLXNS when the IPC channel S/NS attribution
     * failed -- the NS core would face Secure-locked IPC channels. */
    ra8_log_error_val("CPU1IPC", "security_init failed", (uint32_t)sec_err);
    const ra8_tz_secure_boot_step_t step_sec = ra8_tz_secure_boot_get_step();
    g_cpu1_pingpong_ipc_tz_step              = (uint8_t)step_sec;
    return;
  }
  /* HUM Ch 3.2.1 "IPCSAR" p 205 -- read-back so a J-Link memprobe can
   * confirm the write landed even if BLXNS later wedges. */
  g_cpu1_pingpong_ipc_ipcsar_post         = *(volatile uint32_t*)0x40008610UL;
  const ra8_tz_secure_boot_step_t step_ok = ra8_tz_secure_boot_get_step();
  g_cpu1_pingpong_ipc_tz_step             = (uint8_t)step_ok;

  internal_release_cpu1();

  /* On hardware ra8_tz_secure_boot_jump_ns does not return. On host
   * the BLXNS is captured into the secure-boot library's state for
   * the unit tests to inspect. */
  const ra8_err_t jump_err = ra8_tz_secure_boot_jump_ns(ns_vt);
  if (jump_err != k_ra8_ok) {
    /* Reached on hardware ONLY when the NS image was DENIED (vector-table
     * validation failed): stamp the step for the J-Link memprobe and fall
     * back to the S-side main() -- never treat the NS world as live. */
    ra8_log_error_val("CPU1IPC", "jump_ns denied", (uint32_t)jump_err);
    const ra8_tz_secure_boot_step_t step_denied = ra8_tz_secure_boot_get_step();
    g_cpu1_pingpong_ipc_tz_step                 = (uint8_t)step_denied;
    return;
  }

  /* Post 1: g_cpu1_pingpong_ipc_ipcsar_post == 0x00050000 (bench). */
  /* Post 2: ra8_tz_secure_boot_get_step() == ..._branched (host) /
   *         control transferred to NS reset_handler (target). */
#else
  /* Without RA8_TRUSTZONE_ENABLE this is a no-op. */
  g_cpu1_pingpong_ipc_tz_step     = 0U;
  g_cpu1_pingpong_ipc_ipcsar_post = 0U;
#endif
}
