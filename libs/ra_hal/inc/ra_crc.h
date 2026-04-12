/**
 * @file ra_crc.h
 * @brief CRC hardware calculator driver header
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_crc_regs.h"
#include "ra_err.h"

/**
 * @brief Configure the CRC block for a given polynomial.
 * @param[in] poly One of the `k_ra_crc_poly_*` values.
 * @return `k_ra_ok` on success.
 */
[[nodiscard]] ra_err_t ra_crc_init(ra_crc_poly_t poly);

/**
 * @brief Compute a CRC over a byte buffer.
 *
 * @details
 * Feeds each byte of `data` into `CRCDIR` and reads the running
 * result from `CRCDOR` at the end. The CRC unit is not reset
 * between calls -- the caller must write `CRCCR1` to clear state
 * via `ra_crc_reset()` first.
 *
 * @param[in]  data      Pointer to bytes (not NULL).
 * @param[in]  len       Byte count.
 * @param[out] out_crc   Receives the final result.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t ra_crc_compute(const uint8_t* data, uint32_t len, uint32_t* out_crc);

/**
 * @brief Clear the running CRC state.
 */
void ra_crc_reset(void);

#ifdef __cplusplus
}
#endif
