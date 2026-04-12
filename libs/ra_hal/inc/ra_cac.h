/**
 * @file ra_cac.h
 * @brief Clock Accuracy Check driver
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @brief Programme CAC with upper + lower bounds on measured cycles.
 *
 * @param[in] upper Upper limit count.
 * @param[in] lower Lower limit count.
 * @return `k_ra_ok` on success.
 */
[[nodiscard]] ra_err_t ra_cac_init(uint16_t upper, uint16_t lower);

/**
 * @brief Kick off a CAC measurement and wait for completion.
 *
 * @param[out] out_count Counter value captured on completion.
 * @return `k_ra_ok` if the measurement completed inside the
 *         window, `k_ra_err_hw_timeout` if CAC did not finish.
 */
[[nodiscard]] ra_err_t ra_cac_measure(uint16_t* out_count);

#ifdef __cplusplus
}
#endif
