/**
 * @file ra_trng.h
 * @brief True Random Number Generator (TRNG) driver header
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_trng_regs.h"
#include "ra_err.h"

/**
 * @brief Enable the TRNG collector.
 *
 * @details
 * Clears the control/status registers and sets the `EN` bit in
 * `TRNGCR`. On the simulator this just programmes host RAM.
 *
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_trng_init(void);

/**
 * @brief Read a 32-bit random value.
 *
 * @details
 * On target, polls `TRNGSR.DRDY` and returns `TRNGDO`. In
 * `RA_SIMULATOR_MODE` the function returns a deterministic
 * pseudo-random value seeded from a static counter so host-side
 * tests are reproducible.
 *
 * @param[out] out Receives 32 random bits.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_trng_read(uint32_t* out);

#ifdef __cplusplus
}
#endif
