/**
 * @file ra8_dlm.h
 * @brief Device Lifecycle Management (DLM) and Debug Port Lockdown API
 * @ingroup grp_ra8_hal
 *
 * @par Tag
 * [Ring 1 / HAL] {World: S}
 *
 * @details
 * Production-only debug-port and DLM lifecycle management API.
 * This module provides routines to inspect the current DLM and debug
 * authentication status, close debug ports (DBGAUTH0 lockdown), and
 * progress lifecycle state into strictly recoverable production modes.
 *
 * Safety policy:
 * - Strictly opt-in at compile time: `#ifdef RA8_PRODUCTION_LOCKDOWN_OPT_IN`.
 *   When this macro is not defined (default for development and bench builds),
 *   all mutating routines fail-closed and return `k_ra8_err_not_supported`.
 * - Strictly honors anti-recovery policy: irreversible bricking actions
 *   (such as setting the Disable-Initialize security flag or terminal
 *   locked-boot states) are strictly rejected with `k_ra8_err_invalid_arg`
 *   and never executed.
 * - Hardware abstraction seam (Dependency Injection) allows complete host
 *   unit testing without touching physical silicon or OTP memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ra8_dlm_regs.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_dlm_lifecycle_t
 * @brief Valid recoverable lifecycle states (HUM 51.4 and Table 51.6).
 *
 * Only recoverable OEM lifecycle states are permitted.
 * Irreversible terminal states are deliberately excluded from this enum.
 */
typedef enum : uint8_t {
  k_ra8_dlm_lifecycle_unknown     = 0x00U, /**< Unknown or invalid lifecycle.             */
  k_ra8_dlm_lifecycle_oem_open    = 0x01U, /**< OEM development / open debug (PL2).       */
  k_ra8_dlm_lifecycle_oem_auth    = 0x02U, /**< OEM authenticated debug (PL1).            */
  k_ra8_dlm_lifecycle_oem_locked  = 0x03U, /**< OEM production locked (PL0, recoverable). */
  k_ra8_dlm_lifecycle_oem_default = 0x04U, /**< Factory blank / default OEM state (SSD).  */
} ra8_dlm_lifecycle_t;

/**
 * @enum ra8_dlm_al_t
 * @brief Authentication levels reported by ALCTRL (HUM 2.9.5.7).
 */
typedef enum : uint8_t {
  k_ra8_dlm_al0_locked = 0x00U, /**< AL0: Locked / unauthenticated. */
  k_ra8_dlm_al1_auth   = 0x05U, /**< AL1: Authenticated level 1.    */
  k_ra8_dlm_al2_auth   = 0xA5U, /**< AL2: Authenticated level 2.    */
  k_ra8_dlm_al_other   = 0xFFU, /**< Unrecognized AL value.         */
} ra8_dlm_al_t;

/**
 * @struct ra8_dlm_status_t
 * @brief Complete snapshot of DLM and debug authentication hardware state.
 */
typedef struct {
  ra8_dlm_lifecycle_t lifecycle;         /**< Decoded lifecycle state.                */
  uint32_t            raw_dlmmon;        /**< Raw 32-bit DLMMON register value.       */
  uint32_t            raw_dbgauth0;      /**< Raw 32-bit DBGAUTH0 register value.     */
  uint32_t            raw_dbgstr;        /**< Raw 32-bit DBGSTR register value.       */
  uint32_t            raw_alctrl;        /**< Raw 32-bit ALCTRL register value.       */
  ra8_dlm_al_t        auth_level;        /**< Decoded authentication level.           */
  uint8_t             fail_count;        /**< Failed auth count (0..3).               */
  bool                debugger_attached; /**< True if CDBGPWRUPREQ is asserted.       */
  bool                debug_ports_open;  /**< True if any DBGAUTH0 enable bit is set. */
} ra8_dlm_status_t;

/**
 * @struct ra8_dlm_hw_t
 * @brief Dependency injection seam for MMIO register access.
 */
typedef struct ra8_dlm_hw {
  uint32_t (*read32)(void* ctx, uintptr_t addr);
  void (*write32)(void* ctx, uintptr_t addr, uint32_t val);
  void* ctx;
} ra8_dlm_hw_t;

/**
 * @brief Check if compile-time opt-in lockdown support is compiled into this build.
 *
 * @return true if RA8_PRODUCTION_LOCKDOWN_OPT_IN is defined, false otherwise.
 */
bool ra8_dlm_is_opt_in_enabled(void);

/**
 * @brief Get default hardware access structure for real target MMIO.
 *
 * @return Const pointer to hardware access interface using volatile MMIO.
 */
const ra8_dlm_hw_t* ra8_dlm_default_hw(void);

/**
 * @brief Query current DLM and debug-port status via injectable hardware seam.
 *
 * @param[in]  hw         Hardware access interface (must not be NULL).
 * @param[out] out_status Pointer to status structure to receive snapshot (must not be NULL).
 *
 * @retval k_ra8_ok           Status successfully read and decoded.
 * @retval k_ra8_err_null_ptr Either @p hw, @p hw->read32, or @p out_status is NULL.
 */
