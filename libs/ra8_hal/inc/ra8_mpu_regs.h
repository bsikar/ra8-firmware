/**
 * @file ra8_mpu_regs.h
 * @brief Memory Protection Unit register layout for the Renesas RA8D2
 *
 * @details Defines typed Cortex-M85 MPU and RA8 memory-protection register layouts plus their fixed MMIO addresses.
 * @ingroup grp_hal_system
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef enum : uintptr_t {
  k_ra8_mpu_core_base_addr  = 0xE000ED90UL, /**< RA8 MPU core base address.  */
  k_ra8_mpu_mmpu_base_addr  = 0x40000000UL, /**< RA8 MPU mmpu base address.  */
  k_ra8_mpu_spmon_base_addr = 0x40000D00UL, /**< RA8 MPU spmon base address. */
} ra8_mpu_addr_t;

/**
 * @struct r_mpu_regs_t
 * @brief Architectural Cortex-M85 MPU register block.
 */
typedef struct {
  volatile uint32_t TYPE;      /**< TYPE register.    */
  volatile uint32_t CTRL;      /**< CTRL register.    */
  volatile uint32_t RNR;       /**< RNR register.     */
  volatile uint32_t RBAR;      /**< RBAR register.    */
  volatile uint32_t RLAR;      /**< RLAR register.    */
  volatile uint32_t RBAR_A1;   /**< RBAR_A1 register. */
  volatile uint32_t RLAR_A1;   /**< RLAR_A1 register. */
  volatile uint32_t RBAR_A2;   /**< RBAR_A2 register. */
  volatile uint32_t RLAR_A2;   /**< RLAR_A2 register. */
  volatile uint32_t RBAR_A3;   /**< RBAR_A3 register. */
  volatile uint32_t RLAR_A3;   /**< RLAR_A3 register. */
  volatile uint32_t reserved0; /**< Reserved.         */
  volatile uint32_t MAIR0;     /**< MAIR0 register.   */
  volatile uint32_t MAIR1;     /**< MAIR1 register.   */
} r_mpu_regs_t;

typedef enum : uint32_t {
  k_ra8_mpu_regs_block_size = 0x38UL, /**< RA8 MPU registers block size. */
} ra8_mpu_layout_t;

static_assert(sizeof(r_mpu_regs_t) == (size_t)k_ra8_mpu_regs_block_size,
              "r_mpu_regs_t size must match k_ra8_mpu_regs_block_size");

typedef enum : uint32_t {
  k_ra8_mpu_ctrl_enable     = (1UL << 0U), /**< RA8 MPU control enable.     */
  k_ra8_mpu_ctrl_hfnmiena   = (1UL << 1U), /**< RA8 MPU control hfnmiena.   */
  k_ra8_mpu_ctrl_privdefena = (1UL << 2U), /**< RA8 MPU control privdefena. */
} ra8_mpu_ctrl_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_type_dregion_shift = 8UL,          /**< RA8 MPU type dregion shift. */
  k_ra8_mpu_type_dregion_mask  = 0x0000FF00UL, /**< RA8 MPU type dregion mask.  */
} ra8_mpu_type_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_rbar_xn_mask   = 0x00000001UL, /**< RA8 MPU rbar xn mask.   */
  k_ra8_mpu_rbar_ap_shift  = 1UL,          /**< RA8 MPU rbar ap shift.  */
  k_ra8_mpu_rbar_ap_mask   = 0x00000006UL, /**< RA8 MPU rbar ap mask.   */
  k_ra8_mpu_rbar_sh_shift  = 3UL,          /**< RA8 MPU rbar sh shift.  */
  k_ra8_mpu_rbar_sh_mask   = 0x00000018UL, /**< RA8 MPU rbar sh mask.   */
  k_ra8_mpu_rbar_base_mask = 0xFFFFFFE0UL, /**< RA8 MPU rbar base mask. */
} ra8_mpu_rbar_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_rlar_en_mask       = 0x00000001UL, /**< RA8 MPU rlar en mask.       */
  k_ra8_mpu_rlar_attridx_shift = 1UL,          /**< RA8 MPU rlar attridx shift. */
  k_ra8_mpu_rlar_attridx_mask  = 0x0000000EUL, /**< RA8 MPU rlar attridx mask.  */
  k_ra8_mpu_rlar_limit_mask    = 0xFFFFFFE0UL, /**< RA8 MPU rlar limit mask.    */
} ra8_mpu_rlar_bits_t;

/**
 * @brief Accessor for the architectural Cortex-M85 MPU register block.
 * @return Pointer to the live MPU registers.
 * @pre Called from privileged mode (or RA8_OFF_TARGET).
 * @post Returned pointer is non-null and word-aligned.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile r_mpu_regs_t* ra8_mpu_regs(void)
{
  return (volatile r_mpu_regs_t*)k_ra8_mpu_core_base_addr;
}

#ifdef __cplusplus
}
#endif
