/**
 * @file ra_canfd.h
 * @brief CANFD Lite driver (framework)
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
 * @brief Take a CANFD channel out of reset into operation mode.
 */
[[nodiscard]] ra_err_t ra_canfd_init(uint8_t channel);

/**
 * @brief Put the CANFD channel back into reset.
 */
[[nodiscard]] ra_err_t ra_canfd_deinit(uint8_t channel);

#ifdef __cplusplus
}
#endif
