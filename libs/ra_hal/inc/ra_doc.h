/**
 * @file ra_doc.h
 * @brief Data Operation Circuit (DOC) driver header
 *
 * @details
 * Public API for the 16-bit Data Operation Circuit that performs
 * hardware add / subtract / compare operations. Useful for cheap
 * running checksums and threshold compares without burning CPU
 * cycles.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_doc_regs.h"
#include "ra_err.h"

/**
 * @brief Reset the DOC control register to its reset state.
 *
 * @details
 * Clears `DOCR`, `DODIR`, and `DODSR0`. Leaves the block disabled
 * until the first operation.
 *
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_doc_init(void);

/**
 * @brief Perform a 16-bit hardware add.
 *
 * @param[in]  a       First operand.
 * @param[in]  b       Second operand.
 * @param[out] out_sum Receives `a + b` (wraps mod 2^16).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_doc_add16(uint16_t a, uint16_t b, uint16_t* out_sum);

/**
 * @brief Perform a 16-bit hardware subtract.
 *
 * @param[in]  a        Minuend.
 * @param[in]  b        Subtrahend.
 * @param[out] out_diff Receives `a - b` (wraps mod 2^16).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_doc_sub16(uint16_t a, uint16_t b, uint16_t* out_diff);

#ifdef __cplusplus
}
#endif
