/**
 * @file ra8_sau_regs.h
 * @brief Security Attribution Unit register layout for the Cortex-M85
 *
 * @details Defines the typed Armv8-M SAU register block, its fixed MMIO address,
 * and the CTRL / TYPE / RLAR bit fields the driver encodes.
 * @ingroup grp_hal_system
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef enum : uintptr_t {
  k_ra8_sau_base_addr = 0xE000EDD0UL, /**< RA8 SAU register block base address. */
} ra8_sau_addr_t;

/**
 * @struct r_sau_regs_t
 * @brief Architectural Armv8-M Security Attribution Unit register block.
 *
 * @details
 * Laid out from the Armv8-M ARM "SAU registers" summary, secure alias of the
 * System Control Space. SFSR / SFAR follow RLAR in the address map but belong
 * to the SecureFault reporting path, not to attribution, so they are not part
 * of this block.
 */
typedef struct {
  volatile uint32_t CTRL; /**< CTRL register. */
  volatile uint32_t TYPE; /**< TYPE register. */
  volatile uint32_t RNR;  /**< RNR register.  */
  volatile uint32_t RBAR; /**< RBAR register. */
  volatile uint32_t RLAR; /**< RLAR register. */
} r_sau_regs_t;

typedef enum : uint32_t {
  k_ra8_sau_regs_block_size = 0x14UL, /**< RA8 SAU registers block size. */
} ra8_sau_layout_t;

static_assert(sizeof(r_sau_regs_t) == (size_t)k_ra8_sau_regs_block_size,
              "r_sau_regs_t size must match k_ra8_sau_regs_block_size");

typedef enum : uint32_t {
  k_ra8_sau_ctrl_enable = (1UL << 0U), /**< CTRL.ENABLE: SAU active.           */
  k_ra8_sau_ctrl_allns  = (1UL << 1U), /**< CTRL.ALLNS: unmapped memory is NS. */
} ra8_sau_ctrl_bits_t;

typedef enum : uint32_t {
  k_ra8_sau_type_sregion_mask = 0x000000FFUL, /**< TYPE.SREGION implemented count. */
} ra8_sau_type_bits_t;

typedef enum : uint32_t {
  k_ra8_sau_rbar_base_mask = 0xFFFFFFE0UL, /**< RBAR.BADDR, 32-byte aligned. */
} ra8_sau_rbar_bits_t;

typedef enum : uint32_t {
  k_ra8_sau_rlar_enable = (1UL << 0U), /**< RLAR.ENABLE: region active. */
  k_ra8_sau_rlar_nsc    = (1UL << 1U), /**< RLAR.NSC: NS-Callable.      */
} ra8_sau_rlar_bits_t;

typedef enum : uint32_t {
  k_ra8_sau_rlar_limit_mask = 0xFFFFFFE0UL, /**< RLAR.LADDR, 32-byte aligned. */
} ra8_sau_rlar_field_t;

/**
 * @brief Accessor for the architectural Armv8-M SAU register block.
 * @return Pointer to the live SAU registers.
 * @pre Called from secure privileged mode (or RA8_OFF_TARGET).
 * @post Returned pointer is non-null and word-aligned.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile r_sau_regs_t* ra8_sau_regs(void)
{
  return (volatile r_sau_regs_t*)k_ra8_sau_base_addr;
}

#ifdef __cplusplus
}
#endif
