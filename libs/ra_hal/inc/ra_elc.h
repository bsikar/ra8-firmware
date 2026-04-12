/**
 * @file ra_elc.h
 * @brief Event Link Controller helper
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
 * @brief Enable or disable the ELC globally.
 */
[[nodiscard]] ra_err_t ra_elc_enable(bool enable);

/**
 * @brief Route an ELC event to an ELSR slot so the destination
 *        peripheral triggers on it.
 *
 * @param[in] elsr_index 0..22.
 * @param[in] event      `ra_elc_event_t` value.
 */
[[nodiscard]] ra_err_t ra_elc_link(uint8_t elsr_index, ra_elc_event_t event);

#ifdef __cplusplus
}
#endif
