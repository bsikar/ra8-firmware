/**
 * @file ra8d2_pdm_regs.h
 * @brief PDM (Pulse Density Modulation) microphone interface register layout
 *
 * @details
 * Single PDM-IF instance at 0x40256000. introduces a
 * minimal register window covering lifecycle, status, and
 * interrupt enable; full PCM decimation / FIR filter setup land
 * with the first audio driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_pdm_base_addr = 0x40256000UL,
} ra_pdm_addr_t;

/**
 * @struct r_pdm_regs_t
 * @brief Minimal PDM register window.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_pdm()`` in
 * ``libs/ra_hal/src/ra_pdm.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t PDM_CTRL; /**< +0x00 Control. */
  volatile uint32_t PDM_CFG;  /**< +0x04 Configuration. */
  volatile uint32_t PDM_STAT; /**< +0x08 Status. */
  volatile uint32_t PDM_IER;  /**< +0x0C Interrupt enbl. */
  volatile uint32_t PDM_DATA; /**< +0x10 PCM data out. */
} r_pdm_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the PDM-IF block. */
static inline volatile r_pdm_regs_t* ra_pdm(void)
{
  return (volatile r_pdm_regs_t*)k_ra_pdm_base_addr;
}

#ifdef __cplusplus
}
#endif
