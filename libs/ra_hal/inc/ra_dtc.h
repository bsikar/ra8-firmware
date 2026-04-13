/**
 * @file ra_dtc.h
 * @brief Data Transfer Controller (DTC) driver header
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
 * @brief Initialise the DTC and install its vector table base.
 *
 * @param[in] vector_base Pointer to a caller-supplied DTC vector
 *                        table (16-byte-aligned SRAM region).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dtc_init(void* vector_base);

/**
 * @brief Start the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dtc_enable(void);

/**
 * @brief Stop the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dtc_disable(void);

#ifdef __cplusplus
}
#endif
