/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dual_core.h
 * @brief Dual-core (CPU0 / CPU1) lifecycle helper -- public API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The Renesas RA8D2 ships two Arm cores -- a Cortex-M85 primary (CPU0)
 * at 1 GHz and a Cortex-M33 secondary (CPU1) at 250 MHz. The secondary
 * core boots held in a power-gated reset/wait state. CPU0 firmware is
 * responsible for installing the CPU1 reset vector address and then
 * issuing an activation request that takes CPU1 out of reset.
 *
 * Register map (HUM Ch 2.9.1 "CPU control registers" p 128-130 and
 * Ch 2.6.1.1 "Boot CPU Selection" p 117):
 *
 *   - CPU_CTRL base       = 0x4000_F000 (Secure alias).
 *   - CPU1INITVTOR @ 0x044 -- CPU1 initial vector table base; the M33
 *     latches this into VTOR on the next reset-release edge. The first
 *     word of that vector table is the initial MSP (Armv8-M reset
 *     behaviour) so a separate stack-pointer register is NOT needed.
 *   - CPU1WAITCR   @ 0x054 -- bit 0 CPUWAIT: 0 = run on release,
 *     1 = stall after reset release. Reset value = 0.
 *   - CPU1ACTCSR   @ 0x064 -- 16-bit register, bit 0 ACTREQ (W,
 *     activation request), bit 7 ACT (R, active state). Writes require
 *     KEY=0xA5 in the upper byte.
 *
 * The earlier draft of this driver used Renesas-FSP-style names
 * (LPCSR / VTORC1 / MSPC1 at SYSC base 0x4001_E000); those registers
 * do not exist in the RA8D2 HUM and the hardware silently dropped
 * every write, so CPU1 never came out of reset. The cpu1_pingpong
 * HIL gate caught this when ``g_cpu1_pingpong_match`` stayed at zero
 * for the entire 5 s probe window.
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_dual_core_addr_t
 * @brief Base address of the CPU control register window used for
 *        secondary-core lifecycle management.
 *
 * @details The HUM lists both a Secure (0x4000_F000) and Non-Secure
 *          (0x5000_F000) alias for this window; this driver uses the
 *          Secure alias because CPU1 lifecycle is owned by Secure
 *          firmware on this project (CPUSAR.CPUSA1 defaults Secure).
 */
typedef enum : uintptr_t {
  k_ra8_dual_core_ctrl_base_addr = 0x4000F000UL, /**< CPU_CTRL Secure base. */
} ra8_dual_core_addr_t;

/**
 * @enum ra8_dual_core_off_t
 * @brief Offsets within the CPU_CTRL window for the CPU1 lifecycle
 *        registers (HUM Ch 2.9.1 p 128-130, section indices 2.9.1.7
 *        / 2.9.1.8 / 2.9.1.9).
 */
typedef enum : uint16_t {
  k_ra8_dual_core_off_cpu1_initvtor = 0x044U, /**< CPU1INITVTOR (32-bit). */
  k_ra8_dual_core_off_cpu1_waitcr   = 0x054U, /**< CPU1WAITCR   (8-bit).  */
  k_ra8_dual_core_off_cpu1_actcsr   = 0x064U, /**< CPU1ACTCSR   (16-bit). */
} ra8_dual_core_off_t;

/**
 * @enum ra8_dual_core_field_t
 * @brief Bit positions inside CPU1WAITCR / CPU1ACTCSR.
 *
 * @details
 * HUM Ch 2.9.1.8 p 129 -- CPU1WAITCR.CPUWAIT @ bit 0.
 * HUM Ch 2.9.1.9 p 129-130 -- CPU1ACTCSR.ACTREQ @ bit 0 (W), ACT @ bit 7 (R).
 * Writes to CPU1ACTCSR require KEY=0xA5 in bits 15:8.
 */
typedef enum : uint16_t {
  k_ra8_dual_core_waitcr_cpuwait_mask = (1U << 0), /**< CPU1WAITCR.CPUWAIT. */
  k_ra8_dual_core_actcsr_actreq_mask  = (1U << 0), /**< CPU1ACTCSR.ACTREQ.  */
  k_ra8_dual_core_actcsr_act_mask     = (1U << 7), /**< CPU1ACTCSR.ACT.     */
  k_ra8_dual_core_actcsr_key_shift    = 8U,        /**< Key byte position.  */
  k_ra8_dual_core_actcsr_key_value    = 0xA5U,     /**< Required key code.  */
} ra8_dual_core_field_t;

/**
 * @enum ra8_dual_core_const_t
 * @brief Tunable constants for the bounded poll loops.
 */
