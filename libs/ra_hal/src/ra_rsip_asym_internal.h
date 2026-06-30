/**
 * @file ra_rsip_asym_internal.h
 * @brief Cross-TU surface shared by the RSIP asymmetric + RSA split.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Not part of the public API. The asymmetric slice of the RSIP-E50D HAL
 * driver is split across two translation units:
 *
 * - ``ra_rsip_asym.c`` -- ECDSA sign / verify, ECDH key agreement,
 *   Ed25519 PureEdDSA, plus the OEM anti-rollback counter, wrapped-key
 *   vault, key wrap / unwrap, KDF, lifecycle / debug authorisation,
 *   tamper subsystem, and DOTF key routing;
 * - ``ra_rsip_rsa.c`` -- RSASSA sign / verify and RSAES encrypt /
 *   decrypt.
 *
 * Both translation units stream operand bytes through the engine's
 * asymmetric input / output lanes. The two packing helpers below are
 * defined in ``ra_rsip_asym.c`` and consumed by both TUs; their full
 * Doxygen contract lives here at the shared declaration site. See
 * CLAUDE.md "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_rsip.h"

/**
 * @brief Stream a byte buffer into an asymmetric input lane.
 *
 * @details
 * Packs whole little-endian 32-bit words into the register at ``off``
 * and zero-extends any trailing 1 .. 3 bytes into a final partial word.
 * Defined in ``ra_rsip_asym.c``; shared with the RSA entry points in
 * ``ra_rsip_rsa.c``.
 *
 * @param[in] off Register offset of the asymmetric input lane.
 * @param[in] buf Source buffer (>= ``len`` bytes); never NULL here.
 * @param[in] len Number of bytes to push (may be zero).
 *
 * @pre ``off`` is a valid ``ra_rsip_off_t`` input-lane offset.
 * @pre Either ``len`` is zero or ``buf`` is non-NULL.
 *
 * @post The engine has observed ``ceil(len / 4)`` word writes to ``off``.
 * @post No command-word side effect.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 */
void internal_asym_push(ra_rsip_off_t off, const uint8_t* buf, uint32_t len);

/**
 * @brief Pull a byte buffer back through an asymmetric output lane.
 *
 * @details
 * Inverse of ``internal_asym_push``: reads whole little-endian 32-bit
 * words from the register at ``off`` into ``buf`` and unpacks any
 * trailing 1 .. 3 bytes from a final partial word. Defined in
 * ``ra_rsip_asym.c``; shared with the RSA entry points in
 * ``ra_rsip_rsa.c``.
 *
 * @param[in]  off Register offset of the asymmetric output lane.
 * @param[out] buf Destination buffer (>= ``len`` bytes); never NULL here.
 * @param[in]  len Number of bytes to pull (may be zero).
 *
 * @pre ``off`` is a valid ``ra_rsip_off_t`` output-lane offset.
 * @pre ``buf`` points to at least ``len`` writable bytes.
 *
 * @post ``buf[0 .. len-1]`` reflect the engine output in little-endian order.
 * @post No state outside the destination buffer is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 */
void internal_asym_pull(ra_rsip_off_t off, uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif
