/**
 * @file ra8_poeg_regs.h
 * @brief Port Output Enable for GPT (POEG) register layout for the RA8D2
 * @ingroup grp_hal_timers
 *
 * @details
 * Four POEG groups (POEG0..POEG3) at `0x40212000` with `0x100`
 * stride. Each group monitors one external emergency-stop input
 * and, on trigger, forces the associated GPT outputs into a safe
 * (high-impedance) state without CPU involvement. Essential for
 * safety-critical motor control so an over-current fault can halt
 * the H-bridges in a single cycle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

typedef enum : uintptr_t {
  k_ra8_poeg0_base_addr = 0x40212000UL, /**< RA8 poeg0 base address. */
  k_ra8_poeg1_base_addr = 0x40212100UL, /**< RA8 poeg1 base address. */
  k_ra8_poeg2_base_addr = 0x40212200UL, /**< RA8 poeg2 base address. */
  k_ra8_poeg3_base_addr = 0x40212300UL, /**< RA8 poeg3 base address. */
} ra8_poeg_addr_t;

typedef enum : uint16_t {
  k_ra8_poeg_group_count  = 4U,     /**< RA8 poeg group count.  */
  k_ra8_poeg_group_stride = 0x100U, /**< RA8 poeg group stride. */
} ra8_poeg_limits_t;

typedef struct {
  volatile uint32_t POEGG; /**< +0x00 POEG Group setting + status. */
} r_poeg_regs_t;

/** @brief Get pointer to POEG group N (0..3). */
RA8_HW_REGISTER_ACCESS
static inline volatile r_poeg_regs_t* ra8_poeg(uint8_t group)
{
  if ((uint16_t)group >= k_ra8_poeg_group_count) {
    return nullptr;
  }
  return (volatile r_poeg_regs_t*)(k_ra8_poeg0_base_addr +
                                   ((uintptr_t)group * (uintptr_t)k_ra8_poeg_group_stride));
}

#ifdef __cplusplus
}
#endif
