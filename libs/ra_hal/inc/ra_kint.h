/**
 * @file ra_kint.h
 * @brief Key-matrix interrupt (KINT) driver
 *
 * @details
 * Mirrors FSP `r_kint_api_t` lifecycle (open / close / read_matrix)
 * for the on-chip key-matrix interrupt block. Programmes KRCTL for
 * column scan + edge selection and reads KRSTR to deliver the live
 * row state to the application. Per-row IRQ masking lives in KRM.
 *
 * Reference: FSP `r_kint` driver shape. Layout in `ra8d2_kint_regs.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_kint_regs.h"
#include "ra_err.h"

/**
 * @enum ra_kint_edge_t
 * @brief KRCTL.KREG edge selector.
 */
typedef enum : uint8_t {
  k_ra_kint_edge_falling = 0U, /**< Trigger on falling edge. */
  k_ra_kint_edge_rising  = 1U, /**< Trigger on rising edge.  */
  k_ra_kint_edge_max     = 2U, /**< Range guard.             */
} ra_kint_edge_t;

/**
 * @struct ra_kint_cfg_t
 * @brief Configuration descriptor for `ra_kint_open`.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_kint_edge_t edge;     /**< Edge select.                          */
  uint8_t        row_mask; /**< Per-row IRQ enable mask (one bit/row).*/
} ra_kint_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open the KINT driver.
 *
 * @details
 * Programmes KRCTL with the requested edge selector, copies
 * `cfg->row_mask` into KRM, clears KRF, and asserts KRCTL.KRE so the
 * column-scan engine is live.
 *
 * @param[in] cfg Configuration. Must not be NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Driver ready.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `cfg->edge` out of range.
 *
 * @pre Single-threaded init context.
 *
 * @post `KRCTL.KRE = 1`, `KRM = cfg->row_mask`, `KRF = 0`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kint_open(const ra_kint_cfg_t* cfg);

/**
 * @brief Close the KINT driver.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Released.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post KRCTL = 0, KRM = 0, KRF = 0.
 * @post Subsequent `_read_matrix` calls return `k_ra_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kint_close(void);

/**
 * @brief Read the live key-matrix row state.
 *
 * @details
 * Snapshots KRSTR. Each bit represents one row of the configured
 * key matrix; a 1 means the row is currently pressed (active-high
 * after edge / mask are factored in by the column-scan engine).
 *
 * @param[out] state Receives the 8-bit row snapshot.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Snapshot copied.
 * @retval k_ra_err_null_ptr `state` was NULL.
 * @retval k_ra_err_invalid_state Driver was never opened.
 *
 * @pre `ra_kint_open` succeeded.
 *
 * @post `*state` reflects KRSTR at call time.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_kint_read_matrix(uint8_t* state);

#ifdef __cplusplus
}
#endif
