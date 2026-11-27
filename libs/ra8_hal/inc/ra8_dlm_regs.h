/**
 * @file ra8_dlm_regs.h
 * @brief Device Lifecycle Management (DLM) and CoreSight CPU Debug registers
 * @ingroup grp_ra8_hal
 *
 * @par Tag
 * [Ring 1 / HAL] {World: S}
 *
 * @details
 * Renesas RA8D2 hardware registers for Device Lifecycle Management (DLM)
 * and CPU CoreSight debug authentication control.
 *
 * Citations:
 * - Renesas RA8D2 Hardware User Manual (HUM) Rev 1.30:
 *   - Section 51.8.13 "DLMMON : Device Lifecycle Management State Monitor Register"
 *     (p. 3300)
 *   - Section 2.9.5 "DBGREG" and Section 2.9.5.2 "DBGSTR : Debug Status Register"
 *     (pp. 139-141)
 *   - Section 2.9.5.4 "DBGAUTH0 : Debug Authentication Control Register 0"
 *     (pp. 142-143)
 *   - Section 2.9.5.7 "ALCTRL : Authentication Level Control Register"
 *     (pp. 144-145)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_dlm_reg_addr_t
 * @brief Memory-mapped base addresses and register offsets for DLM and CPU_DBG.
 */
typedef enum : uintptr_t {
  k_ra8_pscu_base_addr         = 0x40204000UL, /**< PSCU secure base address (HUM 51.8.13). */
  k_ra8_pscu_ns_base_addr      = 0x50204000UL, /**< PSCU non-secure base address.           */
  k_ra8_dlmmon_addr            = 0x40204038UL, /**< DLMMON register address (offset 0x38).  */
  k_ra8_dlmmon_ns_addr         = 0x50204038UL, /**< DLMMON NS register address.             */

  k_ra8_cpu_dbg_base_addr      = 0x4001B000UL, /**< CPU_DBG secure base address (HUM 2.9.5). */
  k_ra8_cpu_dbg_ns_base_addr   = 0x5001B000UL, /**< CPU_DBG non-secure base address.         */
  k_ra8_cpu_dbg_dbgstr_addr    = 0x4001B000UL, /**< DBGSTR offset 0x000 (HUM 2.9.5.2).       */
  k_ra8_cpu_dbg_dbgstopcr_addr = 0x4001B010UL, /**< DBGSTOPCR offset 0x010 (HUM 2.9.5.3).    */
  k_ra8_cpu_dbg_dbgauth0_addr  = 0x4001B020UL, /**< DBGAUTH0 offset 0x020 (HUM 2.9.5.4).     */
  k_ra8_cpu_dbg_alctrl_addr    = 0x4001B100UL, /**< ALCTRL offset 0x100 (HUM 2.9.5.7).       */
} ra8_dlm_reg_addr_t;

/**
 * @enum ra8_dlmmon_bits_t
 * @brief Bitfield masks and values for DLMMON register (HUM 51.8.13).
 */
typedef enum : uint32_t {
  k_ra8_dlmmon_state_mask        = 0x0000000FUL, /**< DLMMON[3:0] lifecycle state mask. */
  k_ra8_dlmmon_state_oem_default = 0x00000004UL, /**< 0100b: OEM lifecycle state.       */
  k_ra8_dlmmon_state_rma_req     = 0x00000007UL, /**< RMA request state.                */
  k_ra8_dlmmon_state_rma_ack     = 0x00000008UL, /**< RMA acknowledge state.            */
  k_ra8_dlmmon_state_rma_ret     = 0x00000009UL, /**< RMA return state.                 */
} ra8_dlmmon_bits_t;

/**
 * @enum ra8_dbgauth0_bits_t
 * @brief Bitfield masks for DBGAUTH0 register (HUM 2.9.5.4).
 */
typedef enum : uint32_t {
  k_ra8_dbgauth0_dbgen0      = (1UL << 0),  /**< CPU0 invasive debug enable.           */
  k_ra8_dbgauth0_dbgen1      = (1UL << 1),  /**< CPU1 invasive debug enable.           */
  k_ra8_dbgauth0_niden0      = (1UL << 4),  /**< CPU0 non-invasive debug enable.       */
  k_ra8_dbgauth0_niden1      = (1UL << 5),  /**< CPU1 non-invasive debug enable.       */
  k_ra8_dbgauth0_deviceen    = (1UL << 16), /**< CoreSight APB-AP debug access enable. */
  k_ra8_dbgauth0_swdbg       = (1UL << 31), /**< Software control active flag (RO).    */
  k_ra8_dbgauth0_all_enables = (k_ra8_dbgauth0_dbgen0 | k_ra8_dbgauth0_dbgen1 |
                                k_ra8_dbgauth0_niden0 | k_ra8_dbgauth0_niden1 |
                                k_ra8_dbgauth0_deviceen),
} ra8_dbgauth0_bits_t;

/**
 * @enum ra8_dbgstr_bits_t
 * @brief Bitfield masks for DBGSTR register (HUM 2.9.5.2).
 */
typedef enum : uint32_t {
  k_ra8_dbgstr_cdbgpwrupreq = (1UL << 28), /**< OCD debug power-up request.     */
  k_ra8_dbgstr_cdbgpwrupack = (1UL << 29), /**< OCD debug power-up acknowledge. */
} ra8_dbgstr_bits_t;

/**
 * @enum ra8_alctrl_bits_t
 * @brief Bitfield masks and level constants for ALCTRL register (HUM 2.9.5.7).
 */
typedef enum : uint32_t {
  k_ra8_alctrl_al_mask       = 0x000000FFUL, /**< AL[7:0] authentication level mask. */
  k_ra8_alctrl_failcnt_mask  = 0xC0000000UL, /**< FAILCNT[1:0] at bits [31:30].      */
  k_ra8_alctrl_failcnt_shift = 30U,
  k_ra8_alctrl_al0           = 0x00000000UL, /**< AL0: Locked / unauthenticated. */
  k_ra8_alctrl_al1           = 0x00000005UL, /**< AL1: Authenticated level 1.    */
  k_ra8_alctrl_al2           = 0x000000A5UL, /**< AL2: Authenticated level 2.    */
} ra8_alctrl_bits_t;

#ifdef __cplusplus
}
#endif
