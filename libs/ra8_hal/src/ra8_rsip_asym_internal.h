/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_asym_internal.h
 * @brief Cross-TU surface shared by the RSIP asymmetric + RSA split.
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Not part of the public API. The asymmetric slice of the RSIP-E50D HAL
 * driver is split across two translation units:
 *
 * - ``ra8_rsip_asym.c`` -- the OEM anti-rollback counter, wrapped-key
 *   vault, key wrap / unwrap, KDF, lifecycle / debug authorisation,
 *   tamper subsystem, DOTF key routing, and the shared byte-lane +
 *   handle-tail helpers below;
 * - ``ra8_rsip_ecc.c`` -- ECDSA sign / verify, ECDH key agreement, and
 *   Ed25519 PureEdDSA (fail-closed in production; no RSIP backend);
 * - ``ra8_rsip_rsa.c`` -- RSASSA sign / verify and RSAES encrypt /
 *   decrypt (fail-closed in production; no RSIP backend).
 *
 * The ECC and RSA slices stream operand bytes through the engine's
 * asymmetric input / output lanes. The helpers below are defined in
 * ``ra8_rsip_asym.c`` and consumed by those TUs; their full Doxygen
 * contract lives here at the shared declaration site. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_rsip.h"

/**
 * @brief Stream a byte buffer into an asymmetric input lane.
 *
 * @details
 * Packs whole little-endian 32-bit words into the register at ``off``
 * and zero-extends any trailing 1 .. 3 bytes into a final partial word.
 * Defined in ``ra8_rsip_asym.c``; shared with the RSA entry points in
 * ``ra8_rsip_rsa.c``.
 *
 * @param[in] off Register offset of the asymmetric input lane.
 * @param[in] buf Source buffer (>= ``len`` bytes); never NULL here.
 * @param[in] len Number of bytes to push (may be zero).
 *
 * @pre ``off`` is a valid ``ra8_rsip_off_t`` input-lane offset.
 * @pre Either ``len`` is zero or ``buf`` is non-NULL.
 *
 * @post The engine has observed ``ceil(len / 4)`` word writes to ``off``.
 * @post No command-word side effect.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 */
RA8_PRIV void internal_asym_push(ra8_rsip_off_t off, const uint8_t* buf, uint32_t len);

/**
 * @brief Pull a byte buffer back through an asymmetric output lane.
 *
 * @details
 * Inverse of ``internal_asym_push``: reads whole little-endian 32-bit
 * words from the register at ``off`` into ``buf`` and unpacks any
 * trailing 1 .. 3 bytes from a final partial word. Defined in
 * ``ra8_rsip_asym.c``; shared with the RSA entry points in
 * ``ra8_rsip_rsa.c``.
 *
 * @param[in]  off Register offset of the asymmetric output lane.
 * @param[out] buf Destination buffer (>= ``len`` bytes); never NULL here.
 * @param[in]  len Number of bytes to pull (may be zero).
 *
 * @pre ``off`` is a valid ``ra8_rsip_off_t`` output-lane offset.
 * @pre ``buf`` points to at least ``len`` writable bytes.
 *
 * @post ``buf[0 .. len-1]`` reflect the engine output in little-endian order.
 * @post No state outside the destination buffer is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 */
RA8_PRIV void internal_asym_pull(ra8_rsip_off_t off, uint8_t* buf, uint32_t len);

/**
 * @brief Zero-fill the unused tail of a key-handle body buffer.
 *
 * @details
 * Several engine paths return a wrapped body shorter than the maximum body
 * capacity. To avoid leaking stale stack contents into the structure, callers
 * pad ``body[words .. max-1]`` with zeros. Centralised here and defined in
 * ``ra8_rsip_asym.c``; shared with the key wrap / unwrap + KDF paths there and
 * with the ECDH shared-secret pull in ``ra8_rsip_ecc.c``.
 *
 * @param[in,out] handle Handle whose ``body[]`` tail is wiped; never NULL here.
 * @param[in]     words  Number of words already populated.
 *
 * @pre ``handle`` is non-NULL.
 * @pre ``words`` <= ``k_ra8_rsip_handle_words_rsa4096_priv``.
 *
 * @post ``handle->body[w] == 0`` for all ``w`` in [``words``, max).
 * @post No other field is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 */
RA8_PRIV void internal_zero_handle_tail(ra8_rsip_key_handle_t* handle, uint32_t words);

#ifdef __cplusplus
}
#endif
