/**
 * @file ra_icu.h
 * @brief Interrupt Control Unit driver
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra_err.h"

/**
 * @brief Route an ELC event to NVIC line `nvic_index` via IELSR.
 *
 * @param[in] nvic_index IRQ number (0..111 on RA8D2 primary core).
 * @param[in] event      ELC event number to route.
 * @return `k_ra_ok` on success.
 */
[[nodiscard]] ra_err_t ra_icu_route(uint16_t nvic_index, ra_elc_event_t event);

/**
 * @brief Enable a NVIC line (direct NVIC_ISER write).
 */
void ra_icu_nvic_enable(uint16_t nvic_index);

/**
 * @brief Disable a NVIC line.
 */
void ra_icu_nvic_disable(uint16_t nvic_index);

/**
 * @brief Set NVIC priority for a line (0..15, 0 = highest).
 */
void ra_icu_nvic_set_priority(uint16_t nvic_index, uint8_t priority);

#ifdef __cplusplus
}
#endif