ra8_err_t ra8_dlm_get_status_hw(const ra8_dlm_hw_t* hw, ra8_dlm_status_t* out_status);

/**
 * @brief Query current DLM and debug-port status using target MMIO.
 *
 * @param[out] out_status Pointer to status structure to receive snapshot (must not be NULL).
 *
 * @retval k_ra8_ok           Status successfully read and decoded.
 * @retval k_ra8_err_null_ptr @p out_status is NULL.
 */
ra8_err_t ra8_dlm_get_status(ra8_dlm_status_t* out_status);

/**
 * @brief Lock down debug ports by clearing DBGAUTH0 enables via injectable hardware seam.
 *
 * When software debug control is enabled (OFS1.SWDBG = 0), clearing DBGAUTH0
 * disables CPU0/CPU1 invasive debug, non-invasive trace, and CoreSight APB-AP access.
 *
 * Preconditions:
 * - `ra8_dlm_is_opt_in_enabled()` must be true (RA8_PRODUCTION_LOCKDOWN_OPT_IN).
 * - @p hw must not be NULL.
 *
 * Postconditions:
 * - DBGAUTH0 is cleared to 0 (all enable bits disabled).
 *
 * @param[in] hw Hardware access interface (must not be NULL).
 *
 * @retval k_ra8_ok                Debug ports successfully locked down.
 * @retval k_ra8_err_null_ptr      @p hw, @p hw->write32, or @p hw->read32 is NULL.
 * @retval k_ra8_err_not_supported Lockdown opt-in is not compiled in.
 * @retval k_ra8_fail              Hardware register readback did not verify zero.
 */
ra8_err_t ra8_dlm_lockdown_debug_ports_hw(const ra8_dlm_hw_t* hw);

/**
 * @brief Lock down debug ports using target MMIO.
 *
 * @retval k_ra8_ok                Debug ports successfully locked down.
 * @retval k_ra8_err_not_supported Lockdown opt-in is not compiled in.
 * @retval k_ra8_fail              Hardware register readback did not verify zero.
 */
ra8_err_t ra8_dlm_lockdown_debug_ports(void);

/**
 * @brief Transition DLM to a recoverable production lifecycle state via injectable hardware seam.
 *
 * Strictly enforces that @p target_state is a recoverable state (k_ra8_dlm_lifecycle_oem_auth
 * or k_ra8_dlm_lifecycle_oem_locked). Any attempt to transition to an unrecoverable state
 * is rejected with k_ra8_err_invalid_arg.
 *
 * @param[in] hw           Hardware access interface (must not be NULL).
 * @param[in] target_state Desired recoverable lifecycle state.
 *
 * @retval k_ra8_ok                Transition validated and initiated.
 * @retval k_ra8_err_null_ptr      @p hw is NULL.
 * @retval k_ra8_err_not_supported Lockdown opt-in is not compiled in.
 * @retval k_ra8_err_invalid_arg   @p target_state is invalid or unrecoverable.
 */
ra8_err_t ra8_dlm_transition_recoverable_hw(const ra8_dlm_hw_t* hw,
                                            ra8_dlm_lifecycle_t target_state);

/**
 * @brief Transition DLM to a recoverable production lifecycle state using target MMIO.
 *
 * @param[in] target_state Desired recoverable lifecycle state.
 *
 * @retval k_ra8_ok                Transition validated and initiated.
 * @retval k_ra8_err_not_supported Lockdown opt-in is not compiled in.
 * @retval k_ra8_err_invalid_arg   @p target_state is invalid or unrecoverable.
 */
ra8_err_t ra8_dlm_transition_recoverable(ra8_dlm_lifecycle_t target_state);

/**
 * @brief Check if an active on-chip debugger is currently connected.
 *
 * Inspects DBGSTR.CDBGPWRUPREQ to detect active debugger connection.
 *
 * @param[in]  hw           Hardware access interface (must not be NULL).
 * @param[out] out_attached Set to true if debugger is attached, false otherwise.
 *
 * @retval k_ra8_ok           Check succeeded.
 * @retval k_ra8_err_null_ptr Either @p hw, @p hw->read32, or @p out_attached is NULL.
 */
ra8_err_t ra8_dlm_is_debugger_attached_hw(const ra8_dlm_hw_t* hw, bool* out_attached);

/**
 * @brief Check if an active on-chip debugger is currently connected using target MMIO.
 *
 * @param[out] out_attached Set to true if debugger is attached, false otherwise.
 *
 * @retval k_ra8_ok           Check succeeded.
 * @retval k_ra8_err_null_ptr @p out_attached is NULL.
 */
ra8_err_t ra8_dlm_is_debugger_attached(bool* out_attached);

#ifdef __cplusplus
}
#endif
