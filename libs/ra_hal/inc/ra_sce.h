/**
 * @file ra_sce.h
 * @brief Secure Cryptographic Engine (SCE) driver header
 *
 * @details
 * Minimal bringup API for the SCE block. Goal is to exercise the
 * register writes that a real AES-128 ECB encrypt would perform;
 * the actual cipher is delegated to the hardware on target and a
 * memcpy-based shim in simulator builds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_sce_regs.h"
#include "ra_err.h"

/**
 * @brief Reset the SCE control and command registers.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sce_init(void);

/**
 * @brief Encrypt one 16-byte block with AES-128 ECB.
 *
 * @details
 * Programmes the 16-byte key into the `KEY` window, the plaintext
 * block into `DIN`, kicks off the engine, and reads the ciphertext
 * back from `DOUT`. In `RA_SIMULATOR_MODE` the operation is a no-op
 * `memcpy` from `plaintext` into `ciphertext` so tests can exercise
 * the code path without a real crypto core.
 *
 * @param[in]  key        Pointer to 16-byte AES-128 key.
 * @param[in]  plaintext  Pointer to 16-byte input block.
 * @param[out] ciphertext Pointer to 16-byte output block.
 * @return `ra_err_t` error code.
 */
[[nodiscard]] ra_err_t
ra_sce_aes128_encrypt(const uint8_t* key, const uint8_t* plaintext, uint8_t* ciphertext);

#ifdef __cplusplus
}
#endif