typedef enum : uint32_t {
  k_ra8_dual_core_release_poll_max = 1000000U, /**< Bounded poll iterations. */
} ra8_dual_core_const_t;

/**
 * @brief Release CPU1 (Cortex-M33) from reset and start it executing.
 * @details Writes the vector-table base into CPU1INITVTOR, clears
 *          CPU1WAITCR.CPUWAIT (so CPU1 starts immediately rather than
 *          stalling), then writes (KEY << 8 | ACTREQ) into CPU1ACTCSR
 *          and bounded-polls CPU1ACTCSR.ACT for the active transition.
 *          The Cortex-M33 fetches its initial MSP from the first word
 *          of the vector table at ``entry`` (Armv8-M reset behaviour),
 *          so ``sp`` is validated for alignment but does not drive a
 *          separate register write -- the application is responsible
 *          for placing the stack-top value at ``entry[0]``.
 * @param[in] entry CPU1 reset vector base (128-byte aligned). The
 *                  first word at this address must be the initial MSP.
 * @param[in] sp    CPU1 initial main stack pointer (8-byte aligned).
 *                  Validated only; the M33 reads SP from ``entry[0]``.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Core released and confirmed active.
 * @retval k_ra8_err_null_ptr      entry or sp was NULL.
 * @retval k_ra8_err_invalid_arg   entry or sp mis-aligned.
 * @retval k_ra8_err_not_supported Caller is not CPU0.
 * @retval k_ra8_err_timeout       ACTCSR.ACT did not assert within the
 *                                bounded poll budget.
 * @pre Caller is running on CPU0 (Cortex-M85).
 * @pre The vector table at ``entry`` contains a valid Reset_Handler
 *      at offset +4 and a valid initial MSP at offset 0.
 * @post On success, CPU1 is fetching from ``entry`` and ACT == 1.
 * @post On success, ra8_cpu1_is_running() returns true.
 * @note Not thread-safe.
 * @see ra8_cpu1_halt()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cpu1_release(void* entry, void* sp);

/**
 * @brief Park CPU1 by asserting CPU1WAITCR.CPUWAIT.
 * @details RA8D2 does not document a clean deactivation path once
 *          CPU1ACTCSR.ACT has gone to 1, so this driver stalls CPU1
 *          by writing CPUWAIT=1 instead. The ACT bit may still read 1
 *          (CPU1 remains powered/clocked) but no instructions retire.
 *          ``ra8_cpu1_is_running()`` will return false after this call.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                CPU1 stalled.
 * @retval k_ra8_err_not_supported Caller is not CPU0.
 * @pre Caller is running on CPU0.
 * @pre CPU1 was previously released via ra8_cpu1_release().
 * @post CPU1 is stalled (CPUWAIT=1).
 * @post ra8_cpu1_is_running() returns false.
 * @note Not thread-safe.
 * @see ra8_cpu1_release()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cpu1_halt(void);

/**
 * @brief Return whether CPU1 is currently fetching instructions.
 * @details A truth-table of (ACT, CPUWAIT) -> running:
 *          (0, x) -> false (not yet activated / power-gated)
 *          (1, 0) -> true  (out of reset, running)
 *          (1, 1) -> false (out of reset, stalled by CPUWAIT)
 * @return bool True if CPU1 is fetching, false otherwise.
 * @retval true  CPU1 is fetching instructions.
 * @retval false CPU1 is in the stopped or stalled state.
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post Module state unchanged.
 * @note Re-entrant.
 * @since 0.1.0
 */
bool ra8_cpu1_is_running(void);

#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
/**
 * @brief Host-test fault-seam key for the CPU1ACTCSR.ACT release poll.
 *
 * @details
 * Returns the address the ``ra8_cpu1_release()`` ACT poll reads on the host
 * build, so a unit test can arm ::ra8_fake_mmio_fail_wait / satisfy_after on the
 * exact register the loop consults and drive its retry / timeout legs. The real
 * secondary-core activation cannot be modelled by host RAM (nothing self-sets
 * ACT), so the seam owns the loop-exit decision.
 *
 * @return Opaque fault-table key (address of the host ACTCSR mirror).
 * @retval non-NULL Stable per-process key usable with the ra8_fake_mmio seam.
 *
 * @pre Built with ``RA8_OFF_TARGET`` and ``UNIT_TEST`` defined.
 * @pre Called from the single-threaded host test harness.
 * @post No module state is modified.
 * @post The returned key is stable for the lifetime of the process.
 *
 * @note Host-test only; not part of the target firmware API.
 * @since 0.1.0
 */
[[nodiscard]] volatile const void* ra8_dual_core_test_actcsr_key(void);
#endif

#ifdef __cplusplus
}
#endif
