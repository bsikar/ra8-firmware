/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_crc.h
 * @brief CRC hardware calculator driver header
 * @ingroup grp_hal_analog
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_crc_regs.h"
#include "ra8_err.h"

/**
 * @brief Configure the CRC block for a given polynomial.
 * @param[in] poly One of the `k_ra8_crc_poly_*` values.
 * @return `k_ra8_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_init(ra8_crc_poly_t poly);

/**
 * @brief Compute a CRC over a byte buffer.
 *
 * @details
 * Feeds each byte of `data` into `CRCDIR` and reads the running
 * result from `CRCDOR` at the end. The CRC unit is not reset
 * between calls -- the caller must write `CRCCR1` to clear state
 * via `ra8_crc_reset()` first.
 *
 * @param[in]  data      Pointer to bytes (not NULL).
 * @param[in]  len       Byte count.
 * @param[out] out_crc   Receives the final result.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc);

/**
 * @brief Clear the running CRC state.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
void ra8_crc_reset(void);

/**
 * @brief Tear down the CRC block (disable + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_deinit(void);

/**
 * @brief Change the CRC polynomial at runtime without a reset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_set_poly(ra8_crc_poly_t poly);

/**
 * @brief Read the current polynomial selection (CRCCR0.GPS).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_get_status(uint8_t* out_poly);

/**
 * @brief Put CRC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_crc_exit_stop(void);

#ifdef __cplusplus
}
#endif
