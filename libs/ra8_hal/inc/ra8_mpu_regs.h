/**
 * @file ra8_mpu_regs.h
 * @brief Memory Protection Unit register layout for the Renesas RA8D2
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
  k_ra8_mpu_core_base_addr  = 0xE000ED90UL,
  k_ra8_mpu_mmpu_base_addr  = 0x40000000UL,
  k_ra8_mpu_spmon_base_addr = 0x40000D00UL,
} ra8_mpu_addr_t;

/**
 * @struct r_mpu_regs_t
 * @brief Architectural Cortex-M85 MPU register block.
 */
typedef struct {
  volatile uint32_t TYPE;
  volatile uint32_t CTRL;
  volatile uint32_t RNR;
  volatile uint32_t RBAR;
  volatile uint32_t RLAR;
  volatile uint32_t RBAR_A1;
  volatile uint32_t RLAR_A1;
  volatile uint32_t RBAR_A2;
  volatile uint32_t RLAR_A2;
  volatile uint32_t RBAR_A3;
  volatile uint32_t RLAR_A3;
  volatile uint32_t reserved0;
  volatile uint32_t MAIR0;
  volatile uint32_t MAIR1;
} r_mpu_regs_t;

typedef enum : uint32_t {
  k_ra8_mpu_regs_block_size = 0x38UL,
} ra8_mpu_layout_t;

static_assert(sizeof(r_mpu_regs_t) == (size_t)k_ra8_mpu_regs_block_size,
              "r_mpu_regs_t size must match k_ra8_mpu_regs_block_size");

typedef enum : uint32_t {
  k_ra8_mpu_ctrl_enable     = (1UL << 0U),
  k_ra8_mpu_ctrl_hfnmiena   = (1UL << 1U),
  k_ra8_mpu_ctrl_privdefena = (1UL << 2U),
} ra8_mpu_ctrl_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_type_dregion_shift = 8UL,
  k_ra8_mpu_type_dregion_mask  = 0x0000FF00UL,
} ra8_mpu_type_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_rbar_xn_mask   = 0x00000001UL,
  k_ra8_mpu_rbar_ap_shift  = 1UL,
  k_ra8_mpu_rbar_ap_mask   = 0x00000006UL,
  k_ra8_mpu_rbar_sh_shift  = 3UL,
  k_ra8_mpu_rbar_sh_mask   = 0x00000018UL,
  k_ra8_mpu_rbar_base_mask = 0xFFFFFFE0UL,
} ra8_mpu_rbar_bits_t;

typedef enum : uint32_t {
  k_ra8_mpu_rlar_en_mask       = 0x00000001UL,
  k_ra8_mpu_rlar_attridx_shift = 1UL,
  k_ra8_mpu_rlar_attridx_mask  = 0x0000000EUL,
  k_ra8_mpu_rlar_limit_mask    = 0xFFFFFFE0UL,
} ra8_mpu_rlar_bits_t;

/**
 * @brief Accessor for the architectural Cortex-M85 MPU register block.
 * @return Pointer to the live MPU registers.
 * @pre Called from privileged mode (or RA8_SIMULATOR_MODE).
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
