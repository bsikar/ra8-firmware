/**
 * @file ra_dual_core.h
 * @brief Dual-core (CPU0 / CPU1) lifecycle helper -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The Renesas RA8D2 ships two Arm cores -- a Cortex-M85 primary (CPU0)
 * at 1 GHz and a Cortex-M33 secondary (CPU1) at 250 MHz. The secondary
 * core boots held-in-reset; CPU0 firmware is responsible for installing
 * the CPU1 reset vector + initial stack pointer and then releasing CPU1
 * from its standby state. See HUM Ch 11.4 "Multi-core control".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_dual_core_addr_t
 * @brief SYSC base address used for the multi-core control registers.
 */
typedef enum : uintptr_t {
  k_ra_dual_core_sysc_base_addr = 0x4001E000UL, /**< SYSC base. */
} ra_dual_core_addr_t;

/**
 * @enum ra_dual_core_off_t
 * @brief Offsets into the SYSC block for the multi-core registers.
 * @details Names mirror the Renesas FSP RA8M1/D1 source for the same block.
 */
typedef enum : uint16_t {
  k_ra_dual_core_off_lpcsr  = 0x0490U, /**< LPCSR  (CPU1 stop / start). */
  k_ra_dual_core_off_vtorc1 = 0x0494U, /**< VTORC1 (CPU1 initial VTOR). */
  k_ra_dual_core_off_mspc1  = 0x0498U, /**< MSPC1  (CPU1 initial MSP).  */
} ra_dual_core_off_t;

/**
 * @enum ra_dual_core_lpcsr_field_t
 * @brief Bitfield positions inside LPCSR used by this driver.
 */
typedef enum : uint32_t {
  k_ra_dual_core_lpcsr_stpch1_mask = (1U << 1),  /**< LPCSR.LPCSTPCH1. */
  k_ra_dual_core_lpcsr_stat_mask   = (1U << 17), /**< LPCSR.LPCSTPCH1_STAT. */
} ra_dual_core_lpcsr_field_t;

/**
 * @enum ra_dual_core_const_t
 * @brief Tunable constants for the bounded poll loops.
 */
typedef enum : uint32_t {
  k_ra_dual_core_release_poll_max = 1000000U, /**< Bounded poll iterations. */
} ra_dual_core_const_t;

/**
 * @brief Release CPU1 (Cortex-M33) from reset and start it executing.
 * @details Writes entry into VTORC1 and sp into MSPC1, clears
 *          LPCSR.LPCSTPCH1, polls STAT for clear with a NASA Rule-2
 *          bounded loop. See ra_dual_core.c for the full algorithm.
 * @param[in] entry CPU1 reset vector base (128-byte aligned).
 * @param[in] sp    CPU1 initial main stack pointer (8-byte aligned).
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Core released and confirmed running.
 * @retval k_ra_err_null_ptr      entry or sp was NULL.
 * @retval k_ra_err_invalid_arg   entry or sp mis-aligned.
 * @retval k_ra_err_not_supported Caller is not CPU0.
 * @retval k_ra_err_timeout       LPCSTPCH1_STAT did not clear in time.
 * @pre Caller is running on CPU0 (Cortex-M85).
 * @pre The vector table at entry contains a valid Reset_Handler.
 * @post On success, CPU1 is fetching from entry.
 * @post On success, ra_cpu1_is_running() returns true.
 * @note Not thread-safe.
 * @see ra_cpu1_halt()
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cpu1_release(void* entry, void* sp);

/**
 * @brief Put CPU1 back into the LPCSTPCH1=1 stopped state.
 * @details Sets LPCSR.LPCSTPCH1 = 1, polls STAT for assert with a
 *          NASA Rule-2 bounded loop.
 * @return ra_err_t Error code.
 * @retval k_ra_ok                CPU1 stopped.
 * @retval k_ra_err_not_supported Caller is not CPU0.
 * @retval k_ra_err_timeout       STAT did not assert within the bound.
 * @pre Caller is running on CPU0.
 * @pre CPU1 was previously released via ra_cpu1_release().
 * @post CPU1 is stopped.
 * @post ra_cpu1_is_running() returns false.
 * @note Not thread-safe.
 * @see ra_cpu1_release()
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cpu1_halt(void);

/**
 * @brief Return whether CPU1 is currently running.
 * @details Reads LPCSR.LPCSTPCH1_STAT.
 * @return bool True if CPU1 is fetching, false otherwise.
 * @retval true  CPU1 is fetching instructions.
 * @retval false CPU1 is in the stopped state.
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post Module state unchanged.
 * @note Re-entrant.
 * @since 0.1.0
 */
bool ra_cpu1_is_running(void);

#ifdef __cplusplus
}
#endif
