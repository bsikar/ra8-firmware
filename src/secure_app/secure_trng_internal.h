/**
 * @file secure_trng_internal.h
 * @brief Secure-side TRNG read API (RSIP-backed entropy)
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * The RSIP TRNG output registers live in the secure region and
 * cannot be reached directly from Non-Secure code. The veneer
 * ``ra8_nsc_trng_read`` lets NS request entropy without ever seeing
 * an RSIP register; this header is the secure-side bridge.
 *
 * The host implementation is a deterministic-but-decorrelated
 * xorshift64* PRNG. On the real chip the call drops through to
 * ``ra8_rsip_trng_read``. The split keeps the veneer code
 * unaware of which entropy source is wired in.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_secure_trng_limits_t
 * @brief Sizing constants for ``ra8_secure_trng_read``.
 */
typedef enum : uint16_t {
  k_ra8_secure_trng_max_bytes = 256U, /**< Max bytes per call. */
} ra8_secure_trng_limits_t;

/**
 * @brief Reset the TRNG seed (test-only; production reseeds from RSIP).
 *
 * @return ``ra8_err_t`` error code (currently always ``k_ra8_ok``).
 *
 * @pre Called from secure boot before any veneer can fire.
 *
 * @post Subsequent reads produce a deterministic stream rooted at
 *       the new seed.
 *
 * @note Thread safety: secure-world only, single-threaded init.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_secure_trng_reset(void);

/**
 * @brief Fill ``[out, out+len)`` with TRNG-quality entropy.
 *
 * @details
 * Pulls 8 bytes at a time from the entropy core and copies the
 * needed prefix into ``out``. The caller must ensure ``out`` is
 * already a *secure* buffer -- the NSC veneer copies bytes from
 * here back to the NS destination, so the secure-side function
 * itself never touches NS memory.
 *
 * @param[out] out Destination buffer (secure scratch).
 * @param[in]  len Bytes to fill, ``1..k_ra8_secure_trng_max_bytes``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bytes written.
 * @retval k_ra8_err_null_ptr      ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg   ``len`` zero or above the cap.
 *
 * @pre ``out`` non-NULL and points to ``len`` bytes of secure RAM.
 *
 * @post ``out[0..len-1]`` contains entropy bytes.
 *
 * @note Thread safety: not thread-safe; the seed is a single static.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t ra8_secure_trng_read(uint8_t* out, uint32_t len);

#ifdef __cplusplus
}
#endif
